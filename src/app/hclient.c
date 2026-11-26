#include "hclient.h"
#include "roots.h"
#include "worker.h"
#include "clock.h"
#include "stm32h7xx_hal.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WAIT_MS 30000u
#define HC_LOST (-0x0050)

static struct {
    struct tcp_pcb *pcb;
    struct pbuf *q;
    uint32_t q_off;
    bool up, eof, lost, setup;
    char host[64];
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt roots;
} s_c;
static uint32_t s_shakes;

uint32_t hc_handshakes(void) { return s_shakes; }

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    (void)ctx;
    if (s_c.lost || !s_c.pcb) return HC_LOST;
    uint32_t room = tcp_sndbuf(s_c.pcb);
    if (n > room) n = room;
    if (!n || tcp_write(s_c.pcb, b, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    tcp_output(s_c.pcb);
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    size_t got = 0;
    (void)ctx;
    if (s_c.lost) return HC_LOST;
    while (s_c.q && got < n) {
        struct pbuf *q = s_c.q;
        size_t k = q->len - s_c.q_off;
        if (k > n - got) k = n - got;
        memcpy(b + got, (const uint8_t *)q->payload + s_c.q_off, k);
        got += k;
        s_c.q_off += (uint32_t)k;
        if (s_c.q_off < q->len) break;
        s_c.q = q->next;
        s_c.q_off = 0;
        if (s_c.q) pbuf_ref(s_c.q);
        pbuf_free(q);
    }
    if (got) { tcp_recved(s_c.pcb, (u16_t)got); return (int)got; }
    return s_c.eof ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
}

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg; (void)err;
    if (!p) { s_c.eof = true; return ERR_OK; }
    if (pcb != s_c.pcb) { tcp_recved(pcb, p->tot_len); pbuf_free(p); return ERR_OK; }
    if (s_c.q) pbuf_cat(s_c.q, p);
    else { s_c.q = p; s_c.q_off = 0; }
    return ERR_OK;
}

static void cb_err(void *arg, err_t err)
{
    (void)arg; (void)err;
    s_c.pcb = NULL;
    s_c.lost = true;
    if (s_c.q) pbuf_free(s_c.q);
    s_c.q = NULL;
}

static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg; (void)pcb;
    s_c.up = err == ERR_OK;
    return ERR_OK;
}

static ip_addr_t s_ip;
static int s_dns;                        /* 0 waiting, 1 found, -1 not */
static void cb_dns(const char *name, const ip_addr_t *ip, void *arg)
{
    (void)name; (void)arg;
    if (ip) s_ip = *ip;
    s_dns = ip ? 1 : -1;
}

void hc_close(void)
{
    if (s_c.setup) {
        if (s_c.up && !s_c.lost) mbedtls_ssl_close_notify(&s_c.ssl);
        mbedtls_ssl_free(&s_c.ssl);
        mbedtls_ssl_config_free(&s_c.conf);
        mbedtls_x509_crt_free(&s_c.roots);
    }
    if (s_c.q) {
        if (s_c.pcb) tcp_recved(s_c.pcb, (u16_t)(s_c.q->tot_len - s_c.q_off));
        pbuf_free(s_c.q);
    }
    if (s_c.pcb) {
        tcp_arg(s_c.pcb, NULL);
        tcp_recv(s_c.pcb, NULL);
        tcp_err(s_c.pcb, NULL);
        if (tcp_close(s_c.pcb) != ERR_OK) tcp_abort(s_c.pcb);
    }
    memset(&s_c, 0, sizeof s_c);
}

static bool waited(uint32_t t0) { worker_yield(); return HAL_GetTick() - t0 > WAIT_MS; }

static const int SUITES[] = { MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                              MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 0 };

