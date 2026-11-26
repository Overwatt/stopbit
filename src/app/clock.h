/*
 * clock.h — The time of day, from SNTP: the server set in the settings, else the
 * DHCP server's, then pool.ntp.org. Carried across a restart in backup registers
 * (not across a power cut), so known from the start, to within a few seconds, until
 * the first answer. There is no watch crystal for the RTC.
 */
#ifndef CLOCK_H
#define CLOCK_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
void     clock_init(void);           /* after the network */
void     clock_poll(void);           /* main loop: keeps a copy for the next start */
void     clock_server(const char *name);  /* from the settings; blank = DHCP's, then pool.ntp.org */
bool     clock_known(void);
bool     clock_synced(void);         /* set by a time server since the start, not carried over */
size_t   clock_json(char *out, size_t max);   /* {"now","src","ago","from"} */
uint32_t clock_unix(void);           /* seconds since 1970, UTC; 0 if unknown */
uint32_t clock_synced_ago(void);     /* seconds since the last answer */
void     clock_set(uint32_t unix_s); /* from SNTP */
/* "20261125T174000Z", or "20261125" with date_only. False if unknown. */
bool     clock_iso(char *out, bool date_only);
/* Days since 1970 for a calendar date, and back. For certificates and signatures. */
int32_t  clock_days(int y, int m, int d);
#endif
