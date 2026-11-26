/*
 * hclient.c — Replies to the outgoing HTTPS client (src/app/hclient.c), as Let's Encrypt
 * or AWS might send them: TLS is a stand-in that hands over the input, in pieces whose
 * sizes the input also decides. Two requests on one connection, as renewal makes them.
 *
 * Input: one byte of flags (bit 0: the server closes at the end, else it goes quiet),
 * then pairs of (piece length, bytes).
 */
#include "hclient.h"
#include "roots.h"
#include "lwip/tcp.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint32_t t_now;
t_dwt_t t_dwt;
uint32_t t_dwt_now(void) { return t_now * 400000u; }
void NVIC_SystemReset(void) { abort(); }
const root_t g_roots[1];
const unsigned g_nroots = 0;
bool clock_synced(void) { return true; }
void worker_yield(void) { t_now += 100; }

/* ---- lwIP: connects at once ---- */
struct tcp_pcb { tcp_recv_fn rx; tcp_err_fn err; void *arg; };
static struct tcp_pcb s_pcb;
struct tcp_pcb *tcp_new(void) { memset(&s_pcb, 0, sizeof s_pcb); return &s_pcb; }
void tcp_arg(struct tcp_pcb *p, void *a) { p->arg = a; }
void tcp_recv(struct tcp_pcb *p, tcp_recv_fn f) { p->rx = f; }
void tcp_err(struct tcp_pcb *p, tcp_err_fn f) { p->err = f; }
err_t tcp_connect(struct tcp_pcb *p, const ip_addr_t *ip, u16_t port, err_t (*f)(void *, struct tcp_pcb *, err_t)) { (void)ip; (void)port; return f(p->arg, p, ERR_OK); }
err_t tcp_close(struct tcp_pcb *p) { (void)p; return ERR_OK; }
void tcp_abort(struct tcp_pcb *p) { (void)p; }
void tcp_recved(struct tcp_pcb *p, u16_t n) { (void)p; (void)n; }
u16_t tcp_sndbuf(struct tcp_pcb *p) { (void)p; return 8000; }
err_t tcp_write(struct tcp_pcb *p, const void *d, u16_t n, u8_t f) { (void)p; (void)d; (void)n; (void)f; return ERR_OK; }
err_t tcp_output(struct tcp_pcb *p) { (void)p; return ERR_OK; }
void pbuf_free(struct pbuf *p) { (void)p; }
void pbuf_ref(struct pbuf *p) { (void)p; }
void pbuf_cat(struct pbuf *h, struct pbuf *t) { (void)h; (void)t; }
err_t dns_gethostbyname(const char *n, ip_addr_t *ip, void (*f)(const char *, const ip_addr_t *, void *), void *a)
{ (void)n; (void)f; (void)a; ip->addr = 0x01020304; return ERR_OK; }

/* ---- TLS: the input is what the server says ---- */
static const uint8_t *s_in;
static size_t s_n, s_at;
static bool s_closes;
void mbedtls_ssl_init(mbedtls_ssl_context *s) { (void)s; }
void mbedtls_ssl_config_init(mbedtls_ssl_config *c) { (void)c; }
void mbedtls_x509_crt_init(mbedtls_x509_crt *c) { (void)c; }
int mbedtls_x509_crt_parse_der_nocopy(mbedtls_x509_crt *c, const unsigned char *b, size_t n) { (void)c; (void)b; (void)n; return 0; }
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *c, int a, int b, int d) { (void)c; (void)a; (void)b; (void)d; return 0; }
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *c, int m) { (void)c; (void)m; }
void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config *c, mbedtls_x509_crt *ca, mbedtls_x509_crl *crl) { (void)c; (void)ca; (void)crl; }
void mbedtls_ssl_conf_ciphersuites(mbedtls_ssl_config *c, const int *s) { (void)c; (void)s; }
int mbedtls_ssl_setup(mbedtls_ssl_context *s, const mbedtls_ssl_config *c) { (void)s; (void)c; return 0; }
int mbedtls_ssl_set_hostname(mbedtls_ssl_context *s, const char *h) { (void)s; (void)h; return 0; }
void mbedtls_ssl_set_bio(mbedtls_ssl_context *s, void *p, mbedtls_ssl_send_t *a, mbedtls_ssl_recv_t *b, mbedtls_ssl_recv_timeout_t *c) { (void)s; (void)p; (void)a; (void)b; (void)c; }
int mbedtls_ssl_handshake(mbedtls_ssl_context *s) { (void)s; return 0; }
uint32_t mbedtls_ssl_get_verify_result(const mbedtls_ssl_context *s) { (void)s; return 0; }
int mbedtls_ssl_write(mbedtls_ssl_context *s, const unsigned char *b, size_t n) { (void)s; (void)b; return (int)n; }
int mbedtls_ssl_close_notify(mbedtls_ssl_context *s) { (void)s; return 0; }
void mbedtls_ssl_free(mbedtls_ssl_context *s) { (void)s; }
void mbedtls_ssl_config_free(mbedtls_ssl_config *c) { (void)c; }
void mbedtls_x509_crt_free(mbedtls_x509_crt *c) { (void)c; }
int mbedtls_ssl_read(mbedtls_ssl_context *s, unsigned char *b, size_t n)
{
    (void)s;
    if (s_at >= s_n) return s_closes ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
    size_t k = s_in[s_at++] + 1u;                    /* this piece's length */
    if (k > s_n - s_at) k = s_n - s_at;
    if (k > n) { k = n; }
    memcpy(b, s_in + s_at, k);
    s_at += k;
    return (int)k;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static char buf[8192];
    if (!n) return 0;
    s_closes = d[0] & 1;
    s_in = d + 1; s_n = n - 1; s_at = 0;
    for (int i = 0; i < 2; i++) {
        hc_resp_t r;
        const char *why = hc_request(i ? "GET" : "POST", "acme.example", "/a", "Content-Type: x\r\n", "{}", 2, buf, sizeof buf, &r);
        if (why) { assert(*why); continue; }
        assert(r.body >= buf && r.body + r.len < buf + sizeof buf && r.body[r.len] == 0);
        assert(strlen(r.location) < sizeof r.location && strlen(r.nonce) < sizeof r.nonce);
    }
    hc_close();
    return 0;
}
