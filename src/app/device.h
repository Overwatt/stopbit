/*
 * device.h — Devices on the RS485 ports, polled for their profiles' values. Each port's
 * devices take turns in its Modbus queue (one request of ours at a time); a reply is
 * decoded into the device's values; misses make blocks stale and devices offline.
 */
#ifndef DEVICE_H
#define DEVICE_H
#include "cfg.h"
#include "modbus.h"
#include "pfdecode.h"

typedef struct {
    bool       used;
    uint8_t    gen, slot;          /* gen: bumps when reconfigured (replies to the old one are ignored) */
    const profile_t *p;            /* NULL: not polled; see error */
    dev_cfg_t  cfg;
    pf_state_t st;
    uint32_t   due[PF_BLOCKS];     /* ms: each block's next read */
    uint8_t    miss[PF_BLOCKS];    /* in a row */
    uint16_t   have, stale;        /* blocks with values now; blocks whose values were dropped */
    bool       online;             /* a block not marked info has values */
    uint32_t   seq;                /* +1 on any change of values, online or error */
    uint32_t   polls, misses, last_ok;
    char       why[48];            /* the last miss */
    char       error[72];          /* why it is not polled */
} device_t;

void  device_init(void);
void  device_configure(const cfg_t *c);   /* devices whose settings changed start afresh */
void  device_poll(void);                  /* main loop */
/* A request of ours ended (modbus.c's io.local, through bridge.c). */
void  device_reply(int port, uint32_t tag, int status, const uint8_t *pdu, size_t n);
/* A command for one of a device's controls (over MQTT): queued to be written (true), or
 * refused, with an event saying why. retained: the broker kept it, so it is stale. */
bool  device_command(const char *name, const char *control, const char *payload, bool retained);
const device_t *device_get(int i);        /* NULL: no device i */
uint16_t device_line_use(int port);       /* the line's use by our requests, permille, last minute */
void  device_count(int up[PORTS], int all[PORTS]);   /* each port's devices, and how many are online */
/* Field f's value as JSON: a number, true/false, a string, or null if not known. 0 if it does not fit. */
size_t device_value(const device_t *d, int f, char *out, size_t max);
#define DEV_PARTS 3                 /* /api/devices gives a device in this many parts */
/* Part k of device i's JSON: 0 its settings and state and values 0 to 39; 1 values 40 to 79;
 * 2 the rest, and the end. -1 if it does not fit. Part 0 always opens "values", part 2 always
 * closes it, so parts need no shared state: a device without a profile has "values":[], and
 * one gone since part 0 gives only the closing "]}" in part 2. */
int   device_json_part(int i, int k, char *out, size_t max);
size_t device_json(int i, char *out, size_t max);   /* all of it; 0 if it does not fit */
/* Told before device i is removed or changed (devmqtt.c, to clear it from HA). It may
 * hold the device's profile meanwhile, so that it stays loaded. */
void  device_on_gone(void (*fn)(int i, const device_t *d));
void  device_hold(const profile_t *p, int delta);
/* Implemented by bridge.c: port's Modbus state, or NULL if it is off or not a gateway. */
modbus_t *bridge_modbus_port(int port);

#endif
