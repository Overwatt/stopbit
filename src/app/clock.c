#include "clock.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#ifndef CLOCK_HOST
#include "lwip/apps/sntp.h"
#endif

static uint32_t s_base, s_at;            /* unix seconds at tick s_at */
static bool s_known;

void clock_set(uint32_t unix_s)
{
    s_base = unix_s;
    s_at = HAL_GetTick();
    s_known = true;
}

bool clock_known(void) { return s_known; }
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

void clock_init(void)
{
#ifndef CLOCK_HOST
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_servermode_dhcp(1);
    sntp_setservername(1, "pool.ntp.org");
    sntp_init();
#endif
}
