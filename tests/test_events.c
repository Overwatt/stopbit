/* The event log: order, paging, wrapping, rationing, and a power cut at every flash
 * operation, which must leave no half-written record and the numbering going on. */
#include "events.h"
#include "slots.h"
#include "stm32h7xx_hal.h"
#include "flash_sim.h"
#include <assert.h>
#include <stdio.h>

void NVIC_SystemReset(void) { abort(); }
t_dwt_t t_dwt;
uint32_t t_dwt_now(void) { return t_now * 400000u; }
bool clock_known(void) { return true; }
uint32_t clock_unix(void) { return 1795600000u + t_now / 1000u; }

static char J[16384];
static int count(const char *j) { int n = 0; for (const char *p = j; (p = strstr(p, "],[")); p++) n++; return strstr(j, "\"events\":[[") ? n + 1 : 0; }
static unsigned long newest(void) { events_json(0, 1, J, sizeof J); const char *p = strstr(J, "[["); return p ? strtoul(p + 2, NULL, 10) : 0; }

int main(void)
{
    f_no[f_nno++] = (range_t){ 1, 0, 0x0FC000 };              /* only its own four sectors */
    f_no[f_nno++] = (range_t){ 0, 0, 0x200000 };
    memset(F, 0xFF, sizeof F);
    events_init();
    assert(events_json(0, 30, J, sizeof J) && strstr(J, "\"events\":[]") && strstr(J, "\"more\":false"));

    events_add(EV_START, 0, 0x1460000u, "1019aa2");
    events_add(EV_TURNED_AWAY, 1, 0x0A00000Au, NULL);
    events_add(EV_SETTINGS, 0, 3, "mqtt,\"x\"\\");                /* escaped in the JSON */
    events_poll();
    assert(events_json(0, 30, J, sizeof J) && count(J) == 3);
    assert(strstr(J, "[[3,") && strstr(J, "\"mqtt,\\\"x\\\"\\\\\"") && strstr(J, ",\"1019aa2\"]]"));
    events_init();                                               /* a restart finds the end */
    events_add(EV_CLOCK, 0, 0, "pool.ntp.org"); events_poll();
    assert(newest() == 4);

    /* the ration: 40 at once, then the rest counted, and the count kept */
    for (int i = 0; i < 60; i++) { events_add(EV_AUTH_FAIL, 0, (uint32_t)i, NULL); events_poll(); }
    assert(newest() == 43);                                      /* 4, then 39 left of a fresh ration */
    t_now += 90000; events_add(EV_CLOCK, 0, 0, "x"); events_poll();
    assert(events_json(0, 2, J, sizeof J) && strstr(J, ",19,0,21,") && strstr(J, "\"x\"]") && strstr(J, "\"more\":true"));   /* 21 lost, then the event */

    /* paging: every event exactly once, newest first */
    for (int i = 0; i < 400; i++) { t_now += 90000; events_add(EV_ALERT, 2, (uint32_t)i, "silent"); events_poll(); }
    assert(events_json(0, 1, J, sizeof J) && strstr(J, ",10,2,399,\"silent\"]"));    /* a trickle is all kept */
    unsigned long before = 0, seen = 0, last = 0;
    for (;;) {
        assert(events_json((uint32_t)before, 30, J, sizeof J));
        for (const char *p = strstr(J, "[["); p; p = strstr(p + 1, "],[")) {
            unsigned long q = strtoul(p + (p[1] == '[' ? 2 : 3), NULL, 10);
            if (last && q != last - 1) { printf("gap: %lu after %lu\n%.600s\n", q, last, J); abort(); }
            last = q; seen++;
        }
        if (!strstr(J, "\"more\":true")) break;
        before = last;
    }
    assert(seen >= 192 && seen <= 256);                          /* a sector's worth goes at each wrap */
    printf("%lu kept of %lu, newest %lu\n", seen, newest(), newest());

    /* a power cut during each flash operation of a write, at a sector boundary and not */
    for (int at = 0; at < 2; at++) {
        while ((newest() % 64 == 63) != at) { t_now += 90000; events_add(EV_ALERT, 1, 0, ""); events_poll(); }
        static uint8_t img[0x4000];
        memcpy(img, F[1] + 0x0FC000, sizeof img);
        events_init();
        unsigned long was = newest();
        t_now += 90000; events_add(EV_RENEWAL, 0, 0, "Renewed");
        f_ops = 0; events_poll(); long total = f_ops;
        for (long k = 0; k < total; k++) {
            memcpy(F[1] + 0x0FC000, img, sizeof img);
            events_init();
            t_now += 90000; events_add(EV_RENEWAL, 0, 0, "Renewed");
            f_ops = 0; f_cut = k;
            if (!setjmp(f_power)) { events_poll(); assert(!"cut"); }
            f_cut = -1;
            events_init();                                       /* the restart after the cut */
            unsigned long n = newest();
            if (n != was && n != was + 1) { printf("at %d cut %ld of %ld: newest %lu, was %lu\n", at, k, total, n, was); abort(); }
            assert(events_json(0, 30, J, sizeof J));
            if (n == was + 1) assert(strstr(J, "\"Renewed\"]"));
            t_now += 90000; events_add(EV_CLOCK, 0, 0, "after"); events_poll();
            assert(newest() == n + 1);                            /* and it goes on */
        }
        printf("power cut at each of %ld operations: the log holds\n", total);
    }
    puts("ok");
    return 0;
}
