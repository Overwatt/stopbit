/*
 * device.c — Devices polled for their profiles' values (device.h). Per port: which block
 * of which device is most overdue goes next, as one request in the port's Modbus queue
 * (modbus.c's local client); its reply is decoded (pfdecode.c). Profiles are parsed once
 * and shared by the devices that use them: parsed in one place with every table at its
 * largest, then packed into s_room at the size each needs. Commands for controls are
 * written before polls: the control's block read (as a poll reads it), written if that
 * changes it, the block read back.
 */
#include "device.h"
#include "events.h"
#include "pfwrite.h"
#include "profiles.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#ifndef D2_BSS
#define D2_BSS __attribute__((section(".d2_bss")))
#endif
#if !defined(D3_BSS) && defined(__arm__)
#define D3_BSS __attribute__((section(".d3_bss")))
#elif !defined(D3_BSS)
#define D3_BSS
#endif
#define LOADED  4                /* different profiles at once, room allowing */
#ifndef PF_ROOM
#define PF_ROOM 24576u           /* for all of them packed: the largest there can be is 21 KB, the EG4's is 5 */
#endif
#define LOST_MS 90000u           /* a request whose end never came: the port is free again (MB_QUEUE x the longest timeout, 10 s, + 10 s) */
#define WRITES       16          /* commands waiting, for all devices */
#define WRITE_GAP_MS 500u        /* 2 writes a second per device at most */

/* What a request of ours is for (tag bits 24-25); n (bits 0-7) is a poll's block or a write's place. */
enum { TAG_POLL, TAG_READ, TAG_WRITE, TAG_CHECK };
enum { W_FREE, W_QUEUED, W_READING, W_WRITING, W_CHECKING };
enum { WR_OK, WR_UNCHANGED, WR_REFUSED, WR_FAILED, WR_DIFFERS };   /* an EV_WRITE's outcome */
typedef struct {
    uint8_t    stage, dev, gen, field;
    uint8_t    port;                 /* the port it went out on, once read (0-based) */
    uint32_t   seq;                  /* arrival: the oldest goes first */
    pf_write_t w;
    uint16_t   want[2];              /* the registers as written */
    char       what[40];             /* "control=value", for its event */
} wq_t;
static wq_t s_wq[WRITES];
static uint32_t s_wseq, s_wlast[DEVS];

static D3_BSS pf_build_t s_build;                 /* where one is parsed; nothing is kept here */
static D2_BSS uint64_t s_room[PF_ROOM / 8];
static D2_BSS device_t s_dev[DEVS];
/* The profiles in s_room. p NULL: none here. One that nobody uses or holds stays until
 * its place is wanted, in case it is asked for again. */
static struct { const profile_t *p; uint32_t size; uint8_t users, holds; } s_slot[LOADED];
static struct { bool busy; uint8_t rr; uint16_t use; uint32_t since, busy_ms, win_at; } s_port[PORTS];
static uint32_t s_baud[PORTS];
static char     s_frame[PORTS][4];      /* "8N1" */
static void (*s_gone)(int i, const device_t *d);

void device_init(void)
{
    memset(s_dev, 0, sizeof s_dev);            /* D2 is not zeroed at start */
    memset(s_slot, 0, sizeof s_slot);
    memset(s_port, 0, sizeof s_port);
    memset(s_wq, 0, sizeof s_wq); memset(s_wlast, 0, sizeof s_wlast); s_wseq = 0;
}

void device_on_gone(void (*fn)(int i, const device_t *d)) { s_gone = fn; }

void device_hold(const profile_t *p, int delta)
{
    for (int s = 0; s < LOADED; s++) if (p && p == s_slot[s].p) s_slot[s].holds = (uint8_t)(s_slot[s].holds + delta);
}

const device_t *device_get(int i) { return i >= 0 && i < DEVS && s_dev[i].used ? &s_dev[i] : NULL; }
uint16_t device_line_use(int port) { return port >= 0 && port < PORTS ? s_port[port].use : 0; }

