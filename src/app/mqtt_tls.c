/*
 * mqtt_tls.c — TLS under lwIP's MQTT client: an altcp layer over an altcp_tcp
 * connection (lwip/priv/altcp_priv.h), as lwIP's own altcp_tls_mbedtls.c, but with
 * the handshake run as a job so the ports never wait for it.
 *
 * MQTT is told "connected" only once the handshake is done, from mqtt_tls_poll() in
 * the main loop. After that every write is one record: sndbuf is the room the inner
 * connection has, less a record's overhead, and 0 while a record is still going out,
 * so a write never half succeeds. Received records are decrypted as they come and
 * passed up as plaintext; the TCP window opens as the ciphertext is read.
 */
#include "mqtt_tls.h"
#include "roots.h"
#include "worker.h"
#include "stm32h7xx_hal.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/priv/altcp_priv.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include <stdio.h>
#include <string.h>

#define SHAKE_MS 30000u
#define LOST     (-0x0050)               /* the connection went: our own code for mbedTLS */
#define CHUNK    1024u                   /* plaintext passed up at a time */

struct altcp_tls_config { char host[48]; };
static struct altcp_tls_config s_cfg;

enum { T_FREE, T_TCP, T_SHAKE, T_DONE, T_UP };   /* T_DONE: the job has ended, MQTT not told yet */

static struct {
    uint8_t phase;
    bool ok, stop, setup, eof;
    bool offered, saw_cert;              /* a kept session was offered; a certificate came (so not resumed) */
    bool aborted;                        /* inner was aborted inside one of its callbacks */
    struct altcp_pcb *conn;              /* what MQTT holds; NULL once it has let go */
    struct altcp_pcb *inner;             /* the TCP connection; NULL once gone */
    struct pbuf *q;                      /* ciphertext received, not yet read */
    uint32_t q_off, flush;               /* flush: length of a write to repeat (tls.c) */
    uint32_t t0;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
} s_t;

static const char *s_why = "";
static char s_whybuf[64], s_until[11], s_ver[12];
static uint32_t s_shakes, s_resumed;
static mbedtls_ssl_session s_sess;
static bool s_sess_init, s_have_sess;
static char s_sess_host[48], s_sess_until[11];

const char *mqtt_tls_error(void) { return s_why; }
const char *mqtt_tls_version(void) { return s_t.phase == T_UP ? s_ver : NULL; }
const char *mqtt_tls_until(void) { return s_t.phase == T_UP && s_until[0] ? s_until : NULL; }
uint32_t mqtt_tls_handshakes(void) { return s_shakes; }
uint32_t mqtt_tls_resumed(void) { return s_resumed; }

struct altcp_tls_config *mqtt_tls_config(const char *host)
{
    snprintf(s_cfg.host, sizeof s_cfg.host, "%s", host);
    return &s_cfg;
}

