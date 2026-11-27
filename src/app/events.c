#include "events.h"
#include "clock.h"
#include "cfg.h"
#include "extflash.h"
#include "slots.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#define DEV     1
#define BASE    0x0FC000u               /* four sectors, to 0x100000 */
#define PER     64u                     /* records in a sector */
#define N       (4u * PER)
#define EMPTY   0xFFFFFFFFu
#define BURST   40u                     /* written at once, before rationing */
#define REFILL  90000u                  /* ms for one more */

typedef struct {
    uint32_t seq;                       /* written last: a record cut short stays EMPTY */
    uint32_t t;                         /* unix seconds; 0 if the clock was not known */
    uint32_t up;                        /* seconds since the start */
    uint32_t a;
    uint32_t check;                     /* over the rest: an erase cut short leaves garbage, not blanks */
    uint16_t boot;
    uint8_t  kind, port;
    char     s[40];
} rec_t;
_Static_assert(sizeof(rec_t) == 64 && PER * sizeof(rec_t) == 4096, "a sector of records");

static uint32_t s_seqs[N];              /* each slot's seq, as in flash */
/* The newest records, so the log can be shown without reading the flash (about 3 ms for
 * a page of them). Slot i is kept at i % RECENT; it is good while its seq is the slot's. */
#define RECENT  32u
static rec_t    s_recent[RECENT];
static uint32_t s_w, s_next = 1;        /* the slot written next; its seq */
static rec_t    s_q[16];                /* queued, not yet written */
static uint32_t s_qn, s_dropped, s_tokens = BURST, s_refill_at;
static bool     s_ready;

static uint32_t addr(uint32_t i) { return BASE + i * (uint32_t)sizeof(rec_t); }

static uint32_t sum(const rec_t *r)
{
    rec_t c = *r;
    c.check = 0;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof c; i++) h = (h ^ ((const uint8_t *)&c)[i]) * 16777619u;   /* seq too: it may be cut short */
    return h;
}

static bool valid(const rec_t *r) { return r->seq != EMPTY && r->check == sum(r); }

void events_init(void)
{
    uint32_t best = 0;
    bool any = false;
    memset(s_recent, 0, sizeof s_recent);
    for (uint32_t i = 0; i < N; i++) {
        rec_t r;
        extflash_read(DEV, addr(i), &r, sizeof r);
        s_seqs[i] = valid(&r) ? r.seq : EMPTY;
        if (s_seqs[i] != EMPTY) s_recent[i % RECENT] = r;
        if (s_seqs[i] != EMPTY && (!any || s_seqs[i] > s_seqs[best])) { best = i; any = true; }
    }
    s_w = any ? (best + 1) % N : 0;
    s_next = any ? s_seqs[best] + 1 : 1;
    s_qn = s_dropped = 0;                   /* as at power-on */
    s_tokens = BURST;
    s_refill_at = HAL_GetTick() + REFILL;
    s_ready = true;
}

void events_add(ev_kind_t k, int port, uint32_t a, const char *s)
{
    if (s_qn == sizeof s_q / sizeof s_q[0]) { s_dropped++; return; }
    rec_t *r = &s_q[s_qn++];
    memset(r, 0, sizeof *r);
    r->t = clock_known() ? clock_unix() : 0;
    r->up = HAL_GetTick() / 1000u;
    r->a = a;
    r->boot = (uint16_t)bkp_get(BKP_BOOTS);
    r->kind = (uint8_t)k;
    r->port = (uint8_t)port;
    if (s) snprintf(r->s, sizeof r->s, "%s", s);
}

/* In one read, so a whole sector lets the ports run between pieces (extflash.c). The
 * buffer is filled before it is looked at, so it needs no zeroed memory. */
#if defined(__arm__)
#define COLD __attribute__((section(".itcm_bss")))
#else
#define COLD
#endif
static bool blank(uint32_t a, uint32_t n)
{
    static COLD uint8_t b[4096];
    extflash_read(DEV, a, b, n);
    for (uint32_t i = 0; i < n; i++) if (b[i] != 0xFF) return false;
    return true;
}

/* Into the next slot: the sector it starts is erased first, losing the oldest 64. A
 * slot a power cut left half-written is passed over. */
static bool put(rec_t *r)
{
    for (int tries = 0; tries < 4; tries++) {
        if (s_w % PER == 0 && !blank(addr(s_w), PER * sizeof(rec_t))) {
            if (!extflash_erase(DEV, addr(s_w), 4096)) return false;
            for (uint32_t i = 0; i < PER; i++) s_seqs[s_w + i] = EMPTY;
        }
        if (!blank(addr(s_w), sizeof(rec_t))) { s_w = (s_w + 1) % N; continue; }
        r->seq = s_next;
        r->check = sum(r);
        if (!extflash_write(DEV, addr(s_w) + 4, (const uint8_t *)r + 4, sizeof *r - 4) ||
            !extflash_write(DEV, addr(s_w), &r->seq, 4)) return false;
        s_recent[s_w % RECENT] = *r;
        s_seqs[s_w] = s_next++;
        s_w = (s_w + 1) % N;
        return true;
    }
    return false;
}

void events_poll(void)
{
    if (!s_ready) return;
    if (s_tokens < BURST && (int32_t)(HAL_GetTick() - s_refill_at) >= 0) {
        s_tokens++;
        s_refill_at = HAL_GetTick() + REFILL;
    }
    while (s_qn) {
        if (!s_tokens) { s_dropped += s_qn; s_qn = 0; return; }
        if (s_dropped) {                /* how many were lost, just before the next kept: no token of its own */
            rec_t d = s_q[0];
            memset(d.s, 0, sizeof d.s);
            d.kind = EV_DROPPED;
            d.port = 0;
            d.a = s_dropped;
            if (!put(&d)) return;
            s_dropped = 0;
        }
        if (!put(&s_q[0])) return;      /* the flash is not answering: try next pass */
        s_tokens--;
        memmove(s_q, s_q + 1, --s_qn * sizeof s_q[0]);
    }
}

void events_flush(void)
{
    s_tokens += s_qn + 1;               /* what is queued goes, whatever the ration */
    events_poll();
}

size_t events_json(uint32_t before, int max_n, char *out, size_t max)
{
    size_t n = 0;
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) { return 0; } n += (size_t)w_; } while (0)
    OUT("{\"boot\":%u,\"events\":[", (unsigned)(uint16_t)bkp_get(BKP_BOOTS));
    int k = 0;
    bool more = false;
    for (uint32_t back = 1; back <= N; back++) {        /* newest first: back from the end */
        uint32_t i = (s_w + N - back) % N, q = s_seqs[i];
        if (q == EMPTY || (before && q >= before)) continue;
        if (k == max_n) { more = true; break; }
        rec_t r = s_recent[i % RECENT];
        if (r.seq != q) {                                    /* not kept: from the flash */
            extflash_read(DEV, addr(i), &r, sizeof r);
            if (r.seq != q || !valid(&r)) continue;         /* changed under us */
            s_recent[i % RECENT] = r;
        }
        r.s[sizeof r.s - 1] = 0;
        OUT("%s[%lu,%lu,%lu,%u,%u,%u,%lu,", k++ ? "," : "", (unsigned long)r.seq, (unsigned long)r.t,
            (unsigned long)r.up, r.boot, r.kind, r.port, (unsigned long)r.a);
        size_t w = json_str(r.s, out + n, max - n);
        if (!w) return 0;
        n += w;
        OUT("]");
    }
    OUT("],\"more\":%s}\n", more ? "true" : "false");
    return n;
#undef OUT
}