void device_count(int up[PORTS], int all[PORTS])
{
    for (int i = 0; i < PORTS; i++) up[i] = all[i] = 0;
    for (int i = 0; i < DEVS; i++) {
        const device_t *d = device_get(i);
        if (!d || d->off || d->cfg.port < 1 || d->cfg.port > PORTS) continue;
        all[d->cfg.port - 1]++;
        up[d->cfg.port - 1] += d->online;
    }
}

/* The lowest place in s_room for `need` bytes that no loaded profile is in, or NULL.
 * Profiles come and go in any order, so the room may be there only in pieces. */
static void *room_for(size_t need)
{
    unsigned char *base = (unsigned char *)s_room, *at = base;
    for (;;) {
        const unsigned char *hit = NULL;
        size_t n = 0;
        for (int s = 0; s < LOADED && !hit; s++) {
            const unsigned char *q = (const unsigned char *)s_slot[s].p;
            if (q && q < at + need && at < q + s_slot[s].size) { hit = q; n = s_slot[s].size; }
        }
        if (!hit) return need <= PF_ROOM && (size_t)(at - base) <= PF_ROOM - need ? at : NULL;
        at = base + (hit - base) + n;           /* past it: sizes are multiples of 8 */
    }
}

/* The device's profile: already loaded, or parsed and packed into a free place. */
static const profile_t *load(device_t *d, const char *id)
{
    static char err[64];
    size_t n;
    for (int s = 0; s < LOADED; s++)
        if (s_slot[s].p && !strcmp(pf_str(s_slot[s].p, s_slot[s].p->id), id)) { s_slot[s].users++; d->slot = (uint8_t)s; return s_slot[s].p; }
    const char *t = profiles_text(id, &n);
    if (!t) { snprintf(d->error, sizeof d->error, "No profile '%s'", id); return NULL; }
    if (!profile_parse(&s_build, t, n, err, sizeof err)) {
        snprintf(d->error, sizeof d->error, "Profile %.20s: %.40s", id, err);
        return NULL;
    }
    int free_ = -1;
    for (int s = 0; s < LOADED; s++) {           /* those nobody needs make way */
        if (!s_slot[s].users && !s_slot[s].holds) s_slot[s].p = NULL;
        if (!s_slot[s].p && free_ < 0) free_ = s;
    }
    if (free_ < 0) { snprintf(d->error, sizeof d->error, "More than %d different profiles are in use", LOADED); return NULL; }
    size_t need = profile_size(&s_build);
    void *at = room_for(need);
    const profile_t *p = at ? profile_pack(&s_build, at, need) : NULL;
    if (!p) { snprintf(d->error, sizeof d->error, "No room for another profile (%u bytes)", (unsigned)need); return NULL; }
    s_slot[free_].p = p;
    s_slot[free_].size = (uint32_t)need;
    s_slot[free_].users = 1;
    s_slot[free_].holds = 0;
    d->slot = (uint8_t)free_;
    return p;
}

static const char OFF[] = "Its port is off, or not a Modbus gateway", WANTS[] = "The profile wants";

static bool line_differs(const device_t *d)
{
    int port = d->cfg.port - 1;
    return d->p && port >= 0 && port < PORTS && d->p->baud &&
           (d->p->baud != s_baud[port] || strcmp(d->p->frame, s_frame[port]));
}

/* "" if the profile's line is the port's, else what differs. */
static void line_msg(const device_t *d, char *out, size_t max)
{
    int port = d->cfg.port - 1;
    out[0] = 0;
    if (!line_differs(d)) return;
    snprintf(out, max, "%s %lu %s; the port is %lu %s", WANTS, (unsigned long)d->p->baud, d->p->frame,
             (unsigned long)s_baud[port], s_frame[port]);
}