/* ---- to and from the TCP connection ---------------------------------------- */
static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    (void)ctx;
    if (!s_t.inner || s_t.stop) return LOST;
    uint32_t room = altcp_sndbuf(s_t.inner);
    if (n > room) n = room;
    if (!n || altcp_write(s_t.inner, b, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    altcp_output(s_t.inner);
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    size_t got = 0;
    (void)ctx;
    if (!s_t.inner || s_t.stop) return LOST;
    while (s_t.q && got < n) {
        struct pbuf *q = s_t.q;
        size_t k = q->len - s_t.q_off;
        if (k > n - got) k = n - got;
        memcpy(b + got, (const uint8_t *)q->payload + s_t.q_off, k);
        got += k;
        s_t.q_off += (uint32_t)k;
        if (s_t.q_off < q->len) break;
        s_t.q = q->next;
        s_t.q_off = 0;
        if (s_t.q) pbuf_ref(s_t.q);
        pbuf_free(q);
    }
    if (got) { altcp_recved(s_t.inner, (u16_t)got); return (int)got; }
    return s_t.eof ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
}

static void drop_queue(void)
{
    if (s_t.q) pbuf_free(s_t.q);
    s_t.q = NULL;
    s_t.q_off = 0;
}

static void free_ssl(void)
{
    if (s_t.setup) {
        mbedtls_ssl_free(&s_t.ssl);
        mbedtls_ssl_config_free(&s_t.conf);
        mbedtls_x509_crt_free(&s_t.roots);
        s_t.setup = false;
    }
    drop_queue();
}

static void unhook(struct altcp_pcb *in)
{
    altcp_arg(in, NULL);
    altcp_recv(in, NULL);
    altcp_sent(in, NULL);
    altcp_err(in, NULL);
    altcp_poll(in, NULL, 0);
}

/* The TCP connection goes; `hard`: at once. */
static void inner_off(bool hard)
{
    struct altcp_pcb *in = s_t.inner;
    if (!in) return;
    s_t.inner = NULL;
    unhook(in);
    if (hard || altcp_close(in) != ERR_OK) { altcp_abort(in); s_t.aborted = true; }
}

/* MQTT is told the connection is gone, and lets go of it. */
static void tell_err(err_t e)
{
    struct altcp_pcb *c = s_t.conn;
    if (!c) return;
    s_t.conn = NULL;
    if (c->err) c->err(c->arg, e);
    altcp_free(c);
}

/* Nothing left that uses the context: free it. */
static void reap(void)
{
    if (s_t.phase != T_FREE && !s_t.conn && !s_t.inner && !worker_busy(WORKER_MQTT)) {
        free_ssl();
        s_t.phase = T_FREE;
    }
}

/* ---- the handshake job -------------------------------------------------------- */
static const int SUITES[] = { MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                              MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0 };

static int note_until(void *p, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)p; (void)flags;
    s_t.saw_cert = true;
    if (depth == 0)
        snprintf(s_until, sizeof s_until, "%04u-%02u-%02u", (unsigned)crt->valid_to.year % 10000u,
                 (unsigned)crt->valid_to.mon % 100u, (unsigned)crt->valid_to.day % 100u);
    return 0;
}

static const char *reason(int r)
{
    uint32_t v = s_t.setup ? mbedtls_ssl_get_verify_result(&s_t.ssl) : 0;
    if (v && v != (uint32_t)-1)
        return v & MBEDTLS_X509_BADCERT_EXPIRED ? "Its certificate has expired" :
               v & MBEDTLS_X509_BADCERT_FUTURE ? "Its certificate is not valid yet: is the time right?" :
               v & MBEDTLS_X509_BADCERT_CN_MISMATCH ? "Its certificate is for another name" :
               "Its certificate is not trusted";
    if (r == LOST || r == MBEDTLS_ERR_SSL_CONN_EOF) return "The broker closed the connection during the TLS handshake";
    if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) return "The broker refused the TLS handshake";
    if (r == MBEDTLS_ERR_SSL_ALLOC_FAILED) return "Not enough memory for TLS";
    snprintf(s_whybuf, sizeof s_whybuf, "TLS handshake failed (-0x%04x)", (unsigned)-r);
    return s_whybuf;
}

