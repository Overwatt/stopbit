/*
 * http.c — The web server (src/http/http.c, as built into the app) on a fake lwIP, driven
 * by a script: open connections, send bytes in pieces, send requests signed with the
 * (made-up) update key, close, reset, acknowledge, let time pass. The firmware library
 * and settings parser behind it are the real ones; HTTPS and the rest are stand-ins.
 *
 * The fake checks lwIP's rules as it goes: nothing written to a connection after it was
 * closed or aborted, no more acknowledged than was received, no write bigger than the room.
 *
 * Script: op byte, then for ops with data a 2-byte length and the bytes.
 *   0 open (port 80)  1 data  2 signed request: "METHOD PATH\nheaders\n\nbody"
 *   3 client closes   4 acknowledge what was sent   5 time passes, the main loop runs
 *   6 reset by the client   7 pick another connection   8 open on 443   9 data on 443
 */
#include "metrics.h"
#include "http.h"
#include "api.h"
#include "tls.h"
#include "fwlib.h"
#include "events.h"
#include "cfg.h"
#include "net.h"
#include "eth_mac.h"
#include "sha256.h"
#include "secret_key.h"
#include "lwip/tcp.h"
#include "stm32h7xx_hal.h"
#include "../flash_sim.h"
#include <assert.h>
#include "json_ok.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- lwIP ---- */
struct tcp_pcb {
    bool used, listening, closed, aborted, gone;
    void *arg; tcp_accept_fn acc; tcp_recv_fn rx; tcp_sent_fn sent; tcp_err_fn err; tcp_poll_fn poll;
    uint8_t out[TCP_SND_BUF]; int out_n; long unacked; u16_t port;
};
static struct tcp_pcb P[24];
static void live(const struct tcp_pcb *p) { assert(p >= P && p < P + 24 && p->used && !p->gone && !p->aborted); }
struct tcp_pcb *tcp_new(void) { for (int i = 0; i < 24; i++) if (!P[i].used) { memset(&P[i], 0, sizeof P[i]); P[i].used = true; return &P[i]; } return NULL; }
void ip_set_option(struct tcp_pcb *p, int o) { (void)p; (void)o; }
err_t tcp_bind(struct tcp_pcb *p, const void *a, u16_t port) { (void)a; live(p); p->port = port; return ERR_OK; }
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *p, u8_t n) { (void)n; live(p); p->listening = true; return p; }
void tcp_accept(struct tcp_pcb *p, tcp_accept_fn f) { p->acc = f; }
void tcp_arg(struct tcp_pcb *p, void *a) { live(p); p->arg = a; }
void tcp_recv(struct tcp_pcb *p, tcp_recv_fn f) { live(p); p->rx = f; }
void tcp_sent(struct tcp_pcb *p, tcp_sent_fn f) { live(p); p->sent = f; }
void tcp_err(struct tcp_pcb *p, tcp_err_fn f) { live(p); p->err = f; }
void tcp_poll(struct tcp_pcb *p, tcp_poll_fn f, u8_t n) { (void)n; live(p); p->poll = f; }
void tcp_nagle_disable(struct tcp_pcb *p) { live(p); }
u16_t tcp_sndbuf(struct tcp_pcb *p) { live(p); return (u16_t)(TCP_SND_BUF - p->out_n); }
u16_t tcp_sndqueuelen(struct tcp_pcb *p) { live(p); return (u16_t)(p->out_n / TCP_MSS); }
err_t tcp_write(struct tcp_pcb *p, const void *d, u16_t n, u8_t f)
{
    (void)f; live(p); assert(!p->closed);
    if (n > TCP_SND_BUF - p->out_n) return ERR_MEM;
    memcpy(p->out + p->out_n, d, n); p->out_n += n; return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *p) { live(p); return ERR_OK; }