void device_configure(const cfg_t *c)
{
    for (int i = 0; i < PORTS; i++) {
        const port_cfg_t *p = &c->port[i];
        s_baud[i] = p->baud ? p->baud : 9600;
        snprintf(s_frame[i], sizeof s_frame[i], "%u%c%u", p->data_bits % 10u, p->parity == PARITY_EVEN ? 'E' : p->parity == PARITY_ODD ? 'O' : 'N', p->stop_bits % 10u);
    }
    for (int i = 0; i < DEVS; i++) {
        device_t *d = &s_dev[i];
        const dev_cfg_t *k = &c->dev[i];
        bool want = k->profile[0] != 0, off = want && !cfg_dev_enabled(c, i);
        if (d->used == want && (!want || (!memcmp(&d->cfg, k, sizeof *k) && d->off == off))) continue;
        if (d->used) {
            if (s_gone) s_gone(i, d);
            if (d->p) s_slot[d->slot].users--;
        }
        uint8_t gen = (uint8_t)(d->gen + 1);
        memset(d, 0, sizeof *d);
        d->gen = gen;
        if (!want) continue;
        d->used = true;
        d->cfg = *k;
        d->seq = 1;
        if ((d->off = off)) continue;
        d->p = load(d, k->profile);
        for (int b = 0; d->p && b < d->p->nblock; b++) d->due[b] = HAL_GetTick();
    }
    for (int i = 0; i < DEVS; i++) {           /* the line may differ since the port changed */
        device_t *d = &s_dev[i];
        char e[sizeof d->error];
        if (!d->used || !d->p || !strcmp(d->error, OFF)) continue;
        line_msg(d, e, sizeof e);
        if (strcmp(d->error, e)) { memcpy(d->error, e, sizeof e); d->seq++; }
    }
}

static void online(device_t *d)
{
    bool on = false;
    for (int b = 0; b < d->p->nblock; b++) on = on || ((d->have >> b & 1u) && !d->p->block[b].info);
    if (on != d->online) { d->online = on; d->seq++; }
}

static void miss(device_t *d, int b, const char *why)
{
    d->misses++;
    snprintf(d->why, sizeof d->why, "%s (%s)", why, pf_str(d->p, d->p->block[b].id));
    if (d->miss[b] < 255) d->miss[b]++;
    if (d->miss[b] >= d->p->stale && (d->have >> b & 1u)) {
        pf_drop(d->p, b, &d->st);
        d->have &= (uint16_t)~(1u << b);
        d->stale |= (uint16_t)(1u << b);
        d->seq++;
    }
    online(d);
}

/* Roughly how long the line was ours: request and reply at 11 bits a byte, the
 * turnaround, and a timeout's whole wait. */
static void used_line(int port, const device_t *d, size_t n, bool timeout)
{
    uint32_t bytes = 8u + (n ? (uint32_t)n + 3u : 0u);
    s_port[port].busy_ms += bytes * 11000u / s_baud[port] + d->p->turn_ms + (timeout ? d->p->timeout_ms : 0u);
}

static void window(int port, uint32_t now)
{
    uint32_t el = now - s_port[port].win_at;
    if (el < 60000u) return;
    uint32_t pm = s_port[port].busy_ms * 1000u / el;
    s_port[port].use = (uint16_t)(pm > 1000u ? 1000u : pm);
    s_port[port].busy_ms = 0;
    s_port[port].win_at = now;
}

/* Every write, and every refused command, in the event log. A note keeps 39 characters, so
 * the device is a number in `a`, with the outcome above it. */
static void wr_event(int i, int outcome, const char *what, const char *more)
{
    char s[80];
    snprintf(s, sizeof s, "%s%s%s", what, more[0] ? "; " : "", more);
    events_add(EV_WRITE, i >= 0 ? s_dev[i].cfg.port : 0, (uint32_t)(i + 1) | (uint32_t)outcome << 8, s);
}

static uint32_t tag_of(int op, const device_t *d, int i, int n) { return (uint32_t)op << 24 | (uint32_t)d->gen << 16 | (uint32_t)i << 8 | (uint32_t)n; }

/* A block's read: function, address, count. Devices may answer only the very ranges they
 * are polled for (the EG4 inverters: whole groups of 40), so every read of ours is one. */
