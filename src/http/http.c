/*
 * http.c — Minimal HTTP/1.0-style control server (lwIP raw API), built into
 * both the loader and the app (APP_IMAGE selects the endpoints).
 *
 *   GET  /status                      JSON, open
 *   GET  /nonce                       32 hex chars, open
 *   GET  /flash/<0|1>?off=N&len=N     raw external flash      (auth)
 *   POST /reboot                                              (auth)
 * loader only:
 *   POST /run                         image -> SRAM, jump     (auth)
 *   POST /install                     image -> app slot       (auth)
 *   POST /selfupdate                  replace the loader      (auth)
 * app only:
 *   GET  /                            web UI, open
 *   GET  /api/{state,config,login}    JSON, open
 *   POST /api/...                     see api.c               (auth)
 *   POST /loader                      reboot into the loader  (auth)
 *   POST /confirm                     keep the running image  (auth)
 *
 * Auth: X-Auth = hex HMAC-SHA256(key, "nonce\nMETHOD path\nbodysha256hex").
 * The nonce is single-use, and the MAC is checked before any body is accepted;
 * the body hash (X-Body-SHA256) is checked before it is acted on. The key never
 * crosses the wire. Replies are not encrypted. In the app, /api/ and /reboot
 * also accept the key derived from the web password.
 */
#include "http.h"
#include "net.h"
#include "eth_mac.h"
#include "extflash.h"
#include "sha256.h"
#include "system.h"
#include "fw_id.h"
#include "slots.h"
#include "secret_key.h"
#include "lwip/tcp.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef APP_IMAGE
#include "api.h"
#define MAX_CONN   8
#define RESP_MAX   4600
#define BODY_MAX   2048
#define CONN_MEM   __attribute__((section(".d2_bss")))
#else
#define MAX_CONN   4
#define RESP_MAX   1460
#define CONN_MEM
#endif
#define HDR_MAX    1024
#define IDLE_TICKS 20            /* tcp_poll ticks (x 1 s) before abort */

enum { ST_FREE, ST_HDR, ST_BODY, ST_SEND };
enum { ACT_NONE, ACT_RUN, ACT_REBOOT, ACT_SELFUPDATE,
       ACT_INSTALL, ACT_LOADER, ACT_CONFIRM, ACT_API };

typedef struct {
    struct tcp_pcb *pcb;
    uint8_t  state, action, idle;
    char     hdr[HDR_MAX + 1];
    uint32_t hdr_len, body_len, body_got;
    uint8_t  body_sha[32];
    sha256_t sha;
    char     resp[RESP_MAX];
    uint32_t resp_len, resp_off;
    int      fl_dev;
    uint32_t fl_off, fl_left;
    const uint8_t *mem;          /* static data still to send */
    uint32_t mem_left;
#ifdef APP_IMAGE
    char     path[32], nonce[33];
    const uint8_t *key;
    char     body[BODY_MAX];
#endif
} conn_t;

static CONN_MEM conn_t s_conn[MAX_CONN];
static conn_t  *s_stage_owner;   /* connection currently filling the stage */
static char     s_nonce[33];
static uint8_t  s_action;
static uint32_t s_action_at, s_stage_size;
extern uint32_t g_reset_cause;
extern bool     g_clock_hse;

#ifdef APP_IMAGE
#define MODE "app"
#define RUNNING_SLOT ((int)bkp_get(BKP_SLOT))
#else
#define MODE "loader"
#define RUNNING_SLOT SLOT_NONE
#endif

/* ---- helpers ------------------------------------------------------------- */
static void hexify(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = d[in[i] >> 4]; out[2*i+1] = d[in[i] & 15]; }
    out[2*n] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool unhex32(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        int a = hexval(s[2*i]);
        int b = a < 0 ? -1 : hexval(s[2*i+1]);
        if (b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

/* Value of header `name`, or NULL. Headers are NUL-terminated in c->hdr. */
static const char *header(const conn_t *c, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = strstr(c->hdr, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 32) == (name[i] | 32)) i++;
        if (i == n && p[n] == ':') {
            p += n + 1;
            while (*p == ' ') p++;
            return p;
        }
    }
    return NULL;
}

