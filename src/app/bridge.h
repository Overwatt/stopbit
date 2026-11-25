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
void bridge_configure(const port_cfg_t cfg[PORTS]);
void bridge_poll(void);
typedef struct {
    const char *state;
    int clients;
    uint32_t serial_rx, serial_tx, errors;
} bridge_info_t;
void bridge_info(int port, bridge_info_t *out);
/* JSON array of per-port live state. Returns length, or 0 if it did not fit. */
size_t bridge_json(char *out, size_t max);
#endif