static bool setup(void)
{
    mbedtls_ssl_init(&s_t.ssl);
    mbedtls_ssl_config_init(&s_t.conf);
    mbedtls_x509_crt_init(&s_t.roots);
    s_t.setup = true;
    for (unsigned i = 0; i < g_nroots; i++)
        if (mbedtls_x509_crt_parse_der_nocopy(&s_t.roots, g_roots[i].der, g_roots[i].len)) { s_why = "A root certificate is unreadable"; return false; }
    if (mbedtls_ssl_config_defaults(&s_t.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) {
        s_why = "Not enough memory for TLS";
        return false;
    }
    mbedtls_ssl_conf_authmode(&s_t.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s_t.conf, &s_t.roots, NULL);
    mbedtls_ssl_conf_ciphersuites(&s_t.conf, SUITES);
    mbedtls_ssl_conf_verify(&s_t.conf, note_until, NULL);
    if (mbedtls_ssl_setup(&s_t.ssl, &s_t.conf) || mbedtls_ssl_set_hostname(&s_t.ssl, s_cfg.host)) {
        s_why = "Not enough memory for TLS";
        return false;
    }
    mbedtls_ssl_set_bio(&s_t.ssl, NULL, bio_send, bio_recv, NULL);
    if (s_have_sess && !strcmp(s_sess_host, s_cfg.host)) s_t.offered = !mbedtls_ssl_set_session(&s_t.ssl, &s_sess);
    return true;
}

static void shake(void *arg)
{
    int r = -1;
    (void)arg;
    if (setup()) {
        while ((r = mbedtls_ssl_handshake(&s_t.ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (s_t.stop) break;
            if (HAL_GetTick() - s_t.t0 > SHAKE_MS) { s_why = "The TLS handshake timed out"; break; }
            worker_yield();
        }
        if (!r && !s_t.stop) {
            s_t.ok = true;
            snprintf(s_ver, sizeof s_ver, "%s", mbedtls_ssl_get_version(&s_t.ssl));
            if (s_t.offered && !s_t.saw_cert) {          /* resumed: the certificate is the one before */
                s_resumed++;
                memcpy(s_until, s_sess_until, sizeof s_until);
            } else {
                memcpy(s_sess_until, s_until, sizeof s_sess_until);
            }
            if (!s_sess_init) { mbedtls_ssl_session_init(&s_sess); s_sess_init = true; }
            mbedtls_ssl_session_free(&s_sess);
            mbedtls_ssl_session_init(&s_sess);
            s_have_sess = !mbedtls_ssl_get_session(&s_t.ssl, &s_sess);
            snprintf(s_sess_host, sizeof s_sess_host, "%s", s_cfg.host);
        } else if (!s_why[0]) {
            s_why = s_t.stop ? "Stopped" : reason(r);
        }
    }
    if (!s_t.ok && s_have_sess) {                 /* a failed resumption should not repeat */
        mbedtls_ssl_session_free(&s_sess);
        mbedtls_ssl_session_init(&s_sess);
        s_have_sess = false;
    }
    s_t.phase = T_DONE;
}

/* ---- records ------------------------------------------------------------------ */
/* A record still going out goes on. True when none is left. */
static bool flushed(void)
{
    static const uint8_t none[1];
    if (!s_t.flush) return true;
    int r = mbedtls_ssl_write(&s_t.ssl, none, s_t.flush);   /* already encrypted; not looked at */
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) return false;
    s_t.flush = 0;
    return r >= 0;
}

/* Decrypts what has come and passes it up; then the end, if the broker closed. */
static void pass_up(void)
{
    while (s_t.conn && s_t.phase == T_UP) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, CHUNK, PBUF_RAM);
        if (!p) return;                          /* try again when more comes */
        int r = mbedtls_ssl_read(&s_t.ssl, p->payload, CHUNK);
        if (r > 0) {
            pbuf_realloc(p, (u16_t)r);
            struct altcp_pcb *c = s_t.conn;
            if (!c->recv) { pbuf_free(p); continue; }
            if (c->recv(c->arg, c, p, ERR_OK) != ERR_OK) pbuf_free(p);
            continue;
        }
        pbuf_free(p);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) {
            struct altcp_pcb *c = s_t.conn;
            if (c->recv) c->recv(c->arg, c, NULL, ERR_OK);      /* MQTT closes */
            else { inner_off(false); tell_err(ERR_CLSD); }
            return;
        }
        snprintf(s_whybuf, sizeof s_whybuf, "TLS error from the broker (-0x%04x)", (unsigned)-r);
        s_why = s_whybuf;
        inner_off(true);
        tell_err(ERR_ABRT);
        return;
    }
}

