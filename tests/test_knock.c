/* cc -Isrc/app tests/test_knock.c src/app/knock.c -o /tmp/k && /tmp/k */
#include "knock.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static knocks_t S;
static int n(uint32_t now) { int c = 0; while (c < 20 && knock_get(&S, c)) c++; return c; }

int main(void)
{
    assert(n(0) == 0 && !knock_persistent(&S, 0));
    /* address 0 and time 0 are ordinary */
    knock_add(&S, 0, 0);
    assert(n(0) == 1 && knock_get(&S, 0)->ip == 0 && knock_get(&S, 0)->count == 1);
    memset(&S, 0, sizeof S);

    /* a scan is one or two tries; a client keeps on */
    knock_add(&S, 7, 1000); knock_add(&S, 7, 2000);
    assert(!knock_persistent(&S, 2000));
    knock_add(&S, 7, 3000);
    assert(knock_persistent(&S, 3000) && n(3000) == 1 && knock_get(&S, 0)->count == 3);
    assert(knock_persistent(&S, 3000 + KNOCK_RECENT - 1) && !knock_persistent(&S, 3000 + KNOCK_RECENT));
    /* coming back after a long time starts again, but the count goes on */
    knock_add(&S, 7, 3000 + KNOCK_RECENT);
    assert(!knock_persistent(&S, 3000 + KNOCK_RECENT) && knock_get(&S, 0)->count == 4);
    /* different addresses do not add up to one */
    memset(&S, 0, sizeof S);
    for (uint32_t i = 1; i <= 8; i++) { knock_add(&S, i, 100 * i); knock_add(&S, i, 100 * i + 1); }
    assert(n(900) == 8 && !knock_persistent(&S, 900));
    for (int i = 0; i < 8; i++) assert(knock_get(&S, i)->ip == 8u - (uint32_t)i);      /* newest first */
    /* the ninth takes the oldest's place, and only that */
    knock_add(&S, 1, 900);
    knock_add(&S, 9, 901);
    assert(n(901) == 8 && knock_get(&S, 0)->ip == 9 && knock_get(&S, 0)->count == 1 && knock_get(&S, 1)->ip == 1 && knock_get(&S, 1)->count == 3);
    for (int i = 0; i < 8; i++) assert(knock_get(&S, i)->ip != 2);
    /* forgetting one leaves the rest in order */
    for (int i = 0; i < KNOCK_N; i++) if (S.k[i].ip == 1) knock_forget(&S, i);
    assert(n(901) == 7 && !knock_persistent(&S, 901));
    knock_add(&S, 50, 902);
    assert(n(902) == 8);

    /* two in the same millisecond: the later one is the latest */
    memset(&S, 0, sizeof S);
    knock_add(&S, 1, 5); knock_add(&S, 2, 5); knock_add(&S, 1, 5);
    assert(knock_get(&S, 0)->ip == 1 && knock_get(&S, 1)->ip == 2);

    /* across the tick wrapping round, and never "just now" after 49 days */
    memset(&S, 0, sizeof S);
    uint32_t t = 0xFFFFFF00u;
    for (int i = 0; i < 3; i++) knock_add(&S, 5, t += 100);
    assert(t < 1000 && knock_persistent(&S, t));
    for (uint32_t h = 0; h < 24 * 120; h++) { t += 3600000u; knock_clamp(&S, t); if (h) assert(!knock_persistent(&S, t)); }

    /* against a plain model */
    srand(1);
    memset(&S, 0, sizeof S);
    struct { uint32_t ip, count, at; } M[KNOCK_N]; int m = 0;
    for (uint32_t now = 1; now < 200000; now++) {
        uint32_t ip = 1 + (uint32_t)rand() % 12;
        knock_add(&S, ip, now);
        int f = -1, o = 0;
        for (int i = 0; i < m; i++) { if (M[i].ip == ip) f = i; if (M[i].at < M[o].at) o = i; }
        if (f < 0) { f = m < KNOCK_N ? m++ : o; M[f].ip = ip; M[f].count = 0; }
        M[f].count++; M[f].at = now;
        const knock_t *k = knock_get(&S, 0);
        assert(k->ip == ip && k->count == M[f].count && n(now) == m);
    }
    puts("ok");
    return 0;
}
