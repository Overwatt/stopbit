#include "history.h"
#include "cfg.h"
#include <stdio.h>
#include <string.h>

#ifndef D2_BSS
#define D2_BSS __attribute__((section(".d2_bss")))
#endif

typedef struct { uint32_t rx[HIST_N], tx[HIST_N]; uint16_t err[HIST_N]; } hist_t;
static D2_BSS hist_t s_h[PORTS];
static hist_count_t s_last[PORTS];
static uint32_t s_next, s_count, s_head;          /* head: where the next sample goes */

void history_init(uint32_t now)
{
    memset(s_h, 0, sizeof s_h);                   /* D2 SRAM is not zeroed at startup */
    memset(s_last, 0, sizeof s_last);
    s_count = s_head = 0;
    s_next = now + HIST_STEP_MS;
}

void history_poll(uint32_t now, const hist_count_t *t, int ports)
{
    if ((int32_t)(now - s_next) < 0) return;
    s_next += HIST_STEP_MS;
    if ((int32_t)(now - s_next) >= 0) s_next = now + HIST_STEP_MS;   /* was held up */
    for (int i = 0; i < ports && i < PORTS; i++) {
        uint32_t e = t[i].err - s_last[i].err;
        s_h[i].rx[s_head] = t[i].rx - s_last[i].rx;
        s_h[i].tx[s_head] = t[i].tx - s_last[i].tx;
        s_h[i].err[s_head] = e > 0xFFFFu ? 0xFFFFu : (uint16_t)e;
        s_last[i] = t[i];
    }
    s_head = (s_head + 1) % HIST_N;
    if (s_count < HIST_N) s_count++;
}

uint32_t history_errors(int p)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < HIST_N; k++) n += s_h[p].err[k];
    return n;
}

size_t history_json(int p, char *out, size_t max)
{
    size_t n = 0;
#define ADD(...) do { int w = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= max - n) { return 0; } n += (size_t)w; } while (0)
    if (!max) return 0;
    uint32_t first = (s_head + HIST_N - s_count) % HIST_N;
    ADD("{\"step_s\":%u", HIST_STEP_MS / 1000u);
    for (int a = 0; a < 3; a++) {
        ADD(",\"%s\":[", a == 0 ? "rx" : a == 1 ? "tx" : "err");
        for (uint32_t k = 0; k < s_count; k++) {
            uint32_t j = (first + k) % HIST_N;
            ADD("%s%lu", k ? "," : "", (unsigned long)(a == 0 ? s_h[p].rx[j] : a == 1 ? s_h[p].tx[j] : s_h[p].err[j]));
        }
        ADD("]");
    }
    ADD("}\n");
    return n;
#undef ADD
}