/* ---- callbacks from the TCP connection ------------------------------------------ */
static err_t low_recv(void *arg, struct altcp_pcb *in, struct pbuf *p, err_t err)
{
    (void)arg; (void)err;
    if (in != s_t.inner) { if (p) { altcp_recved(in, p->tot_len); pbuf_free(p); } return ERR_OK; }
    s_t.aborted = false;
    if (!p) s_t.eof = true;
    else if (s_t.q) pbuf_cat(s_t.q, p);
    else { s_t.q = p; s_t.q_off = 0; }
    if (s_t.phase == T_UP) pass_up();
    reap();
    return s_t.aborted ? ERR_ABRT : ERR_OK;
}

static err_t low_sent(void *arg, struct altcp_pcb *in, u16_t len)
{
    (void)arg; (void)in;
    s_t.aborted = false;
    if (s_t.phase == T_UP && flushed() && s_t.conn && s_t.conn->sent) s_t.conn->sent(s_t.conn->arg, s_t.conn, len);
    reap();
    return s_t.aborted ? ERR_ABRT : ERR_OK;
}

static err_t low_poll(void *arg, struct altcp_pcb *in)
{
    (void)arg; (void)in;
    s_t.aborted = false;
    if (s_t.phase == T_UP) {
        flushed();
        if (s_t.conn && s_t.conn->poll) s_t.conn->poll(s_t.conn->arg, s_t.conn);
    }
    reap();
    return s_t.aborted ? ERR_ABRT : ERR_OK;
}

static void low_err(void *arg, err_t err)
{
    (void)arg;
    s_t.inner = NULL;                            /* lwIP has freed it */
    s_t.stop = true;
    if (s_t.phase == T_SHAKE && !s_why[0]) s_why = "The broker closed the connection during the TLS handshake";
    tell_err(err);
    reap();
}

static err_t low_connected(void *arg, struct altcp_pcb *in, err_t err)
{
    (void)arg; (void)in;
    if (err != ERR_OK) return err;
    s_t.phase = T_SHAKE;
    s_t.t0 = HAL_GetTick();
    if (!worker_start(WORKER_MQTT, shake, NULL)) { s_why = "TLS is busy"; s_t.phase = T_DONE; }
    return ERR_OK;
}

/* ---- the connection MQTT holds -------------------------------------------------- */
static void t_set_poll(struct altcp_pcb *c, u8_t interval) { (void)c; if (s_t.inner) altcp_poll(s_t.inner, low_poll, interval); }
static void t_recved(struct altcp_pcb *c, u16_t len) { (void)c; (void)len; }   /* opened as read (bio_recv) */

static err_t t_connect(struct altcp_pcb *c, const ip_addr_t *ip, u16_t port, altcp_connected_fn connected)
{
    if (!s_t.inner) return ERR_CLSD;
    c->connected = connected;
    return altcp_connect(s_t.inner, ip, port, low_connected);
}

static void t_abort(struct altcp_pcb *c)
{
    if (c == s_t.conn) s_t.conn = NULL;
    s_t.stop = true;
    inner_off(true);
    altcp_free(c);
    reap();
}

static err_t t_close(struct altcp_pcb *c)
{
    if (c == s_t.conn) s_t.conn = NULL;
    s_t.stop = true;
    if (s_t.phase == T_UP && s_t.inner && flushed()) mbedtls_ssl_close_notify(&s_t.ssl);
    inner_off(false);
    altcp_free(c);
    reap();
    return ERR_OK;
}

static err_t t_write(struct altcp_pcb *c, const void *d, u16_t len, u8_t flags)
{
    (void)c; (void)flags;
    if (s_t.phase != T_UP || !s_t.inner) return ERR_CONN;
    if (!flushed()) return ERR_MEM;
    if (!len) return ERR_OK;
    int r = mbedtls_ssl_write(&s_t.ssl, d, len);
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) { s_t.flush = len; return ERR_OK; }   /* encrypted; goes out later */
    if (r == len) return ERR_OK;
    return ERR_ABRT;                            /* not one whole record (sndbuf prevents it), or broken */
}

