#include "clock.h"
#include "slots.h"
#include "events.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#ifndef CLOCK_HOST
#include "lwip/apps/sntp.h"
#endif

static uint32_t s_base, s_at;            /* unix seconds at tick s_at */
static bool s_known, s_synced;           /* synced: by a time server, not carried over */
static char s_server[48];                /* from the settings */
#define CHECK 0xC10C5EEDu                /* the copy in the backup registers is whole */

static const char *from(void);
void clock_set(uint32_t unix_s)
{
    if (!s_synced) {
        const char *f = from();
        s_known = s_synced = true;      /* so the event has the time */
        s_base = unix_s; s_at = HAL_GetTick();
        events_add(EV_CLOCK, 0, 0, f[0] ? f : s_server[0] ? s_server : "a time server");
    }
    s_base = unix_s;
    s_at = HAL_GetTick();
    s_known = s_synced = true;
}

bool clock_known(void) { return s_known; }
bool clock_synced(void) { return s_synced; }
uint32_t clock_synced_ago(void) { return s_known ? (HAL_GetTick() - s_at) / 1000u : 0; }

uint32_t clock_unix(void)
{
    return s_known ? s_base + (HAL_GetTick() - s_at) / 1000u : 0;
}

#ifndef CLOCK_HOST
/* newlib's time() and gettimeofday() come here. */
int _gettimeofday(struct timeval *tv, void *tz);
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv) {
        tv->tv_sec = clock_unix();
        tv->tv_usec = s_known ? (suseconds_t)((HAL_GetTick() - s_at) % 1000u * 1000u) : 0;
    }
    return 0;
}

#endif

/* mbedTLS: timestamps that only need to count up. */
long long mbedtls_ms_time(void);
long long mbedtls_ms_time(void) { return (long long)HAL_GetTick(); }

int32_t clock_days(int y, int m, int d)              /* Howard Hinnant's days_from_civil */
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

bool clock_iso(char *out, bool date_only)
{
    time_t t = (time_t)clock_unix();
    struct tm tm;
    if (!s_known || !gmtime_r(&t, &tm)) return false;
    if (tm.tm_year > 8000) return false;
    if (date_only) snprintf(out, 9, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    else snprintf(out, 17, "%04d%02d%02dT%02d%02d%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

/* Each second, for the next start. Time first, then the check, so a copy cut short
 * by a reset does not match. */
void clock_poll(void)
{
    static uint32_t last;
    if (!s_known || HAL_GetTick() - last < 1000u) return;
    last = HAL_GetTick();
    uint32_t t = clock_unix();
    bkp_set(BKP_CLOCK, t);
    bkp_set(BKP_CLOCK + 1, t ^ CHECK);
}

#ifndef CLOCK_HOST
extern uint32_t g_reset_cause;
/* Behind by however long the restart took, a few seconds. Not after a power cut: the
 * registers may have outlasted it, by any length of time. */
static void carry(void)
{
    uint32_t t = bkp_get(BKP_CLOCK);
    if (g_reset_cause & (RCC_RSR_PORRSTF | RCC_RSR_BORRSTF)) return;
    if (bkp_get(BKP_CLOCK + 1) != (t ^ CHECK) || t < 1700000000u) return;
    s_base = t;
    s_at = HAL_GetTick();
    s_known = true;
}

/* A chosen server first, with pool.ntp.org behind it; else whatever DHCP names. */
static void servers(void)
{
    if (s_server[0]) {
        sntp_servermode_dhcp(0);
        sntp_setservername(0, s_server);
    } else {
        sntp_setservername(0, NULL);
        sntp_servermode_dhcp(1);
    }
    sntp_setservername(1, "pool.ntp.org");
}
#endif

void clock_server(const char *name)
{
    if (!strncmp(name, s_server, sizeof s_server)) return;
    snprintf(s_server, sizeof s_server, "%s", name);
#ifndef CLOCK_HOST
    if (sntp_enabled()) {                /* else clock_init() does it */
        sntp_stop();
        servers();
        sntp_init();
    }
#endif
}

/* The server that answered last. */
static const char *from(void)
{
#ifndef CLOCK_HOST
    for (int i = 0; i < SNTP_MAX_SERVERS; i++) {
        if (!(sntp_getreachability((u8_t)i) & 1u)) continue;
        const char *n = sntp_getservername((u8_t)i);
        if (n) return n;
        const ip_addr_t *a = sntp_getserver((u8_t)i);
        if (a) return ipaddr_ntoa(a);
    }
    return "";
#else
    return "this computer";
#endif
}

size_t clock_json(char *out, size_t max)
{
    char iso[17] = "";
    clock_iso(iso, false);
    int n = snprintf(out, max, "{\"now\":\"%s\",\"src\":\"%s\",\"ago\":%ld,\"from\":\"%s\"}", iso,
                     s_synced ? "ntp" : s_known ? "restart" : "none", s_synced ? (long)clock_synced_ago() : -1L, from());
    return n > 0 && (size_t)n < max ? (size_t)n : 0;
}

void clock_init(void)
{
#ifndef CLOCK_HOST
    carry();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    servers();
    sntp_init();
#endif
}
