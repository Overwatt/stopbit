/* addrs.h — the gateway's extra addresses: each port's own address (portN.ip) that is not
 * the main one, kept by src/ipalias; their events, and their state for /api/state. */
#ifndef ADDRS_H
#define ADDRS_H
#include "cfg.h"

void addrs_init(void);                          /* after net_init */
/* Each port's address, network byte order; 0 blank (bridge_configure). Taken up by
 * addrs_poll. */
void addrs_want(const uint32_t ip[PORTS]);
/* Main loop, after net_poll: the list again if it or the main address has changed. */
void addrs_poll(void);
/* "addresses":[{"ip":"…","state":"checking|live|conflict|outside"},…]; 0 if it does not fit. */
size_t addrs_json(char *out, size_t max);
#endif
