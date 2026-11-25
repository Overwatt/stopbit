#include "traffic.h"
#include <string.h>
#ifndef D2_BSS
#define D2_BSS __attribute__((section(".d2_bss")))
#endif
static D2_BSS traffic_t s_t[PORTS][TRAFFIC_N];
static uint32_t s_n[PORTS];

void traffic_init(void) { memset(s_n, 0, sizeof s_n); }
void traffic_clear(int port) { s_n[port] = 0; }

void traffic_add(int port, bool tx, const uint8_t *d, size_t n, uint32_t ms)
{
    if (!n) return;
    traffic_t *t = &s_t[port][s_n[port]++ % TRAFFIC_N];
    t->ms = ms;
    t->len = (uint16_t)n;
    t->tx = tx;
    memcpy(t->data, d, n < TRAFFIC_DATA ? n : TRAFFIC_DATA);
}

bool traffic_get(int port, int k, traffic_t *out)
{
    if (k < 0 || k >= TRAFFIC_N || (uint32_t)k >= s_n[port]) return false;
    *out = s_t[port][(s_n[port] - 1 - (uint32_t)k) % TRAFFIC_N];
    return true;
}