void tcp_recved(struct tcp_pcb *p, u16_t n) { live(p); p->unacked -= n; assert(p->unacked >= 0); }
err_t tcp_close(struct tcp_pcb *p) { live(p); p->closed = true; return ERR_OK; }
void tcp_abort(struct tcp_pcb *p) { live(p); p->aborted = true; }
err_t tcp_connect(struct tcp_pcb *p, const ip_addr_t *ip, u16_t port, err_t (*f)(void *, struct tcp_pcb *, err_t)) { (void)p; (void)ip; (void)port; (void)f; return ERR_VAL; }
err_t dns_gethostbyname(const char *n, ip_addr_t *ip, void (*f)(const char *, const ip_addr_t *, void *), void *a) { (void)n; (void)ip; (void)f; (void)a; return ERR_VAL; }
void pbuf_ref(struct pbuf *p) { p->ref++; }
void pbuf_cat(struct pbuf *h, struct pbuf *t) { while (h->next) { h->tot_len += t->tot_len; h = h->next; } h->tot_len += t->tot_len; h->next = t; }
void pbuf_free(struct pbuf *p) { while (p && --p->ref == 0) { struct pbuf *n = p->next; free(p->payload); free(p); p = n; } }

/* ---- the rest of the gateway ---- */
t_dwt_t t_dwt;
uint32_t t_dwt_now(void) { return t_now * 400000u; }
void NVIC_SystemReset(void) { _exit(0); }
uint8_t g_mac[6] = { 2, 0x54, 0, 0, 0, 1 };
char g_hostname[16] = "n540-fuzz";
eth_info_t g_eth;
uint32_t g_reset_cause, g_last_crumb;
bool g_clock_hse = true;
void net_ip_str(char o[16]) { strcpy(o, "10.0.0.2"); }
static uint32_t s_rng;
bool rng_fill(void *d, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t *)d)[i] = (uint8_t)(++s_rng * 2654435761u >> 24); return true; }
bool clock_known(void) { return true; }
uint32_t clock_unix(void) { return 1795600000u + t_now / 1000u; }
static const uint8_t PAGE[] = "<!doctype html><title>x</title>";
const uint8_t *api_page(uint32_t *n) { *n = sizeof PAGE; return PAGE; }
static uint8_t s_web[32] = { 9, 9, 9 };
const uint8_t *api_web_key(void) { return s_web; }
const char *api_fw_need(void) { return "listen_only"; }
bool api_is_post(const char *p) { return !strncmp(p, "/api/", 5) && strcmp(p, "/api/state"); }
/* GET /metrics: the real format (metrics.c) of made-up figures, two Modbus ports among them. */
int api_metrics(uint32_t part, char *o, size_t n)
{
    static metrics_t m;
    static const char *const NAME[PORTS] = { "TigoBus", "Meter \"A\"", "Port 3", "Port 4" };
    memset(&m, 0, sizeof m);
    m.version = "sim";
    m.uptime_s = 1234; m.starts = 7; m.loop_recent_us = 1500; m.loop_max_us = 4200;
    m.temp_ok = m.link = m.clock_synced = true; m.temp_dc = 421;
    m.eth_rx = 100000; m.eth_tx = 90000; m.handshakes = 12; m.aes_hw = 3456;
    for (int i = 0; i < PORTS; i++) {
        m.port[i].name = NAME[i];
        m.port[i].modbus = i == 1 || i == 2;
        m.port[i].mode = m.port[i].modbus ? "modbus" : "server";
        m.port[i].up = true; m.port[i].clients = (uint32_t)i;
        m.port[i].line_rx = 1000000u * (uint32_t)(i + 1); m.port[i].net_tx = 999000u * (uint32_t)(i + 1);
        m.port[i].heard_ms = 250 * i - 1;
        for (int b = 0; b <= FWD_BUCKETS; b++) m.port[i].fwd.n[b] = (uint32_t)(1000 >> b);
        m.port[i].fwd.sum_us = 4000000; m.port[i].mb.requests = 5000; m.port[i].mb.replies = 4990; m.port[i].mb.total_ms = 99800;
        for (int b = 0; b <= MB_HIST; b++) m.port[i].mb.hist[b] = (uint32_t)(4000 >> b);
    }
    return metrics_part(&m, part, o, n);
}
void api_restart(void) { _exit(0); }
static cfg_t s_cfg;
const char *api_post(const char *p, api_req_t *r, char *o, size_t n)
{
    assert(r->body && r->len < 8192 && r->body[r->len] == 0);
    if (!strcmp(p, "/api/config")) {
        static cfg_t c;
        c = s_cfg;
        if (!cfg_parse(&c, r->body, o, n) || !cfg_validate(&c, o, n)) return "400 Bad Request";
        s_cfg = c;
    }
    snprintf(o, n, "{}");
    return "200 OK";
}
static int checked(size_t k, const char *o) { assert(!k || json_ok(o)); return (int)k; }
int api_get(const char *p, char *o, size_t n)
{
    if (!strcmp(p, "/api/config")) return checked(cfg_json(&s_cfg, o, n), o);
    if (!strcmp(p, "/api/library")) return checked(fwlib_json(0, o, n), o);
    if (!strcmp(p, "/api/events")) { events_poll(); return checked(events_json(0, 30, o, n), o); }   /* what the server logged */
    if (!strcmp(p, "/api/big")) { if (n < 4300) return 0; memset(o, 'x', 4299); o[0] = o[4298] = '"'; o[4299] = 0; return 4299; }
    return -1;
}
/* HTTPS is a stand-in: what the server writes is kept, as a socket would. */
struct tls { int used; uint8_t out[16384]; int out_n; bool closed; void *http; };
static struct tls T[4];
uint32_t tls_room(const tls_t *t) { assert(!t->closed); return (uint32_t)(sizeof t->out - (size_t)t->out_n > 4096 ? 4096 : sizeof t->out - (size_t)t->out_n); }
void tls_write(tls_t *t, const void *d, uint32_t n) { assert(!t->closed && n <= tls_room(t)); memcpy(t->out + t->out_n, d, n); t->out_n += (int)n; }
void tls_close(tls_t *t) { assert(!t->closed); t->closed = true; t->http = NULL; }
const char *tls_name(void) { return "gateway.example.com"; }
void tls_mask(int dev, uint32_t a, uint8_t *b, uint32_t n) { (void)dev; (void)a; (void)b; (void)n; }
void acme_mask(int dev, uint32_t a, uint8_t *b, uint32_t n) { (void)dev; (void)a; (void)b; (void)n; }