static void block_pdu(const pf_block_t *b, uint8_t pdu[5])
{
    pdu[0] = b->fn; pdu[1] = (uint8_t)(b->addr >> 8); pdu[2] = (uint8_t)b->addr; pdu[3] = (uint8_t)(b->count >> 8); pdu[4] = (uint8_t)b->count;
}

/* One request of ours into the port's queue; the port is busy until it ends. */
static bool ask(int port, modbus_t *m, int i, int op, int n, const uint8_t *pdu, uint16_t len)
{
    const device_t *d = &s_dev[i];
    if (!m || !modbus_local(m, d->cfg.address, pdu, len, d->p->timeout_ms, tag_of(op, d, i, n))) return false;
    s_port[port].busy = true;
    s_port[port].since = HAL_GetTick();
    return true;
}

/* The port's queue was started again, or a request's end never came: writes on the line are
 * given up. One the device accepted is only not read back; one that may have been made has
 * its block read at once, so HA shows what the device holds. */
static void wr_lost(int port)
{
    for (int k = 0; k < WRITES; k++) {
        wq_t *q = &s_wq[k];
        if (q->stage <= W_QUEUED || q->port != port) continue;       /* its device may be gone: the port it went on */
        device_t *d = &s_dev[q->dev];
        if (q->stage == W_CHECKING) wr_event(q->dev, WR_OK, q->what, "not read back");
        else wr_event(q->dev, WR_FAILED, q->what, "lost");
        if (q->stage >= W_WRITING && d->p && d->gen == q->gen) d->due[d->p->field[q->field].block] = HAL_GetTick();
        q->stage = W_FREE;
    }
}

/* Commands waiting for a device that has changed or gone: said so, and the place freed. */
static void wr_sweep(void)
{
    for (int k = 0; k < WRITES; k++) {
        wq_t *q = &s_wq[k];
        if (q->stage != W_QUEUED || (s_dev[q->dev].p && s_dev[q->dev].gen == q->gen)) continue;
        wr_event(q->dev, WR_FAILED, q->what, "device changed");
        q->stage = W_FREE;
    }
}

/* Commands waiting for a device on a port that is off: they would only be written stale, later. */
static void wr_off(int port)
{
    for (int k = 0; k < WRITES; k++) {
        wq_t *q = &s_wq[k];
        if (q->stage != W_QUEUED || s_dev[q->dev].cfg.port != port + 1) continue;
        wr_event(q->dev, WR_FAILED, q->what, "port off");
        q->stage = W_FREE;
    }
}

/* The registers at addr, from a reply to a read that began at `from`. */
static bool regs_at(const uint8_t *pdu, size_t n, uint16_t from, uint16_t addr, uint8_t regs, uint16_t out[2])
{
    size_t at = 2u + 2u * (size_t)(addr - from);
    if (n < 2 || (pdu[0] & 0x80) || addr < from || at + 2u * regs > n) return false;
    for (int r = 0; r < regs; r++) out[r] = (uint16_t)(pdu[at + 2 * r] << 8 | pdu[at + 2 * r + 1]);
    return true;
}

static void poll_reply(int port, device_t *d, int b, int st, const uint8_t *pdu, size_t n)
{
    const pf_block_t *k = &d->p->block[b];
    used_line(port, d, st == 0 ? n : 0, st != 0);
    if (st) { miss(d, b, "No reply"); return; }
    if (n >= 2 && (pdu[0] & 0x80)) {
        char w[32];
        snprintf(w, sizeof w, "Error %u from the device", pdu[1]);
        miss(d, b, w);
        return;
    }
    size_t want = k->fn <= 2 ? (k->count + 7u) / 8u : k->count * 2u;
    if (n < 2 || pdu[0] != k->fn || pdu[1] != want || n != 2 + want) { miss(d, b, "A reply of the wrong size"); return; }
    if (!pf_decode(d->p, b, pdu + 2, want, &d->st)) { miss(d, b, "A reply its profile rejects"); return; }
    d->miss[b] = 0;
    d->have |= (uint16_t)(1u << b);
    d->stale &= (uint16_t)~(1u << b);
    d->last_ok = HAL_GetTick();
    d->seq++;
    online(d);
}