static bool new_nonce(void)
{
    uint8_t r[16];
    if (!rng_fill(r, sizeof r)) { s_nonce[0] = 0; return false; }
    hexify(r, sizeof r, s_nonce);
    return true;
}

#ifdef APP_IMAGE
/* Slows online guessing of the web password. */
static uint32_t s_fails, s_fail_at;

static bool web_key_allowed(const char *path)
{
    return !strncmp(path, "/api/", 5) || !strcmp(path, "/reboot");
}
#endif

static bool authorised(conn_t *c, const char *method, const char *path)
{
    const char *mac_h = header(c, "X-Auth"), *sha_h = header(c, "X-Body-SHA256");
    uint8_t got[32], want[32];
    char msg[256], shahex[65];
    if (!s_nonce[0] || !mac_h || !sha_h) return false;
    if (!unhex32(mac_h, got) || !unhex32(sha_h, c->body_sha)) return false;
    hexify(c->body_sha, 32, shahex);
    int n = snprintf(msg, sizeof msg, "%s\n%s %s\n%s", s_nonce, method, path, shahex);
    if (n <= 0 || n >= (int)sizeof msg) return false;
    const uint8_t *key = SECRET_KEY;
    hmac_sha256(key, msg, (size_t)n, NULL, 0, want);
    bool ok = ct_equal(got, want, 32);
#ifdef APP_IMAGE
    if (HAL_GetTick() - s_fail_at > 5000) s_fails = 0;
    if (!ok && s_fails < 8 && web_key_allowed(path) && (key = api_web_key())) {
        hmac_sha256(key, msg, (size_t)n, NULL, 0, want);
        ok = ct_equal(got, want, 32);
    }
    if (!ok) { s_fails++; s_fail_at = HAL_GetTick(); }
    c->key = key;
    strcpy(c->nonce, s_nonce);
#endif
    if (!ok) return false;
    s_nonce[0] = 0;              /* single use */
    return true;
}

/* ---- connection lifecycle ------------------------------------------------ */
static void conn_free(conn_t *c)
{
    if (s_stage_owner == c) s_stage_owner = NULL;
    c->state = ST_FREE;
    c->pcb = NULL;
}

/* Returns ERR_ABRT if the pcb had to be aborted; callbacks must propagate it. */
static err_t conn_close(conn_t *c)
{
    struct tcp_pcb *pcb = c->pcb;
    uint8_t act = c->action;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    conn_free(c);
    if (act != ACT_NONE) { s_action = act; s_action_at = HAL_GetTick() + 300; }
    if (tcp_close(pcb) != ERR_OK) { tcp_abort(pcb); return ERR_ABRT; }
    return ERR_OK;
}