static const char *open_to(const char *host)
{
    uint32_t t0 = HAL_GetTick();
    int r;
    if (s_c.setup && !s_c.lost && !s_c.eof && !strcmp(s_c.host, host)) return NULL;   /* still open */
    hc_close();
    if (!clock_synced()) return "the time is not known yet";   /* certificates; AWS allows 5 minutes */
    s_dns = 0;
    err_t e = dns_gethostbyname(host, &s_ip, cb_dns, NULL);
    if (e == ERR_OK) s_dns = 1;
    else if (e != ERR_INPROGRESS) return "name lookup failed";
    while (!s_dns) if (waited(t0)) return "name lookup timed out";
    if (s_dns < 0) return "name not found";
    if (!(s_c.pcb = tcp_new())) return "out of connections";
    tcp_recv(s_c.pcb, cb_recv);
    tcp_err(s_c.pcb, cb_err);
    if (tcp_connect(s_c.pcb, &s_ip, 443, cb_connected) != ERR_OK) { hc_close(); return "could not connect"; }
    while (!s_c.up && !s_c.lost) if (waited(t0)) { hc_close(); return "connection timed out"; }
    if (s_c.lost) { hc_close(); return "connection refused"; }

    s_c.setup = true;
    snprintf(s_c.host, sizeof s_c.host, "%s", host);
    mbedtls_ssl_init(&s_c.ssl);
    mbedtls_ssl_config_init(&s_c.conf);
    mbedtls_x509_crt_init(&s_c.roots);
    for (unsigned i = 0; i < g_nroots; i++)
        if (mbedtls_x509_crt_parse_der_nocopy(&s_c.roots, g_roots[i].der, g_roots[i].len)) { hc_close(); return "a root certificate is unreadable"; }
    if (mbedtls_ssl_config_defaults(&s_c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) {
        hc_close();
        return "out of memory";
    }
    mbedtls_ssl_conf_authmode(&s_c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s_c.conf, &s_c.roots, NULL);
    mbedtls_ssl_conf_ciphersuites(&s_c.conf, SUITES);
    if (mbedtls_ssl_setup(&s_c.ssl, &s_c.conf) || mbedtls_ssl_set_hostname(&s_c.ssl, host)) { hc_close(); return "out of memory"; }
    mbedtls_ssl_set_bio(&s_c.ssl, NULL, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&s_c.ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        if (waited(t0)) { hc_close(); return "TLS handshake timed out"; }
    if (r) {
        uint32_t v = mbedtls_ssl_get_verify_result(&s_c.ssl);
        hc_close();
        return v & MBEDTLS_X509_BADCERT_EXPIRED ? "its certificate has expired" :
               v & MBEDTLS_X509_BADCERT_FUTURE ? "its certificate is not valid yet: is the time right?" :
               v & MBEDTLS_X509_BADCERT_CN_MISMATCH ? "its certificate is for another name" :
               v ? "its certificate is not trusted" : "TLS handshake failed";
    }
    s_shakes++;
    return NULL;
}

static bool put(const void *d, size_t n, uint32_t t0)
{
    const uint8_t *p = d;
    while (n) {
        int r = mbedtls_ssl_write(&s_c.ssl, p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) { if (waited(t0)) return false; continue; }
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

/* Reads into buf[*have..max), waiting. False on error or timeout; *closed at the end. */
static bool more(char *buf, size_t *have, size_t max, bool *closed, uint32_t t0)
{
    for (;;) {
        if (*have + 1 >= max) return false;
        int r = mbedtls_ssl_read(&s_c.ssl, (unsigned char *)buf + *have, max - 1 - *have);
        if (r > 0) { *have += (size_t)r; buf[*have] = 0; return true; }
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { *closed = true; return true; }
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
        if (waited(t0)) return false;
    }
}

static const char *header(const char *h, const char *end, const char *name, char *out, size_t max)
{
    size_t n = strlen(name);
    for (const char *p = strstr(h, "\r\n"); p && p < end; p = strstr(p + 2, "\r\n")) {
        if (strncasecmp(p + 2, name, n) || p[2 + n] != ':') continue;
        const char *v = p + 3 + n, *e = strstr(v, "\r\n");
        while (*v == ' ') v++;
        size_t k = (size_t)(e - v);
        if (out) { if (k >= max) k = max - 1; memcpy(out, v, k); out[k] = 0; }
        return v;
    }
    if (out) out[0] = 0;
    return NULL;
}

/* Joins the pieces of a chunked body in place. */
static bool unchunk(char *b, size_t *len)
{
    char *in = b, *out = b, *end = b + *len;
    for (;;) {
        char *e;
        unsigned long n = strtoul(in, &e, 16);
        char *crlf = strstr(in, "\r\n");
        if (!crlf || e == in) return false;
        in = crlf + 2;
        if (!n) break;
        if (in + n + 2 > end) return false;
        memmove(out, in, n);
        out += n;
        in += n + 2;
    }
    *out = 0;
    *len = (size_t)(out - b);
    return true;
}

const char *hc_request(const char *method, const char *host, const char *path, const char *headers,
                       const void *body, size_t blen, char *buf, size_t max, hc_resp_t *r)
{
    const char *why;
    uint32_t t0 = HAL_GetTick();
    memset(r, 0, sizeof *r);
    for (int tries = 0; tries < 2; tries++) {             /* a kept connection may have gone */
        if ((why = open_to(host))) return why;
        int n = snprintf(buf, max, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: n540-gateway\r\nAccept: */*\r\n%sContent-Length: %u\r\n\r\n",
                         method, path, host, headers ? headers : "", (unsigned)blen);
        if (n <= 0 || (size_t)n >= max) return "request too long";
        if (!put(buf, (size_t)n, t0) || !put(body, blen, t0)) { hc_close(); continue; }
        size_t have = 0;
        bool closed = false;
        buf[0] = 0;                                     /* it held the request */
        char *end = NULL;
        while (!(end = strstr(buf, "\r\n\r\n")) && !closed)
            if (!more(buf, &have, max, &closed, t0)) { hc_close(); return "no answer"; }
        if (!end) { hc_close(); if (have) return "answer cut short"; continue; }
        r->status = atoi(strchr(buf, ' ') ? strchr(buf, ' ') + 1 : "0");
        char cl[16], te[24], co[16];
        header(buf, end, "Content-Length", cl, sizeof cl);
        header(buf, end, "Transfer-Encoding", te, sizeof te);
        header(buf, end, "Connection", co, sizeof co);
        header(buf, end, "Location", r->location, sizeof r->location);
        header(buf, end, "Replay-Nonce", r->nonce, sizeof r->nonce);
        size_t hlen = (size_t)(end + 4 - buf);
        memmove(buf, buf + hlen, have - hlen + 1);
        have -= hlen;
        bool chunked = !strcasecmp(te, "chunked");
        size_t want = cl[0] ? (size_t)strtoul(cl, NULL, 10) : (size_t)-1;
        if (!strcmp(method, "HEAD")) want = 0;
        while (!closed && (chunked ? !strstr(buf, "\r\n0\r\n\r\n") && strncmp(buf, "0\r\n\r\n", 5) : have < want))
            if (!more(buf, &have, max, &closed, t0)) { hc_close(); return "answer too long, or cut short"; }
        if (chunked && !unchunk(buf, &have)) { hc_close(); return "answer garbled"; }
        if (!chunked && want != (size_t)-1 && have < want) { hc_close(); return "answer cut short"; }
        if (!chunked && want != (size_t)-1) have = want;
        buf[have] = 0;
        r->body = buf;
        r->len = have;
        if (closed || !strcasecmp(co, "close")) hc_close();
        return NULL;
    }
    return "the connection kept closing";
}