/* A write's request ended: its block read -> written (or unchanged) -> the block read back. */
static void write_reply(int port, int i, uint8_t gen, int op, int k, int st, const uint8_t *pdu, size_t n)
{
    static const uint8_t AT[] = { W_FREE, W_READING, W_WRITING, W_CHECKING };
    device_t *d = &s_dev[i];
    if (k >= WRITES) return;
    wq_t *q = &s_wq[k];
    if (q->stage != AT[op] || q->dev != i || q->gen != gen) return;        /* not the one asked for */
    if (!d->p || d->gen != gen) { wr_event(i, WR_FAILED, q->what, "device changed"); q->stage = W_FREE; return; }   /* its device changed meanwhile */
    const pf_field_t *f = &d->p->field[q->field];
    const pf_block_t *b = &d->p->block[f->block];
    modbus_t *m = bridge_modbus_port(port);
    uint32_t now = HAL_GetTick();
    if (op == TAG_CHECK) {
        uint16_t got[2];
        bool read = st == 0 && regs_at(pdu, n, b->addr, q->w.addr, q->w.regs, got), same = read;
        poll_reply(port, d, f->block, st, pdu, n);
        for (int r = 0; read && r < q->w.regs; r++) same = same && !((got[r] ^ q->want[r]) & q->w.mask[r]);
        char v[24] = "";
        if (read && !same && !device_value(d, q->field, v, sizeof v)) v[0] = 0;
        wr_event(i, read && !same ? WR_DIFFERS : WR_OK, q->what, read ? v : "not read back");
        q->stage = W_FREE;
        return;
    }
    uint16_t have[2] = { 0, 0 };
    if (op == TAG_READ) poll_reply(port, d, f->block, st, pdu, n);     /* the block, as a poll reads it: its values taken */
    else used_line(port, d, st == 0 ? n : 0, st != 0);
    char e[16] = "";
    if (st) snprintf(e, sizeof e, "no reply");
    else if (n >= 2 && (pdu[0] & 0x80)) snprintf(e, sizeof e, "error %u", pdu[1]);
    else if (op == TAG_READ && (n != 2u + 2u * b->count || pdu[0] != b->fn || pdu[1] != 2u * b->count ||
                                !regs_at(pdu, n, b->addr, q->w.addr, q->w.regs, have))) snprintf(e, sizeof e, "bad reply");
    if (e[0]) {
        wr_event(i, WR_FAILED, q->what, e);
        q->stage = W_FREE;
        if (op == TAG_WRITE) d->due[f->block] = now;            /* it may have been made: HA shows what it holds */
        return;
    }
    if (op == TAG_READ) {
        if (!pf_merge(&q->w, have, q->want)) {
            wr_event(i, WR_UNCHANGED, q->what, "");
            q->stage = W_FREE;
            d->due[f->block] = now;                           /* HA may show an older value: read it */
            return;
        }
        uint8_t w[10] = { 6, (uint8_t)(q->w.addr >> 8), (uint8_t)q->w.addr };
        uint16_t len = 5;
        if (q->w.regs == 1) { w[3] = (uint8_t)(q->want[0] >> 8); w[4] = (uint8_t)q->want[0]; }
        else {
            w[0] = 16; w[3] = 0; w[4] = 2; w[5] = 4;
            w[6] = (uint8_t)(q->want[0] >> 8); w[7] = (uint8_t)q->want[0]; w[8] = (uint8_t)(q->want[1] >> 8); w[9] = (uint8_t)q->want[1];
            len = 10;
        }
        if (!ask(port, m, i, TAG_WRITE, k, w, len)) { wr_event(i, WR_FAILED, q->what, "not sent"); q->stage = W_FREE; return; }
        q->stage = W_WRITING;
        return;
    }
    uint8_t r[5];
    block_pdu(b, r);
    if (!ask(port, m, i, TAG_CHECK, k, r, 5)) {
        wr_event(i, WR_OK, q->what, "not read back");
        q->stage = W_FREE;
        d->due[f->block] = now;
        return;
    }
    q->stage = W_CHECKING;
}