static err_t t_output(struct altcp_pcb *c)
{
    (void)c;
    if (!s_t.inner) return ERR_CONN;
    flushed();
    return altcp_output(s_t.inner);
}

/* Room for one whole record: the plaintext a write may give now. */
static u16_t t_sndbuf(struct altcp_pcb *c)
{
    (void)c;
    if (s_t.phase != T_UP || !s_t.inner || s_t.flush) return 0;
    if (altcp_sndqueuelen(s_t.inner) >= TCP_SND_QUEUELEN - 4) return 0;
    int over = mbedtls_ssl_get_record_expansion(&s_t.ssl);
    uint32_t room = altcp_sndbuf(s_t.inner);
    if (over < 0 || room <= (uint32_t)over) return 0;
    room -= (uint32_t)over;
    int most = mbedtls_ssl_get_max_out_record_payload(&s_t.ssl);
    if (most > 0 && room > (uint32_t)most) room = (uint32_t)most;
    return (u16_t)room;
}

static u16_t t_mss(struct altcp_pcb *c) { (void)c; return s_t.inner ? altcp_mss(s_t.inner) : 0; }

static void t_dealloc(struct altcp_pcb *c) { (void)c; }

static const struct altcp_functions FNS = {
    t_set_poll, t_recved, altcp_default_bind, t_connect, NULL, t_abort, t_close, altcp_default_shutdown,
    t_write, t_output, t_mss, t_sndbuf, altcp_default_sndqueuelen, altcp_default_nagle_disable,
    altcp_default_nagle_enable, altcp_default_nagle_disabled, altcp_default_setprio, t_dealloc,
    altcp_default_get_tcp_addrinfo, altcp_default_get_ip, altcp_default_get_port,
#if LWIP_TCP_KEEPALIVE
    altcp_default_keepalive_disable, altcp_default_keepalive_enable,
#endif
#ifdef LWIP_DEBUG
    altcp_default_dbg_get_tcp_state,
#endif
};

/* lwIP's altcp_tls_new() makes the TCP connection and asks for this around it. */
struct altcp_pcb *altcp_tls_wrap(struct altcp_tls_config *config, struct altcp_pcb *in)
{
    if (config != &s_cfg || s_t.phase != T_FREE || !in) return NULL;   /* one at a time */
    struct altcp_pcb *c = altcp_alloc();
    if (!c) return NULL;
    memset(&s_t, 0, sizeof s_t);
    s_why = "";
    s_until[0] = 0;
    s_t.phase = T_TCP;
    s_t.conn = c;
    s_t.inner = in;
    c->inner_conn = in;
    c->fns = &FNS;
    altcp_arg(in, c);
    altcp_recv(in, low_recv);
    altcp_sent(in, low_sent);
    altcp_err(in, low_err);
    return c;
}

/* ---- main loop ------------------------------------------------------------------ */
void mqtt_tls_poll(void)
{
    worker_poll(WORKER_MQTT);
    if (s_t.phase == T_DONE && !worker_busy(WORKER_MQTT)) {
        if (s_t.ok && s_t.conn && s_t.inner) {
            s_t.phase = T_UP;
            s_shakes++;
            struct altcp_pcb *c = s_t.conn;
            if (c->connected) c->connected(c->arg, c, ERR_OK);       /* MQTT sends CONNECT */
            if (s_t.conn == c && (s_t.q || s_t.eof)) pass_up();      /* what came meanwhile */
        } else {
            if (!s_why[0]) s_why = "The TLS handshake failed";
            inner_off(true);
            tell_err(ERR_ABRT);
        }
    }
    reap();
}