/* ---- driving it ---- */
static struct tcp_pcb *s_listen;

static struct tcp_pcb *open80(void)
{
    struct tcp_pcb *c = tcp_new();
    if (!c || !s_listen || !s_listen->acc) return NULL;
    c->port = 80;
    if (s_listen->acc(s_listen->arg, c, ERR_OK) != ERR_OK) { c->used = false; return NULL; }
    return c;
}

static void deliver(struct tcp_pcb *c, const uint8_t *d, size_t n)
{
    if (!c || !c->used || c->gone || c->aborted || c->closed || !c->rx || !n) return;
    struct pbuf *h = NULL, **tail = &h;
    for (size_t at = 0; at < n;) {                       /* in pieces, as segments come */
        size_t k = n - at > 700 ? 700 : n - at;
        struct pbuf *q = calloc(1, sizeof *q);
        q->payload = malloc(k);
        memcpy(q->payload, d + at, k);
        q->len = (u16_t)k;
        q->ref = 1;
        *tail = q; tail = &q->next;
        at += k;
    }
    for (struct pbuf *q = h; q; q = q->next) { size_t t = 0; for (struct pbuf *r = q; r; r = r->next) t += r->len; q->tot_len = (u16_t)t; }
    c->unacked += (long)n;
    c->rx(c->arg, c, h, ERR_OK);
}

static void ack(struct tcp_pcb *c)
{
    if (!c || !c->used || c->gone || c->aborted || !c->out_n) return;
    u16_t n = (u16_t)c->out_n;
    c->out_n = 0;
    if (c->sent) c->sent(c->arg, c, n);
}

static void tick(uint32_t ms)
{
    t_now += ms;
    http_poll();
    for (int i = 0; i < 24; i++) {
        struct tcp_pcb *c = &P[i];
        if (c->used && !c->gone && !c->aborted && !c->listening && c->poll) c->poll(c->arg, c);
        if (c->used && (c->aborted || (c->closed && !c->out_n))) c->gone = true;   /* lwIP frees it */
    }
}