void device_reply(int port, uint32_t tag, int st, const uint8_t *pdu, size_t n)
{
    int op = (int)(tag >> 24 & 3u), i = (int)(tag >> 8 & 0xFFu), k = (int)(tag & 0xFFu);
    if (port >= 0 && port < PORTS) s_port[port].busy = false;
    if (i >= DEVS || port < 0 || port >= PORTS) return;
    if (op != TAG_POLL) { write_reply(port, i, (uint8_t)(tag >> 16), op, k, st, pdu, n); return; }
    device_t *d = &s_dev[i];
    if (!d->p || d->gen != (uint8_t)(tag >> 16) || k >= d->p->nblock) return;
    poll_reply(port, d, k, st, pdu, n);
}
/* The most overdue block among the port's devices, starting after the last one asked. */
static void submit(int port, modbus_t *m, uint32_t now)
{
    int best = -1, bb = 0;
    uint32_t late = 0;
    for (int k = 0; k < DEVS; k++) {
        int i = (s_port[port].rr + k) % DEVS;
        device_t *d = &s_dev[i];
        if (!d->p || d->cfg.port != port + 1) continue;
        for (int b = 0; b < d->p->nblock; b++) {
            int32_t l = (int32_t)(now - d->due[b]);
            if (l < 0 || (best >= 0 && (uint32_t)l <= late)) continue;
            best = i;
            bb = b;
            late = (uint32_t)l;
        }
    }
    if (best < 0) return;
    device_t *d = &s_dev[best];
    const pf_block_t *b = &d->p->block[bb];
    uint8_t pdu[5];
    block_pdu(b, pdu);
    if (!ask(port, m, best, TAG_POLL, bb, pdu, 5)) return;   /* 8 waiting already: next pass */
    s_port[port].rr = (uint8_t)((best + 1) % DEVS);
    d->due[bb] = now + b->period_ms;
    d->polls++;
}

/* The oldest command waiting for this port whose device may write now: its control's block
 * read first, the same request as its poll (a device that answers only whole groups answers
 * it). False if there is none. */
static bool write_next(int port, modbus_t *m, uint32_t now)
{
    int best = -1;
    for (int k = 0; k < WRITES; k++) {
        wq_t *q = &s_wq[k];
        if (q->stage != W_QUEUED) continue;
        const device_t *d = &s_dev[q->dev];
        if (!d->p || d->gen != q->gen || d->cfg.port != port + 1 || now - s_wlast[q->dev] < WRITE_GAP_MS) continue;
        if (best < 0 || (int32_t)(q->seq - s_wq[best].seq) < 0) best = k;
    }
    if (best < 0) return false;
    wq_t *q = &s_wq[best];
    device_t *d = &s_dev[q->dev];
    uint8_t pdu[5];
    block_pdu(&d->p->block[d->p->field[q->field].block], pdu);
    if (!ask(port, m, q->dev, TAG_READ, best, pdu, 5)) return true;      /* the queue is full: next pass */
    q->stage = W_READING;
    q->port = (uint8_t)port;
    s_wlast[q->dev] = now;
    d->polls++;                                                          /* a block read, as a poll is */
    return true;
}

