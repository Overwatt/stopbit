/*
 * device.c — Devices polled for their profiles' values (device.h). Per port: which block
 * of which device is most overdue goes next, as one request in the port's Modbus queue
 * (modbus.c's local client); its reply is decoded (pfdecode.c). Profiles are parsed once
 * and shared by the devices that use them.
 */
#include "device.h"
#include "profiles.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#ifndef D2_BSS
#define D2_BSS __attribute__((section(".d2_bss")))
#endif
#define LOADED  3                /* different profiles at once */
#define LOST_MS 90000u           /* a request whose end never came: the port is free again (MB_QUEUE x the longest timeout, 10 s, + 10 s) */

static D2_BSS profile_t s_prof[LOADED];
static D2_BSS device_t s_dev[DEVS];
static bool    s_loaded[LOADED];
static uint8_t s_users[LOADED], s_holds[LOADED];
static struct { bool busy; uint8_t rr; uint16_t use; uint32_t since, busy_ms, win_at; } s_port[PORTS];
static uint32_t s_baud[PORTS];
static char     s_frame[PORTS][4];      /* "8N1" */
static void (*s_gone)(int i, const device_t *d);

void device_init(void)
{
    memset(s_dev, 0, sizeof s_dev);            /* D2 is not zeroed at start */
    memset(s_loaded, 0, sizeof s_loaded);
    memset(s_users, 0, sizeof s_users);
    memset(s_holds, 0, sizeof s_holds);
    memset(s_port, 0, sizeof s_port);
}

void device_on_gone(void (*fn)(int i, const device_t *d)) { s_gone = fn; }

void device_hold(const profile_t *p, int delta)
{
    for (int s = 0; s < LOADED; s++) if (p == &s_prof[s]) s_holds[s] = (uint8_t)(s_holds[s] + delta);
}

const device_t *device_get(int i) { return i >= 0 && i < DEVS && s_dev[i].used ? &s_dev[i] : NULL; }
uint16_t device_line_use(int port) { return port >= 0 && port < PORTS ? s_port[port].use : 0; }

void device_count(int up[PORTS], int all[PORTS])
{
    for (int i = 0; i < PORTS; i++) up[i] = all[i] = 0;
    for (int i = 0; i < DEVS; i++) {
        const device_t *d = device_get(i);
        if (!d || d->cfg.port < 1 || d->cfg.port > PORTS) continue;
        all[d->cfg.port - 1]++;
        up[d->cfg.port - 1] += d->online;
    }
}

/* The device's profile: already loaded, or parsed into a free place. */
static const profile_t *load(device_t *d, const char *id)
{
    static char err[64];
    size_t n;
    int free_ = -1;
    for (int s = 0; s < LOADED; s++) {
        if (s_loaded[s] && !strcmp(pf_str(&s_prof[s], s_prof[s].id), id)) { s_users[s]++; d->slot = (uint8_t)s; return &s_prof[s]; }
        if (!s_users[s] && !s_holds[s] && free_ < 0) free_ = s;
    }
    const char *t = profiles_text(id, &n);
    if (!t) { snprintf(d->error, sizeof d->error, "No profile '%s'", id); return NULL; }
    if (free_ < 0) { snprintf(d->error, sizeof d->error, "More than %d different profiles are in use", LOADED); return NULL; }
    s_loaded[free_] = false;
    if (!profile_parse(&s_prof[free_], t, n, err, sizeof err)) {
        snprintf(d->error, sizeof d->error, "Profile %.20s: %.40s", id, err);
        return NULL;
    }
    s_loaded[free_] = true;
    s_users[free_] = 1;
    d->slot = (uint8_t)free_;
    return &s_prof[free_];
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
        bool want = k->profile[0] != 0;
        if (d->used == want && (!want || !memcmp(&d->cfg, k, sizeof *k))) continue;
        if (d->used) {
            if (s_gone) s_gone(i, d);
            if (d->p) s_users[d->slot]--;
        }
        uint8_t gen = (uint8_t)(d->gen + 1);
        memset(d, 0, sizeof *d);
        d->gen = gen;
        if (!want) continue;
        d->used = true;
        d->cfg = *k;
        d->seq = 1;
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

void device_reply(int port, uint32_t tag, int st, const uint8_t *pdu, size_t n)
{
    int i = (int)(tag >> 8 & 0xFFu), b = (int)(tag & 0xFFu);
    if (port >= 0 && port < PORTS) s_port[port].busy = false;
    if (i >= DEVS || port < 0 || port >= PORTS) return;
    device_t *d = &s_dev[i];
    if (!d->p || d->gen != (uint8_t)(tag >> 16) || b >= d->p->nblock) return;
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
    const uint8_t pdu[5] = { b->fn, (uint8_t)(b->addr >> 8), (uint8_t)b->addr, (uint8_t)(b->count >> 8), (uint8_t)b->count };
    if (!modbus_local(m, d->cfg.address, pdu, 5, d->p->timeout_ms, (uint32_t)d->gen << 16 | (uint32_t)best << 8 | (uint32_t)bb))
        return;                                  /* 8 waiting already: next pass */
    s_port[port].busy = true;
    s_port[port].since = now;
    s_port[port].rr = (uint8_t)((best + 1) % DEVS);
    d->due[bb] = now + b->period_ms;
    d->polls++;
}

void device_poll(void)
{
    uint32_t now = HAL_GetTick();
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
            s_port[port].busy = false;
            continue;
        }
        if (s_port[port].busy && !modbus_local_pending(m)) s_port[port].busy = false;   /* dropped by a re-init */
        m->turn_us = (uint32_t)turn * 1000u;
        if (s_port[port].busy && now - s_port[port].since < LOST_MS) continue;
        s_port[port].busy = false;
        submit(port, m, now);
    }
}

