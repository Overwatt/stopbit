/*
 * clock.h — The time of day, from SNTP: the DHCP server's time servers if it names
 * any, else pool.ntp.org. Unknown until the first answer; nothing is stored.
 */
#ifndef CLOCK_H
#define CLOCK_H
#include <stdint.h>
#include <stdbool.h>
void     clock_init(void);           /* after the network */
bool     clock_known(void);
uint32_t clock_unix(void);           /* seconds since 1970, UTC; 0 if unknown */
uint32_t clock_synced_ago(void);     /* seconds since the last answer */
void     clock_set(uint32_t unix_s); /* from SNTP */
/* "20261125T174000Z", or "20261125" with date_only. False if unknown. */
bool     clock_iso(char *out, bool date_only);
/* Days since 1970 for a calendar date, and back. For certificates and signatures. */
int32_t  clock_days(int y, int m, int d);
#endif
