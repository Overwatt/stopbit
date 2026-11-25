/* quiet.h — "Alert if quiet". Pure logic, tested on the host. */
#ifndef QUIET_H
#define QUIET_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { QUIET_OK, QUIET_SILENT, QUIET_NO_CLIENT, QUIET_TURNED_AWAY } quiet_t;

/* Both times start at the moment the port opened, so a port gets the whole limit
 * before it can alert. limit_min 0 = never. */
static inline quiet_t quiet_eval(uint32_t now, uint32_t limit_min, uint32_t heard_at, uint32_t client_at)
{
    uint32_t ms = limit_min * 60000u;
    if (!limit_min) return QUIET_OK;
    if (now - heard_at >= ms) return QUIET_SILENT;
    if (now - client_at >= ms) return QUIET_NO_CLIENT;
    return QUIET_OK;
}

/* Keeps an old time from wrapping round to "just now" (the tick wraps at 49 days). */
static inline uint32_t quiet_clamp(uint32_t now, uint32_t at)
{
    return now - at > 0x70000000u ? now - 0x70000000u : at;
}
#endif