static err_t pump(conn_t *c)
{
    while (c->resp_off < c->resp_len) {
        uint32_t n = c->resp_len - c->resp_off, room = tcp_sndbuf(c->pcb);
        if (n > room) n = room;
        if (!n || tcp_write(c->pcb, c->resp + c->resp_off, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
            goto out;
        c->resp_off += n;
    }
    while (c->fl_left) {
        static uint8_t buf[TCP_MSS];
        uint32_t n = c->fl_left < sizeof buf ? c->fl_left : sizeof buf;
        if (tcp_sndbuf(c->pcb) < n || tcp_sndqueuelen(c->pcb) >= TCP_SND_QUEUELEN - 4) goto out;
        extflash_read(c->fl_dev, c->fl_off, buf, n);
        if (tcp_write(c->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) goto out;
        c->fl_off += n;
        c->fl_left -= n;
    }
    while (c->mem_left) {
        uint32_t n = c->mem_left, room = tcp_sndbuf(c->pcb);
        if (n > TCP_MSS) n = TCP_MSS;
        if (n > room) n = room;
        if (!n || tcp_write(c->pcb, c->mem, (u16_t)n, 0) != ERR_OK) goto out;
        c->mem += n;
        c->mem_left -= n;
    }
    tcp_output(c->pcb);
    return conn_close(c);
out:
    tcp_output(c->pcb);
    return ERR_OK;
}

static err_t reply(conn_t *c, const char *status, const char *type, const char *body)
{
    if (s_stage_owner == c && c->action == ACT_NONE) s_stage_owner = NULL;
    c->state = ST_SEND;
    c->resp_off = 0;
    c->resp_len = (uint32_t)snprintf(c->resp, sizeof c->resp,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n%s",
        status, type, (unsigned)strlen(body), body);
    if (c->resp_len >= sizeof c->resp) {     /* never send a truncated body */
        c->resp_len = (uint32_t)snprintf(c->resp, sizeof c->resp,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
    return pump(c);
}

static err_t fail(conn_t *c, const char *status, const char *why)
{
    char b[96];
    snprintf(b, sizeof b, "%s\n", why);
    return reply(c, status, "text/plain", b);
}

/* ---- endpoints ----------------------------------------------------------- */
static err_t do_status(conn_t *c)
{
    char ip[16], b[1300];
    slot_info_t sl[SLOT_COUNT];
    for (int i = 0; i < SLOT_COUNT; i++) slots_get(i, &sl[i]);
    net_ip_str(ip);
    snprintf(b, sizeof b,
        "{\"mode\":\"" MODE "\",\"version\":\"%s\",\"link_addr\":\"0x%08lx\",\"host\":\"%s\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip\":\"%s\",\"uptime_ms\":%lu,"
        "\"clock\":\"%s\",\"sysclk\":%lu,\"reset_cause\":\"0x%08lx\",\"phy_addr\":%lu,"
        "\"phy_id\":\"0x%08lx\",\"speed\":%d,\"full_duplex\":%s,"
        "\"rx\":%lu,\"tx\":%lu,\"rx_err\":%lu,\"tx_drop\":%lu,"
        "\"flash0\":\"0x%06lx\",\"flash1\":\"0x%06lx\",\"crashes\":%lu,\"boots\":%lu,"
        "\"reset_log\":[\"%lx\",\"%lx\",\"%lx\",\"%lx\",\"%lx\",\"%lx\"],\"running_slot\":%d,"
        "\"slots\":[{\"valid\":%s,\"seq\":%lu,\"version\":\"%s\",\"confirmed\":%s,\"tries\":%d},"
        "{\"valid\":%s,\"seq\":%lu,\"version\":\"%s\",\"confirmed\":%s,\"tries\":%d}]}\n",
        g_fw_id.version, g_fw_id.link_addr, g_hostname,
        g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5], ip,
        (unsigned long)HAL_GetTick(), g_clock_hse ? "HSE" : "HSI",
        (unsigned long)SystemCoreClock, (unsigned long)g_reset_cause,
        (unsigned long)g_eth.phy_addr, (unsigned long)g_eth.phy_id,
        g_eth.speed100 ? 100 : 10, g_eth.full_duplex ? "true" : "false",
        (unsigned long)g_eth.rx_frames, (unsigned long)g_eth.tx_frames,
        (unsigned long)g_eth.rx_errors, (unsigned long)g_eth.tx_drops,
        (unsigned long)extflash_id(0), (unsigned long)extflash_id(1),
        (unsigned long)bkp_get(BKP_CRASHES), (unsigned long)bkp_get(BKP_BOOTS),
        (unsigned long)bkp_get(BKP_RSTLOG), (unsigned long)bkp_get(BKP_RSTLOG + 1),
        (unsigned long)bkp_get(BKP_RSTLOG + 2), (unsigned long)bkp_get(BKP_RSTLOG + 3),
        (unsigned long)bkp_get(BKP_RSTLOG + 4), (unsigned long)bkp_get(BKP_RSTLOG + 5),
        RUNNING_SLOT,
#define SLOT_ARGS(s) (s).valid ? "true" : "false", (s).valid ? (unsigned long)(s).hdr.seq : 0ul, \
        (s).valid ? (s).hdr.version : "", (s).confirmed ? "true" : "false", (s).tries_left
        SLOT_ARGS(sl[0]), SLOT_ARGS(sl[1]));
    return reply(c, "200 OK", "application/json", b);
}

static bool query_u32(const char *path, const char *key, uint32_t *out)
{
    const char *p = strstr(path, key);
    char *end;
    if (!p) return false;
    p += strlen(key);
    *out = strtoul(p, &end, 0);
    return end != p;
}

static err_t do_flash(conn_t *c, const char *path)
{
    uint32_t off, len;
    int dev = path[7] - '0';
    if (dev < 0 || dev >= EXTFLASH_COUNT || path[8] != '?' ||
        !query_u32(path, "off=", &off) || !query_u32(path, "len=", &len) ||
        off >= EXTFLASH_SIZE || len == 0 || len > EXTFLASH_SIZE - off)
        return fail(c, "400 Bad Request", "need /flash/<0|1>?off=&len=");
    c->state = ST_SEND;
    c->fl_dev = dev;
    c->fl_off = off;
    c->fl_left = len;
    c->resp_off = 0;
    c->resp_len = (uint32_t)snprintf(c->resp, sizeof c->resp,
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: %lu\r\nConnection: close\r\n\r\n", (unsigned long)len);
    return pump(c);
}

#ifdef APP_IMAGE
static err_t do_page(conn_t *c)
{
    c->mem = api_page(&c->mem_left);
    c->state = ST_SEND;
    c->resp_off = 0;
    c->resp_len = (uint32_t)snprintf(c->resp, sizeof c->resp,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\n"
        "Content-Length: %lu\r\nCache-Control: no-cache\r\nX-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'\r\n"
        "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", (unsigned long)c->mem_left);
    return pump(c);
}
#endif

static err_t body_done(conn_t *c)
{
    uint8_t d[32];
    char ok[24] = "ok\n";
    const char *why;
    sha256_final(&c->sha, d);
    if (!ct_equal(d, c->body_sha, 32)) {
        c->action = ACT_NONE;
        return fail(c, "400 Bad Request", "body hash mismatch");
    }
#ifdef APP_IMAGE
    if (c->action == ACT_API) {
        static char msg[160];
        api_req_t rq = { c->key == SECRET_KEY, c->key, c->nonce, c->body };
        c->action = ACT_NONE;
        c->body[c->body_len] = 0;
        const char *st = api_post(c->path, &rq, msg, sizeof msg);
        return reply(c, st, st[0] == '2' ? "application/json" : "text/plain", msg);
    }
#endif
    s_stage_size = c->body_len;
    if (c->action == ACT_RUN) why = image_check_ram(STAGE_ADDR, s_stage_size);
    else if (c->action == ACT_SELFUPDATE) why = image_check_loader((const uint8_t *)STAGE_ADDR, s_stage_size);
    else if (c->action == ACT_INSTALL) {
        int slot = SLOT_NONE;
        why = image_check_app(s_stage_size);
        if (!why) why = slots_install(s_stage_size, &slot);
        if (!why) {
            bkp_set(BKP_CRASHES, 0);
            snprintf(ok, sizeof ok, "ok slot=%d\n", slot);
        }
    }
    else if (c->action == ACT_CONFIRM) why = slots_confirm(RUNNING_SLOT) ? NULL : "confirm failed";
    else why = NULL;
    if (why) {
        c->action = ACT_NONE;
        return fail(c, "400 Bad Request", why);
    }
    if (c->action == ACT_INSTALL || c->action == ACT_CONFIRM) {
        c->action = ACT_NONE;        /* already done; nothing deferred */
        s_stage_owner = NULL;
    }
    return reply(c, "200 OK", "text/plain", ok);
}

static err_t on_request(conn_t *c)
{
    static char line[200];
    const char *eol = strstr(c->hdr, "\r\n");
    char *path, *end;
    if (!eol || eol - c->hdr >= (int)sizeof line) return fail(c, "400 Bad Request", "bad request line");
    memcpy(line, c->hdr, (size_t)(eol - c->hdr));
    line[eol - c->hdr] = 0;
    if (!(path = strchr(line, ' '))) return fail(c, "400 Bad Request", "bad request line");
    *path++ = 0;
    if ((end = strchr(path, ' '))) *end = 0;

    const char *cl = header(c, "Content-Length");
    c->body_len = cl ? strtoul(cl, NULL, 10) : 0;

    if (!strcmp(line, "GET")) {
#ifdef APP_IMAGE
        if (!strcmp(path, "/")) return do_page(c);
        if (!strncmp(path, "/api/", 5)) {
            static char js[RESP_MAX - 256];
            int n = api_get(path, js, sizeof js);
            if (n == 0) return fail(c, "500 Internal Server Error", "reply too large");
            if (n > 0) return reply(c, "200 OK", "application/json", js);
        }
#else
        if (!strcmp(path, "/")) return do_status(c);
#endif
        if (!strcmp(path, "/status")) return do_status(c);
        if (!strcmp(path, "/nonce")) {
            char b[40];
            if (!s_nonce[0] && !new_nonce()) return fail(c, "503 Service Unavailable", "rng failure");
            snprintf(b, sizeof b, "%s\n", s_nonce);
            return reply(c, "200 OK", "text/plain", b);
        }
        if (!strncmp(path, "/flash/", 7))
            return authorised(c, line, path) ? do_flash(c, path)
                                             : fail(c, "401 Unauthorized", "bad auth");
        return fail(c, "404 Not Found", "no such endpoint");
    }
    if (strcmp(line, "POST")) return fail(c, "405 Method Not Allowed", "GET or POST only");

    static const struct { const char *path; uint8_t act; uint32_t max; } POSTS[] = {
        { "/reboot",     ACT_REBOOT,     0 },
#ifdef APP_IMAGE
        { "/loader",     ACT_LOADER,     0 },
        { "/confirm",    ACT_CONFIRM,    0 },
#else
        { "/run",        ACT_RUN,        STAGE_SIZE },
        { "/install",    ACT_INSTALL,    STAGE_SIZE },
        { "/selfupdate", ACT_SELFUPDATE, 128u * 1024u },
#endif
    };
    uint8_t act = ACT_NONE;
    uint32_t max = 0;
    for (unsigned i = 0; i < sizeof POSTS / sizeof POSTS[0]; i++)
        if (!strcmp(path, POSTS[i].path)) { act = POSTS[i].act; max = POSTS[i].max; }
#ifdef APP_IMAGE
    if (api_is_post(path)) { act = ACT_API; max = BODY_MAX - 1; }
#endif
    if (act == ACT_NONE) return fail(c, "404 Not Found", "no such endpoint");
    if (c->body_len > max) return fail(c, "413 Payload Too Large", "too big");
    if (max && act != ACT_API && s_stage_owner) return fail(c, "409 Conflict", "upload in progress");
    if (s_action != ACT_NONE) return fail(c, "409 Conflict", "busy");
    if (!authorised(c, line, path)) return fail(c, "401 Unauthorized", "bad auth");

    c->action = act;
    c->body_got = 0;
    sha256_init(&c->sha);
#ifdef APP_IMAGE
    snprintf(c->path, sizeof c->path, "%s", path);
#endif
    if (max && act != ACT_API) s_stage_owner = c;
    if (c->body_len == 0) return body_done(c);
    c->state = ST_BODY;
    return ERR_OK;
}

/* ---- lwIP callbacks ------------------------------------------------------ */
static err_t on_body(conn_t *c, const uint8_t *p, uint32_t n)
{
    uint32_t want = c->body_len - c->body_got;
    if (n > want) n = want;
#ifdef APP_IMAGE
    memcpy(c->body + c->body_got, p, n);   /* STAGE_ADDR is our own code here */
#else
    memcpy((uint8_t *)STAGE_ADDR + c->body_got, p, n);
#endif
    sha256_update(&c->sha, p, n);
    c->body_got += n;
    return c->body_got == c->body_len ? body_done(c) : ERR_OK;
}

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = arg;
    (void)err;
    if (!c) { if (p) { tcp_recved(pcb, p->tot_len); pbuf_free(p); } return ERR_OK; }
    if (!p) {
        if (c->state == ST_SEND) return ERR_OK;   /* half-close: finish the reply */
        c->action = ACT_NONE;
        return conn_close(c);
    }
    c->idle = 0;
    tcp_recved(pcb, p->tot_len);

    err_t r = ERR_OK;
    for (struct pbuf *q = p; q && r == ERR_OK; q = q->next) {
        const uint8_t *d = q->payload;
        uint32_t n = q->len;
        if (c->state == ST_HDR) {
            uint32_t i = 0;
            char *e = NULL;
            while (i < n && c->hdr_len < HDR_MAX) {
                c->hdr[c->hdr_len++] = (char)d[i++];
                if (c->hdr_len >= 4 && !memcmp(c->hdr + c->hdr_len - 4, "\r\n\r\n", 4)) {
                    e = c->hdr + c->hdr_len;
                    break;
                }
            }
            c->hdr[c->hdr_len] = 0;
            if (e) r = on_request(c);
            else if (c->hdr_len >= HDR_MAX) r = fail(c, "431 Headers Too Large", "headers too large");
            d += i;
            n -= i;
        }
        if (r == ERR_OK && n && c->state == ST_BODY) r = on_body(c, d, n);
        if (c->state == ST_FREE) break;
    }
    pbuf_free(p);
    return r;
}

static err_t cb_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    conn_t *c = arg;
    (void)pcb; (void)len;
    if (!c || c->state != ST_SEND) return ERR_OK;
    c->idle = 0;
    return pump(c);
}

static err_t cb_poll(void *arg, struct tcp_pcb *pcb)
{
    conn_t *c = arg;
    if (!c) return ERR_OK;
    if (++c->idle > IDLE_TICKS) {
        tcp_err(pcb, NULL);
        conn_free(c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return c->state == ST_SEND ? pump(c) : ERR_OK;
}

static void cb_err(void *arg, err_t err)
{
    (void)err;
    if (arg) conn_free(arg);      /* pcb is already gone */
}

static err_t cb_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    for (int i = 0; i < MAX_CONN; i++) {
        conn_t *c = &s_conn[i];
        if (c->state != ST_FREE) continue;
        memset(c, 0, sizeof *c);
        c->state = ST_HDR;
        c->pcb = pcb;
        tcp_arg(pcb, c);
        tcp_recv(pcb, cb_recv);
        tcp_sent(pcb, cb_sent);
        tcp_err(pcb, cb_err);
        tcp_poll(pcb, cb_poll, 2);    /* 2 x 500 ms */
        tcp_nagle_disable(pcb);
        return ERR_OK;
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

void http_init(void)
{
    memset(s_conn, 0, sizeof s_conn);
    struct tcp_pcb *l = tcp_new();
    if (!l || tcp_bind(l, IP_ADDR_ANY, 80) != ERR_OK) return;
    tcp_accept(tcp_listen_with_backlog(l, MAX_CONN), cb_accept);
}

void http_poll(void)
{
    if (s_action == ACT_NONE || (int32_t)(HAL_GetTick() - s_action_at) < 0) return;
    switch (s_action) {
#ifndef APP_IMAGE
    case ACT_RUN:
        bkp_set(BKP_SLOT, (uint32_t)SLOT_NONE);
        jump_to_image(STAGE_ADDR);
    case ACT_SELFUPDATE:
        bkp_set(BKP_STAY, BKP_STAY_MAGIC);      /* come back up in the new loader */
        self_flash((const uint8_t *)STAGE_ADDR, s_stage_size);
#endif
    case ACT_LOADER:
        bkp_set(BKP_STAY, BKP_STAY_MAGIC);
        __attribute__((fallthrough));
    default:
#ifdef APP_IMAGE
        s_action = ACT_NONE;                    /* api_restart() still polls the network */
        api_restart();
#endif
        bkp_set(BKP_CRASHES, 0);                /* deliberate, not a crash */
        NVIC_SystemReset();
    }
}