/* A nonce, the way the page gets one, then the request signed with it. */
static bool sign(const uint8_t *t, size_t n, uint8_t *out, size_t *outn, size_t max)
{
    char tmpl[8192], nonce[40] = "";
    if (n >= sizeof tmpl) return false;
    memcpy(tmpl, t, n); tmpl[n] = 0;
    char *nl = memchr(tmpl, '\n', n);
    if (!nl) return false;
    *nl = 0;
    char *sp = strchr(tmpl, ' ');
    if (!sp) return false;
    *sp = 0;
    const char *method = tmpl, *path = sp + 1, *hdrs = nl + 1;
    char *gap = strstr((char *)hdrs, "\n\n");
    const uint8_t *body = (const uint8_t *)"";
    size_t blen = 0;
    if (gap) { *gap = 0; body = (const uint8_t *)gap + 2; blen = n - (size_t)((const char *)body - tmpl); }
    struct tcp_pcb *c = open80();
    if (!c) return false;
    deliver(c, (const uint8_t *)"GET /nonce HTTP/1.1\r\n\r\n", 23);
    char *b = memmem(c->out, (size_t)c->out_n, "\r\n\r\n", 4);
    if (b && c->out + c->out_n - (uint8_t *)b >= 4 + 32) { memcpy(nonce, b + 4, 32); nonce[32] = 0; }
    ack(c);
    if (!nonce[0]) return false;
    uint8_t sha[32], mac[32];
    sha256_t s;
    sha256_init(&s); sha256_update(&s, body, blen); sha256_final(&s, sha);
    char shahex[65], machex[65], msg[512];
    for (int i = 0; i < 32; i++) sprintf(shahex + 2 * i, "%02x", sha[i]);
    int m = snprintf(msg, sizeof msg, "%s\n%s %s\n%s", nonce, method, path, shahex);
    if (m <= 0 || m >= (int)sizeof msg) return false;
    hmac_sha256(SECRET_KEY, msg, (size_t)m, NULL, 0, mac);
    for (int i = 0; i < 32; i++) sprintf(machex + 2 * i, "%02x", mac[i]);
    int h = snprintf((char *)out, max, "%s %s HTTP/1.1\r\nHost: x\r\nX-Auth: %s\r\nX-Body-SHA256: %s\r\nContent-Length: %zu\r\n%s%s\r\n",
                     method, path, machex, shahex, blen, hdrs, *hdrs ? "\r\n" : "");
    if (h <= 0 || (size_t)h + blen > max) return false;
    memcpy(out + h, body, blen);
    *outn = (size_t)h + blen;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    memset(F, 0xFF, sizeof F);
    events_init();
    cfg_defaults(&s_cfg);
    http_init();
    for (int i = 0; i < 24; i++) if (P[i].used && P[i].listening && P[i].port == 80) s_listen = &P[i];
    struct tcp_pcb *cur = open80();
    void *tcur = NULL;
    static uint8_t req[16384];
    size_t at = 0;
    while (at < n) {
        uint8_t op = d[at++] % 10;
        size_t len = 0;
        const uint8_t *data = NULL;
        if (op == 1 || op == 2 || op == 9) {
            if (at + 2 > n) break;
            len = (size_t)d[at] | (size_t)d[at + 1] << 8;
            at += 2;
            if (len > n - at) len = n - at;
            data = d + at;
            at += len;
        }
        switch (op) {
        case 0: { struct tcp_pcb *c = open80(); if (c) cur = c; break; }
        case 1: deliver(cur, data, len); break;
        case 2: { size_t k; if (sign(data, len, req, &k, sizeof req)) deliver(cur, req, k); break; }
        case 3: if (cur && cur->used && !cur->gone && !cur->aborted && cur->rx) cur->rx(cur->arg, cur, NULL, ERR_OK); break;
        case 4: ack(cur); break;
        case 5: tick(at < n ? 1u + d[at++] * 40u : 500u); break;
        case 6: if (cur && cur->used && !cur->gone && !cur->aborted) { tcp_err_fn e = cur->err; void *a = cur->arg; cur->gone = true; if (e) e(a, ERR_ABRT); } break;
        case 7: if (at < n) { struct tcp_pcb *c = &P[d[at++] % 24]; if (c->used && !c->listening) cur = c; } break;
        case 8: for (int i = 0; i < 4; i++) if (!T[i].used) { memset(&T[i], 0, sizeof T[i]); T[i].used = 1; T[i].http = http_tls_open(&T[i]); tcur = &T[i]; break; } break;
        case 9: { struct tls *t = tcur; if (t && t->http && !t->closed) {
                    size_t want = http_tls_want(t->http);
                    http_tls_data(t->http, data, len < want ? len : want);
                    if (t->http) { t->out_n = 0; http_tls_sent(t->http); } } break; }
        }
    }
    for (int i = 0; i < 20; i++) { for (int k = 0; k < 24; k++) ack(&P[k]); tick(1000); }
    return 0;
}