bool device_command(const char *name, const char *control, const char *payload, bool retained)
{
    char what[40], e[32];
    int i = -1, f = -1;
    for (int k = 0; k < DEVS; k++) if (s_dev[k].used && !strcmp(s_dev[k].cfg.name, name)) i = k;
    if (i < 0) {
        snprintf(what, sizeof what, "%s/%s=%s", name, control, payload);
        wr_event(-1, WR_REFUSED, what, "no such device");
        return false;
    }
    device_t *d = &s_dev[i];
    snprintf(what, sizeof what, "%s=%s", control, payload);
    if (d->p) f = profile_field(d->p, control);
    const char *why = retained ? "retained" : d->off ? "device off" : !d->cfg.controls ? "controls off" :
                      f < 0 || !pf_control(&d->p->field[f]) ? "no such control" :
                      d->cfg.port < 1 || d->cfg.port > PORTS || !bridge_modbus_port(d->cfg.port - 1) ? "port off" : NULL;
    pf_write_t w;
    if (!why && !pf_command(d->p, f, payload, &w, e, sizeof e)) why = e;
    if (why) { wr_event(i, WR_REFUSED, what, why); return false; }
    int slot = -1;
    for (int k = 0; k < WRITES && slot < 0; k++)              /* the same control waiting: the newer value */
        if (s_wq[k].stage == W_QUEUED && s_wq[k].dev == i && s_wq[k].field == f && s_wq[k].gen == d->gen) slot = k;
    for (int k = 0; k < WRITES && slot < 0; k++)
        if (s_wq[k].stage == W_FREE) { slot = k; s_wq[k].seq = ++s_wseq; }
    if (slot < 0) { wr_event(i, WR_REFUSED, what, "too many waiting"); return false; }
    wq_t *q = &s_wq[slot];
    q->stage = W_QUEUED;
    q->dev = (uint8_t)i;
    q->gen = d->gen;
    q->field = (uint8_t)f;
    q->w = w;
    snprintf(q->what, sizeof q->what, "%s", what);
    return true;
}

void device_poll(void)
{
    uint32_t now = HAL_GetTick();
    wr_sweep();
    for (int port = 0; port < PORTS; port++) {
        uint16_t turn = 0;
        bool any = false;
        for (int i = 0; i < DEVS; i++)
            if (s_dev[i].p && s_dev[i].cfg.port == port + 1) {
                any = true;
                if (s_dev[i].p->turn_ms > turn) turn = s_dev[i].p->turn_ms;
            }
        modbus_t *m = bridge_modbus_port(port);
        if (!any) {                                  /* nothing of ours: no extra quiet, no use */
            if (m) m->turn_us = 0;
            s_port[port].use = 0;
            s_port[port].busy_ms = 0;
            s_port[port].win_at = now;
            if (!m || !modbus_local_pending(m)) { s_port[port].busy = false; wr_lost(port); }
            continue;
        }
        /* Say why it is not polled, or that it is again. Every pass, so nothing is put
         * into words unless it changed (device_configure keeps a line's message current). */
        for (int i = 0; i < DEVS; i++) {
            device_t *d = &s_dev[i];
            if (!d->p || d->cfg.port != port + 1) continue;
            if (!m) {
                if (strcmp(d->error, OFF)) { memset(d->error, 0, sizeof d->error); memcpy(d->error, OFF, sizeof OFF); d->seq++; }
            } else if (!line_differs(d)) {
                if (d->error[0]) { memset(d->error, 0, sizeof d->error); d->seq++; }
            } else if (strncmp(d->error, WANTS, sizeof WANTS - 1)) {
                line_msg(d, d->error, sizeof d->error);
                d->seq++;
            }
        }
        window(port, now);
        if (!m) {                                    /* keep what is overdue overdue, whatever the time */
            for (int i = 0; i < DEVS; i++)
                if (s_dev[i].p && s_dev[i].cfg.port == port + 1)
                    for (int b = 0; b < s_dev[i].p->nblock; b++)
                        if ((int32_t)(now - s_dev[i].due[b]) > 0) s_dev[i].due[b] = now;
            wr_lost(port);
            wr_off(port);
            s_port[port].busy = false;
            continue;
        }
        if (s_port[port].busy && !modbus_local_pending(m)) { s_port[port].busy = false; wr_lost(port); }   /* dropped by a re-init */
        m->turn_us = (uint32_t)turn * 1000u;
        if (s_port[port].busy && now - s_port[port].since < LOST_MS) continue;
        if (s_port[port].busy) wr_lost(port);               /* its end never came */
        s_port[port].busy = false;
        if (!write_next(port, m, now)) submit(port, m, now);
    }
}