size_t device_value(const device_t *d, int f, char *out, size_t max)
{
    const pf_field_t *x = &d->p->field[f];
    bool ok = pf_ok(&d->st, f);
    int w;
    if (!ok) w = snprintf(out, max, "null");
    else if (x->kind == PF_ENUM || x->kind == PF_BITS || x->kind == PF_ASCII) {
        char t[512];
        pf_text(d->p, f, &d->st, t, sizeof t);
        return json_str(t, out, max);
    } else if (x->kind == PF_BINARY || x->kind == PF_CALCBIN) w = snprintf(out, max, "%s", d->st.val[f].v != 0 ? "true" : "false");
    else return max > 1 ? pf_num(out, max, d->st.val[f].v, pf_prec(x)) : 0;
    return w < 0 || (size_t)w >= max ? 0 : (size_t)w;
}

size_t device_json(int i, char *out, size_t max)
{
    const device_t *d = device_get(i);
    size_t n = 0;
    char s[3][80];
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) { return 0; } n += (size_t)w_; } while (0)
    if (!d || !max) return 0;
    if (!json_str(d->cfg.label, s[0], sizeof s[0]) || !json_str(d->error, s[1], sizeof s[1]) || !json_str(d->why, s[2], sizeof s[2])) return 0;
    OUT("{\"n\":%d,\"profile\":\"%s\",\"name\":\"%s\",\"label\":%s,\"port\":%u,\"address\":%u,\"controls\":%s,"
        "\"ids\":\"%s\",\"prefix\":\"%s\",\"error\":%s,\"online\":%s,\"polls\":%lu,\"misses\":%lu,\"why\":%s,\"ago_ms\":%ld",
        i + 1, d->cfg.profile, d->cfg.name, s[0], d->cfg.port, d->cfg.address, d->cfg.controls ? "true" : "false",
        d->cfg.ids ? "esphome" : "own", d->cfg.prefix, s[1], d->online ? "true" : "false",
        (unsigned long)d->polls, (unsigned long)d->misses, s[2],
        d->last_ok ? (long)((HAL_GetTick() - d->last_ok) & 0x7FFFFFFF) : -1L);
    if (!d->p) { OUT("}"); return n; }
    if (!json_str(pf_str(d->p, d->p->name), s[0], sizeof s[0]) || !json_str(pf_str(d->p, d->p->maker), s[1], sizeof s[1])) return 0;
    OUT(",\"model\":%s,\"maker\":%s,\"values\":[", s[0], s[1]);
    for (int f = 0; f < d->p->nfield; f++) {
        const pf_field_t *x = &d->p->field[f];
        char v[520], nm[96], u[40];
        if (!device_value(d, f, v, sizeof v)) return 0;
        if (!json_str(pf_str(d->p, x->name), nm, sizeof nm) || !json_str(pf_str(d->p, x->unit), u, sizeof u)) return 0;
        OUT("%s[\"%s\",%s,%s,%s]", f ? "," : "", pf_str(d->p, x->id), nm, v, u);
    }
    OUT("]}");
    return n;
#undef OUT
}
