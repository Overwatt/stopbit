/*
 * bridge.h — RS485 <-> network: one TCP server, TCP client or UDP endpoint
 * per port. Everything runs from the main loop.
 */
#ifndef BRIDGE_H
#define BRIDGE_H
#include "cfg.h"

#define MAX_CLIENTS 4

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
} bridge_info_t;
void bridge_info(int port, bridge_info_t *out);
/* Who the access list turned away lately, as JSON. 0 if it did not fit. */
size_t bridge_access_json(int port, char *out, size_t max);
/* Sends bytes on a port's serial line as if a client had. */
bool bridge_send(int port, const uint8_t *d, size_t n);
/* JSON array of per-port live state. Returns length, or 0 if it did not fit. */
size_t bridge_json(char *out, size_t max);
#endif
