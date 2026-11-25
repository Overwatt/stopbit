/* cc -DD2_BSS= -Isrc/app tests/test_history.c src/app/history.c -o /tmp/h && /tmp/h */
#include "history.h"
#include "cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const uint32_t start[] = { 0, 77, 0xFFFFFFFFu - 12000u };      /* tick wrap too */
    char b[4344];
    for (int s = 0; s < 3; s++) {
        hist_count_t t[PORTS] = { 0 };
        uint32_t now = start[s];
        history_init(now);
        assert(history_json(0, b, sizeof b) && !strcmp(b, "{\"step_s\":5,\"rx\":[],\"tx\":[],\"err\":[]}\n"));
        for (int ms = 0; ms < 15000; ms += 10) {
            t[0].rx += 10; if (ms == 7000) { t[0].err = 3; t[1].tx = 9; }
            now += 10;
            history_poll(now, t, PORTS);
        }
        assert(history_json(0, b, sizeof b));
        assert(!strcmp(b, "{\"step_s\":5,\"rx\":[5000,5000,5000],\"tx\":[0,0,0],\"err\":[0,3,0]}\n"));
        assert(history_json(1, b, sizeof b) && strstr(b, "\"tx\":[0,9,0]"));
        assert(history_errors(0) == 3 && history_errors(1) == 0);
        for (int k = 0; k < 118; k++) { now += HIST_STEP_MS; t[0].rx += 1; history_poll(now, t, PORTS); }
        assert(history_errors(0) == 3);                                  /* still inside the window */
        now += HIST_STEP_MS; history_poll(now, t, PORTS);
        assert(history_errors(0) == 0);                                  /* aged out */
        now += 60000; t[0].rx += 7; history_poll(now, t, PORTS);         /* held up: one sample, no burst */
        history_poll(now + 1, t, PORTS);
        assert(history_json(0, b, sizeof b) && strstr(b, ",0,7],\"tx\""));
        t[0].err = 5000000; now += HIST_STEP_MS; history_poll(now, t, PORTS);
        assert(history_errors(0) == 0xFFFF);                             /* saturates */
    }
    hist_count_t t[PORTS] = { 0 };                                       /* longest possible reply fits */
    uint32_t now = 0;
    history_init(0);
    for (uint32_t k = 0; k < HIST_N; k++) {
        t[0].rx += 99999999u; t[0].tx += 99999999u; t[0].err += 70000;
        now += HIST_STEP_MS; history_poll(now, t, PORTS);
    }
    size_t n = history_json(0, b, sizeof b);
    assert(n && n < sizeof b);
    assert(history_json(0, b, 100) == 0);
    printf("ok, longest reply %zu bytes\n", n);
    return 0;
}