size_t device_value(const device_t *d, int f, char *out, size_t max)
{
    const pf_field_t *x = &d->p->field[f];
    bool ok = pf_ok(&d->st, f);
    int w;
    if (!ok) w = snprintf(out, max, "null");
    else if (x->kind == PF_ENUM || x->kind == PF_BITS || x->kind == PF_ASCII || x->kind == PF_SELECT || x->kind == PF_TIME) {
        char t[512];
        pf_text(d->p, f, &d->st, t, sizeof t);
        return json_str(t, out, max);
    } else if (x->kind == PF_BINARY || x->kind == PF_CALCBIN || x->kind == PF_SWITCH) w = snprintf(out, max, "%s", d->st.val[f].v != 0 ? "true" : "false");
    else return max > 1 ? pf_num(out, max, d->st.val[f].v, pf_prec(x)) : 0;
    return w < 0 || (size_t)w >= max ? 0 : (size_t)w;
}

#define PART_VALUES 40

int device_json_part(int i, int k, char *out, size_t max)
{
    const device_t *d = device_get(i);
    size_t n = 0;
    char s[3][80];
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) { return -1; } n += (size_t)w_; } while (0)
    if (!max) return -1;
    out[0] = 0;
    if (k == 0) {
        if (!d) return -1;
        if (!json_str(d->cfg.label, s[0], sizeof s[0]) || !json_str(d->error, s[1], sizeof s[1]) || !json_str(d->why, s[2], sizeof s[2])) return -1;
        OUT("{\"n\":%d,\"profile\":\"%s\",\"name\":\"%s\",\"label\":%s,\"port\":%u,\"address\":%u,\"enabled\":%s,\"controls\":%s,"
            "\"ids\":\"%s\",\"prefix\":\"%s\",\"error\":%s,\"online\":%s,\"polls\":%lu,\"misses\":%lu,\"why\":%s,\"ago_ms\":%ld",
            i + 1, d->cfg.profile, d->cfg.name, s[0], d->cfg.port, d->cfg.address, d->off ? "false" : "true",
            d->cfg.controls ? "true" : "false",
            d->cfg.ids ? "esphome" : "own", d->cfg.prefix, s[1], d->online ? "true" : "false",
            (unsigned long)d->polls, (unsigned long)d->misses, s[2],
            d->last_ok ? (long)((HAL_GetTick() - d->last_ok) & 0x7FFFFFFF) : -1L);
        if (d->p) {
            if (!json_str(pf_str(d->p, d->p->name), s[0], sizeof s[0]) || !json_str(pf_str(d->p, d->p->maker), s[1], sizeof s[1])) return -1;
            OUT(",\"model\":%s,\"maker\":%s", s[0], s[1]);
        }
        OUT(",\"values\":[");
    }
    int from = k * PART_VALUES, to = !d || !d->p ? 0 : k == DEV_PARTS - 1 ? d->p->nfield : from + PART_VALUES;
    for (int f = from; d && d->p && f < to && f < d->p->nfield; f++) {
        const pf_field_t *x = &d->p->field[f];
        char v[520], nm[96], u[40];
        if (!device_value(d, f, v, sizeof v)) return -1;
        if (!json_str(pf_str(d->p, x->name), nm, sizeof nm) || !json_str(pf_str(d->p, x->unit), u, sizeof u)) return -1;
        OUT("%s[\"%s\",%s,%s,%s]", f ? "," : "", pf_str(d->p, x->id), nm, v, u);
    }
    if (k == DEV_PARTS - 1) OUT("]}");
    return (int)n;
#undef OUT
}

size_t device_json(int i, char *out, size_t max)
{
    size_t n = 0;
    for (int k = 0; k < DEV_PARTS; k++) {
        int w = device_json_part(i, k, out + n, max - n);
        if (w < 0) return 0;
        n += (size_t)w;
    }
    return n;
}
