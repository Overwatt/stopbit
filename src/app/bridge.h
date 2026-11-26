/*
 * bridge.h — RS485 <-> network: one TCP server, TCP client or UDP endpoint
 * per port. Everything runs from the main loop.
 */
#ifndef BRIDGE_H
#define BRIDGE_H
#include "cfg.h"
#include "modbus.h"

#define MAX_CLIENTS 4

/* How long a packet heard on the line waited before going out on the network: from its
 * last byte (the silence that ends it included) to its sending. Counts per bucket, the
 * last one for longer than any bound. */
#define FWD_BUCKETS   7
#define FWD_BOUNDS_US { 1000, 2000, 5000, 10000, 20000, 50000, 100000 }
typedef struct { uint32_t n[FWD_BUCKETS + 1]; uint64_t sum_us; uint32_t last_us, max_us; } fwd_t;

void bridge_init(void);
/* (Re)starts only the ports whose settings changed. Applied on the next poll. */
void bridge_configure(const cfg_t *c);
void bridge_poll(void);
/* Only moves data that has already arrived, between ports and open connections. For
 * use while something else holds up the main loop, even inside a network callback. */
void bridge_pump(void);
typedef struct {
    const char *state;
    int clients;
    uint32_t serial_rx, serial_tx, errors;
    uint32_t net_lost, refused;
    char peers[112];
    const char *alert;          /* "", "silent", "no_client" or "turned_away" */
    uint32_t denied, ignored;   /* by the access list: connections or datagrams; bytes */
    char denied_ip[16];
    fwd_t fwd;
    uint32_t net_rx, net_tx;
    int32_t heard_ms;           /* -1: nothing heard yet */
    bool up;                    /* listening, connected or ready */
} bridge_info_t;
/* A Modbus gateway port's figures and how many wait; NULL for other ports. */
const mb_stats_t *bridge_modbus(int port, uint8_t *waiting);
void bridge_info(int port, bridge_info_t *out);
/* Who the access list turned away lately, as JSON. 0 if it did not fit. */
size_t bridge_access_json(int port, char *out, size_t max);
/* Sends bytes on a port's serial line as if a client had. */
bool bridge_send(int port, const uint8_t *d, size_t n);
/* JSON array of per-port live state. Returns length, or 0 if it did not fit. */
size_t bridge_json(char *out, size_t max);
/* The shared network-to-line rings: in use now, most at once, how many, and how
 * often data had to wait for one. */
void bridge_rings(uint32_t *used, uint32_t *peak, uint32_t *size, uint32_t *waits);
#endif
