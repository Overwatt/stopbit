#define MBEDTLS_ALLOW_PRIVATE_ACCESS     /* x509_crt.own_buffer, for the certificates we only pass on */
#include "tls.h"
#include "worker.h"
#include "clock.h"
#include "extflash.h"
#include "sha256.h"
#include "net.h"
#include "stm32h7xx_hal.h"
#include "lwip/tcp.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ticket.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/memory_buffer_alloc.h"
#include <stdio.h>
#include <string.h>

#ifndef AXI_BSS
#define AXI_BSS __attribute__((section(".axi_bss")))
#endif
#define TLS_MAX      4
#define SHAKE_MS     20000u
#define REQUEST_MS   20000u
#define IDLE_MS      60000u
#define STUCK_MS     180000u             /* a reply nobody takes; installs are slow too */
#define TICKET_MS    (12u * 3600u * 1000u)
#define SEND_MAX     2048u               /* plaintext per record sent */
#define PASS_MAX     4096u               /* plaintext taken in one pass of the main loop */

/* ---- what is stored -------------------------------------------------------- */
#define DEV     1
#define MAGIC   0x534C5435u
#define SCHEMA  1u
#define SECTORS 2u                       /* per copy */
static const uint32_t ADDR[2] = { 0x0F4000u, 0x0F6000u };
#define KEYS_FROM 0x0F4000u
#define KEYS_TO   0x0F8000u              /* acme.c masks its own */

enum { CERT_NONE, CERT_SELF, CERT_ISSUED };
typedef struct {
    uint8_t  enabled, kind, req, pad;
    uint8_t  key[32], pub[65], req_key[32], req_pub[65], pad2[2];
    char     name[64], req_name[64];
    uint32_t chain_len;
    uint8_t  chain[4096];                /* DER, one certificate after another */
} body_t;
typedef struct {
    uint32_t magic, schema, seq, size;
    uint8_t  sha[32];
    body_t   b;
} rec_t;
_Static_assert(sizeof(rec_t) <= SECTORS * 4096u, "record exceeds its sectors");

static AXI_BSS rec_t s_rec, s_tmp;       /* s_tmp: scratch, and the job's result */
static int s_cur = -1;

static void digest(const rec_t *r, uint8_t out[32])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, &r->schema, 12);
    sha256_update(&s, &r->b, sizeof r->b);
    sha256_final(&s, out);
}

static bool read_rec(int i, rec_t *r)
{
    uint8_t d[32];
    extflash_read(DEV, ADDR[i], r, sizeof *r);
    if (r->magic != MAGIC || r->schema != SCHEMA || r->size != sizeof r->b || r->b.chain_len > sizeof r->b.chain) return false;
    r->b.name[sizeof r->b.name - 1] = r->b.req_name[sizeof r->b.req_name - 1] = 0;
    digest(r, d);
    return ct_equal(d, r->sha, 32);
}

static void load(void)
{
    memset(&s_rec, 0, sizeof s_rec);
    s_cur = -1;
    for (int i = 0; i < 2; i++) {
        if (!read_rec(i, &s_tmp) || (s_cur >= 0 && s_tmp.seq <= s_rec.seq)) continue;
        s_rec = s_tmp;
        s_cur = i;
    }
}

/* Saves s_tmp.b and makes it current. The magic goes last. */
static bool save(void)
{
    int i = s_cur == 0 ? 1 : 0;
    s_tmp.magic = MAGIC;
    s_tmp.schema = SCHEMA;
    s_tmp.seq = s_rec.seq + 1;
    s_tmp.size = sizeof s_tmp.b;
    digest(&s_tmp, s_tmp.sha);
    s_rec = s_tmp;                       /* s_tmp is the read-back buffer below */
    bool ok = extflash_erase(DEV, ADDR[i], SECTORS * 4096u) &&
              extflash_write(DEV, ADDR[i] + 4, &s_rec.schema, sizeof s_rec - 4) &&
              extflash_write(DEV, ADDR[i], &s_rec.magic, 4) &&
              read_rec(i, &s_tmp) && !memcmp(&s_tmp, &s_rec, sizeof s_rec);
    if (ok) s_cur = i;
    else load();                         /* back to what is really there */
    return ok;
}

void tls_mask(int dev, uint32_t addr, uint8_t *buf, uint32_t n)
{
    if (dev != DEV) return;
    for (uint32_t i = 0; i < n; i++)
        if (addr + i >= KEYS_FROM && addr + i < KEYS_TO) buf[i] = 0xFF;
}

/* ---- mbedTLS --------------------------------------------------------------- */
/* The heap is whatever AXI SRAM the image and its buffers leave (linker/app_ram.ld). */
#ifdef TLS_HEAP
static uint64_t s_heap[TLS_HEAP / 8];
#define HEAP_AT   ((unsigned char *)s_heap)
#define HEAP_SIZE sizeof s_heap
#else
extern uint8_t _eaxi;
#define HEAP_AT   ((unsigned char *)(((uintptr_t)&_eaxi + 31u) & ~(uintptr_t)31u))
#define HEAP_SIZE (0x24080000u - (uint32_t)(uintptr_t)HEAP_AT)
#endif
static mbedtls_ssl_config       s_conf;
static mbedtls_ssl_ticket_context s_tick;
static mbedtls_x509_crt         s_crt;
static mbedtls_pk_context       s_key;
static bool s_ready, s_live;             /* library set up; certificate in place and listening */
static bool s_loaded;                    /* certificate and key in place, maybe not listening yet */
static uint32_t s_listen_at;
static void listen_443(void);
static uint32_t s_tick_at;
static const char *s_err = "";

int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *bits, unsigned char *out, size_t len)
{
    if (flags != PSA_DRIVER_GET_ENTROPY_FLAGS_NONE) return PSA_ERROR_NOT_SUPPORTED;
    if (!rng_fill(out, (uint32_t)len)) return PSA_ERROR_INSUFFICIENT_ENTROPY;
    *bits = 8 * len;
    return 0;
}

/* Where long calculations come up for air: see CMakeLists.txt. */
struct mbedtls_mpi;
int mbedtls_mpi_mul_mpi(struct mbedtls_mpi *X, const struct mbedtls_mpi *A, const struct mbedtls_mpi *B);
int tls_mpi_mul_mpi(struct mbedtls_mpi *X, const struct mbedtls_mpi *A, const struct mbedtls_mpi *B);
int tls_mpi_mul_mpi(struct mbedtls_mpi *X, const struct mbedtls_mpi *A, const struct mbedtls_mpi *B)
{
    worker_breathe();
    return mbedtls_mpi_mul_mpi(X, A, B);
}
struct mbedtls_sha256_context;
int mbedtls_sha256_update(struct mbedtls_sha256_context *c, const unsigned char *d, size_t n);
int tls_sha256_update(struct mbedtls_sha256_context *c, const unsigned char *d, size_t n);
int tls_sha256_update(struct mbedtls_sha256_context *c, const unsigned char *d, size_t n)
{
    worker_breathe();
    return mbedtls_sha256_update(c, d, n);
}
void *tls_core_memcpy(void *d, const void *s, size_t n);
void *tls_core_memcpy(void *d, const void *s, size_t n)
{
    worker_breathe();
    return memcpy(d, s, n);
}

/* ---- certificate and key ---------------------------------------------------- */
/* Works out the public half, so in the worker. */
static bool put_key(mbedtls_pk_context *pk, const uint8_t key[32])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    mbedtls_pk_init(pk);
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT);
    if (psa_import_key(&a, key, 32, &id) != PSA_SUCCESS) return false;
    bool ok = !mbedtls_pk_copy_from_psa(id, pk);
    psa_destroy_key(id);
    return ok;
}

static bool make_key(uint8_t key[32], uint8_t pub[65])
{
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t id = 0;
    size_t n;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT);
    if (psa_generate_key(&a, &id) != PSA_SUCCESS) return false;
    bool ok = psa_export_key(id, key, 32, &n) == PSA_SUCCESS && n == 32 &&
              psa_export_public_key(id, pub, 65, &n) == PSA_SUCCESS && n == 65;
    psa_destroy_key(id);
    return ok;
}

static void san_dns(mbedtls_x509_san_list *l, const char *name)
{
    memset(l, 0, sizeof *l);
    l->node.type = MBEDTLS_X509_SAN_DNS_NAME;
    l->node.san.unstructured_name.p = (unsigned char *)(uintptr_t)name;
    l->node.san.unstructured_name.len = strlen(name);
}

/* Both are written at the end of `out`; the result is moved to its start. */
static size_t front(uint8_t *out, size_t max, int n)
{
    if (n <= 0) return 0;
    memmove(out, out + max - (size_t)n, (size_t)n);
    return (size_t)n;
}

static size_t self_sign(const char *name, const uint8_t key[32], uint8_t *out, size_t max)
{
    mbedtls_x509write_cert w;
    mbedtls_pk_context pk;
    mbedtls_x509_san_list san;
    uint8_t serial[16];
    char dn[80];
    int n = -1;
    snprintf(dn, sizeof dn, "CN=%s", name);
    san_dns(&san, name);
    mbedtls_x509write_crt_init(&w);
    if (put_key(&pk, key) && psa_generate_random(serial, sizeof serial) == PSA_SUCCESS) {
        serial[0] = (uint8_t)((serial[0] & 0x7F) | 0x40);
        mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&w, &pk);
        mbedtls_x509write_crt_set_issuer_key(&w, &pk);
        if (!mbedtls_x509write_crt_set_subject_name(&w, dn) && !mbedtls_x509write_crt_set_issuer_name(&w, dn) &&
            !mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial) &&
            !mbedtls_x509write_crt_set_validity(&w, "20250101000000", "20491231235959") &&      /* no clock */
            !mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1) &&
            !mbedtls_x509write_crt_set_subject_alternative_name(&w, &san))
            n = mbedtls_x509write_crt_der(&w, out, max);
    }
    mbedtls_x509write_crt_free(&w);
    mbedtls_pk_free(&pk);
    return front(out, max, n);
}

static size_t sign_request(const char *name, const uint8_t key[32], uint8_t *out, size_t max)
{
    mbedtls_x509write_csr w;
    mbedtls_pk_context pk;
    mbedtls_x509_san_list san;
    char dn[80];
    int n = -1;
    snprintf(dn, sizeof dn, "CN=%s", name);
    san_dns(&san, name);
    mbedtls_x509write_csr_init(&w);
    if (put_key(&pk, key)) {
        mbedtls_x509write_csr_set_md_alg(&w, MBEDTLS_MD_SHA256);
        mbedtls_x509write_csr_set_key(&w, &pk);
        if (!mbedtls_x509write_csr_set_subject_name(&w, dn) &&
            !mbedtls_x509write_csr_set_subject_alternative_name(&w, &san))
            n = mbedtls_x509write_csr_der(&w, out, max);
    }
    mbedtls_x509write_csr_free(&w);
    mbedtls_pk_free(&pk);
    return front(out, max, n);
}

/* Concatenated DER certificates. Only the first has to be understood; the rest are
 * the issuer's, of any kind, and are just passed on. */
static bool put_chain(mbedtls_x509_crt *crt, const uint8_t *p, size_t len)
{
    mbedtls_x509_crt *last = crt;
    mbedtls_x509_crt_init(crt);
    if (!len) return false;
    while (len) {
        if (len < 4 || p[0] != 0x30) return false;
        size_t n = p[1] < 0x80 ? 2u + p[1] : p[1] == 0x81 ? 3u + p[2] : p[1] == 0x82 ? 4u + ((size_t)p[2] << 8 | p[3]) : len + 1;
        if (n > len) return false;
        if (last == crt && !crt->raw.p) {
            if (mbedtls_x509_crt_parse_der(crt, p, n)) return false;
        } else {
            mbedtls_x509_crt *c = mbedtls_calloc(1, sizeof *c);
            if (!c) return false;
            mbedtls_x509_crt_init(c);
            last->next = c;                     /* freed along with the first from here on */
            last = c;
            if (!(c->raw.p = mbedtls_calloc(1, n))) return false;
            memcpy(c->raw.p, p, n);
            c->raw.len = n;
            c->own_buffer = 1;
        }
        p += n;
        len -= n;
    }
    return true;
}

/* ---- jobs -------------------------------------------------------------------- */
enum { JOB_NONE, JOB_SELF, JOB_REQUEST, JOB_CSR, JOB_LOAD, JOB_SHAKE };
static uint8_t s_job, s_next;
static bool    s_job_ok;
static uint8_t s_csr[512];
static size_t  s_csr_len;
static char    s_want_name[64];

static void run_job(void *arg)
{
    body_t *b = &s_tmp.b;
    (void)arg;
    if (s_job == JOB_LOAD) {
        s_job_ok = put_chain(&s_crt, s_rec.b.chain, s_rec.b.chain_len) && put_key(&s_key, s_rec.b.key);
        return;
    }
    *b = s_rec.b;
    if (s_job == JOB_SELF) {
        memset(b->name, 0, sizeof b->name);
        snprintf(b->name, sizeof b->name, "%s.local", g_hostname);
        memset(b->chain, 0, sizeof b->chain);
        s_job_ok = make_key(b->key, b->pub) &&
                   (b->chain_len = (uint32_t)self_sign(b->name, b->key, b->chain, sizeof b->chain)) > 0;
        memset(b->chain + b->chain_len, 0, sizeof b->chain - b->chain_len);
        b->kind = CERT_SELF;
        b->enabled = 1;
        return;
    }
    s_job_ok = true;
    if (s_job == JOB_REQUEST) {
        memset(b->req_name, 0, sizeof b->req_name);
        snprintf(b->req_name, sizeof b->req_name, "%s", s_want_name);
        b->req = 1;
        s_job_ok = make_key(b->req_key, b->req_pub);
    }
    s_csr_len = s_job_ok ? sign_request(b->req_name, b->req_key, s_csr, sizeof s_csr) : 0;
    s_job_ok = s_csr_len > 0;
}

/* ---- connections --------------------------------------------------------------- */
enum { T_FREE, T_HELLO, T_SHAKE, T_OPEN, T_CLOSE };
struct tls {
    uint8_t  state;
    bool     setup, eof, dead;           /* ssl is in use; peer closed; give up */
    struct tcp_pcb *pcb;                 /* NULL: lost */
    struct pbuf *q;                      /* received, not yet read */
    uint32_t q_off, flush;               /* flush: length of a write to repeat */
    uint32_t at, since;                  /* last activity; accepted */
    uint32_t slices0;
    int      ret;
    void    *http;
    mbedtls_ssl_context ssl;
};
static struct tls s_t[TLS_MAX], *s_shaking;
static struct tcp_pcb *s_listen;
static uint32_t s_shakes, s_failed, s_refused, s_last_ms, s_last_slices;

#define ERR_LOST (-0x0050)

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    struct tls *t = ctx;
    if (!t->pcb) return ERR_LOST;
    uint32_t room = tcp_sndbuf(t->pcb);
    if (n > room) n = room;
    if (!n || tcp_sndqueuelen(t->pcb) >= TCP_SND_QUEUELEN - 4) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (tcp_write(t->pcb, b, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    struct tls *t = ctx;
    size_t got = 0;
    if (!t->pcb) return ERR_LOST;
    while (t->q && got < n) {
        struct pbuf *q = t->q;
        size_t k = q->len - t->q_off;
        if (k > n - got) k = n - got;
        memcpy(b + got, (const uint8_t *)q->payload + t->q_off, k);
        got += k;
        t->q_off += (uint32_t)k;
        if (t->q_off < q->len) break;
        t->q = q->next;
        t->q_off = 0;
        if (t->q) pbuf_ref(t->q);
        pbuf_free(q);
    }
    if (got) {
        tcp_recved(t->pcb, (u16_t)got);
        return (int)got;
    }
    return t->eof ? 0 : MBEDTLS_ERR_SSL_WANT_READ;
}

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct tls *t = arg;
    (void)err;
    if (!t) {
        if (p) { tcp_recved(pcb, p->tot_len); pbuf_free(p); }
        return ERR_OK;
    }
    if (!p) { t->eof = true; return ERR_OK; }
    t->at = HAL_GetTick();
    if (t->q) pbuf_cat(t->q, p);
    else { t->q = p; t->q_off = 0; }
    return ERR_OK;
}

static void drop_queue(struct tls *t)
{
    if (t->q) {
        if (t->pcb) tcp_recved(t->pcb, (u16_t)(t->q->tot_len - t->q_off));
        pbuf_free(t->q);
    }
    t->q = NULL;
    t->q_off = 0;
}

static void cb_err(void *arg, err_t err)
{
    struct tls *t = arg;
    (void)err;
    if (!t) return;
    t->pcb = NULL;                       /* lwIP already freed it */
    drop_queue(t);
}

/* Not while the worker has it. */
static void finish(struct tls *t)
{
    void *h = t->http;
    t->http = NULL;
    if (h) http_tls_gone(h);
    drop_queue(t);
    if (t->pcb) {
        tcp_arg(t->pcb, NULL);
        tcp_recv(t->pcb, NULL);
        tcp_err(t->pcb, NULL);
        if (tcp_close(t->pcb) != ERR_OK) tcp_abort(t->pcb);
    }
    if (t->setup) mbedtls_ssl_free(&t->ssl);
    memset(t, 0, sizeof *t);
}

static err_t cb_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct tls *t = NULL, *idle = NULL;
    uint32_t now = HAL_GetTick();
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    for (int i = 0; i < TLS_MAX; i++) {
        struct tls *c = &s_t[i];
        if (c->state == T_FREE) { t = c; break; }
        if (c->state == T_OPEN && c->http && http_tls_idle(c->http) && (!idle || now - c->at > now - idle->at)) idle = c;
    }
    if (!t && idle) { finish(idle); t = idle; }      /* one that is only being kept alive */
    if (!t) {
        s_refused++;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    t->state = T_HELLO;
    t->pcb = pcb;
    t->at = t->since = now;
    tcp_arg(pcb, t);
    tcp_recv(pcb, cb_recv);
    tcp_err(pcb, cb_err);
    tcp_nagle_disable(pcb);
    return ERR_OK;
}

static void shake(void *arg)
{
    struct tls *t = arg;
    t->ret = mbedtls_ssl_handshake(&t->ssl);
}

static void start_shake(void)
{
    static int turn;
    for (int k = 0; k < TLS_MAX; k++) {
        struct tls *t = &s_t[(turn + k) % TLS_MAX];
        if (t->state != T_HELLO || !t->pcb || !(t->q || t->eof || t->ret == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
        if (!t->setup) {
            mbedtls_ssl_init(&t->ssl);
            t->setup = true;
            if (mbedtls_ssl_setup(&t->ssl, &s_conf)) { s_failed++; finish(t); continue; }     /* out of memory */
            mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
            worker_stats_t w;
            worker_stats(WORKER_TLS, &w);
            t->slices0 = w.slices;
        }
        turn = (turn + k + 1) % TLS_MAX;
        t->state = T_SHAKE;
        s_shaking = t;
        s_job = JOB_SHAKE;
        worker_start(WORKER_TLS, shake, t);
        return;
    }
}

static void shaken(struct tls *t)
{
    worker_stats_t w;
    t->state = T_HELLO;
    if (t->pcb) tcp_output(t->pcb);
    if (t->ret == MBEDTLS_ERR_SSL_WANT_READ || t->ret == MBEDTLS_ERR_SSL_WANT_WRITE) return;
    if (t->ret) { s_failed++; finish(t); return; }
    worker_stats(WORKER_TLS, &w);
    s_shakes++;
    s_last_ms = HAL_GetTick() - t->since;
    s_last_slices = w.slices - t->slices0;
    t->at = HAL_GetTick();
    t->state = T_OPEN;
    if (!(t->http = http_tls_open(t))) { s_refused++; t->state = T_CLOSE; }
}

uint32_t tls_room(const tls_t *t)
{
    if (t->state != T_OPEN || !t->pcb || t->flush || t->dead) return 0;
    uint32_t room = tcp_sndbuf(t->pcb);
    if (room < 512 || tcp_sndqueuelen(t->pcb) >= TCP_SND_QUEUELEN - 6) return 0;
    room -= 64;                          /* a record's own bytes */
    return room < SEND_MAX ? room : SEND_MAX;
}

static void wrote(struct tls *t, int r, uint32_t n)
{
    t->flush = r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ ? n : 0;
    if (r < 0 && !t->flush) t->dead = true;
}

void tls_write(tls_t *t, const void *d, uint32_t n)
{
    t->at = HAL_GetTick();
    if (n) wrote(t, mbedtls_ssl_write(&t->ssl, d, n), n);
}

void tls_close(tls_t *t)
{
    t->http = NULL;
    t->state = T_CLOSE;
    t->at = HAL_GetTick();
}

/* Read the clock here, not earlier: `at` may have moved on since. */
static uint32_t quiet_ms(const struct tls *t) { return HAL_GetTick() - t->at; }

static void service(struct tls *t)
{
    static uint8_t buf[SEND_MAX];
    if (t->state == T_FREE || t->state == T_SHAKE) return;
    if (!t->pcb || t->dead) { finish(t); return; }
    if (t->state == T_HELLO) {
        if (HAL_GetTick() - t->since > SHAKE_MS) { s_failed++; finish(t); }
        return;
    }
    if (t->flush) wrote(t, mbedtls_ssl_write(&t->ssl, buf, t->flush), t->flush);   /* already encrypted; buf is not looked at */
    if (t->state == T_CLOSE) {
        if (t->flush && quiet_ms(t) < REQUEST_MS) return;
        if (!t->dead) mbedtls_ssl_close_notify(&t->ssl);
        tcp_output(t->pcb);
        finish(t);
        return;
    }
    if (tls_room(t)) http_tls_sent(t->http);
    for (uint32_t took = 0; t->state == T_OPEN && !t->dead && took < PASS_MAX;) {
        uint32_t want = http_tls_want(t->http);
        if (!want) break;
        int r = mbedtls_ssl_read(&t->ssl, buf, want < sizeof buf ? want : sizeof buf);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (r <= 0) { t->dead = true; break; }          /* closed, or nonsense */
        t->at = HAL_GetTick();
        took += (uint32_t)r;
        http_tls_data(t->http, buf, (uint32_t)r);
    }
    if (t->pcb) tcp_output(t->pcb);
    if (t->state == T_OPEN && !t->dead && t->http &&
        quiet_ms(t) > (http_tls_idle(t->http) ? IDLE_MS : http_tls_want(t->http) ? REQUEST_MS : STUCK_MS)) t->dead = true;
}

/* ---- on and off ------------------------------------------------------------------ */
static const int SUITES[] = { MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, 0 };   /* as a server */
static const uint16_t GROUPS[] = { MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1, MBEDTLS_SSL_IANA_TLS_GROUP_NONE };

static bool setup(void)
{
    if (s_ready) return true;
    mbedtls_memory_buffer_alloc_init(HEAP_AT, HEAP_SIZE);
    mbedtls_x509_crt_init(&s_crt);
    mbedtls_pk_init(&s_key);
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_ssl_ticket_init(&s_tick);
    s_ready = psa_crypto_init() == PSA_SUCCESS;
    if (!s_ready) s_err = "The random number generator failed";
    return s_ready;
}

static void deactivate(void)
{
    if (s_listen) tcp_close(s_listen);
    s_listen = NULL;
    for (int i = 0; i < TLS_MAX; i++) if (s_t[i].state != T_FREE) finish(&s_t[i]);
    s_live = s_loaded = false;
    mbedtls_ssl_config_free(&s_conf);
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_ssl_ticket_free(&s_tick);
    mbedtls_ssl_ticket_init(&s_tick);
    mbedtls_x509_crt_free(&s_crt);
    mbedtls_pk_free(&s_key);
}

/* Reads in what is stored, which go_live() then serves. Not while the worker runs. */
static void activate(void)
{
    deactivate();
    s_err = "";
    if (s_rec.b.enabled && s_rec.b.kind != CERT_NONE) s_next = JOB_LOAD;
}

static void go_live(bool loaded)
{
    if (!loaded) { s_err = "The stored certificate cannot be read"; return; }
    if (mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) ||
        mbedtls_ssl_ticket_setup(&s_tick, PSA_ALG_GCM, PSA_KEY_TYPE_AES, 128, 86400) ||
        mbedtls_ssl_conf_own_cert(&s_conf, &s_crt, &s_key)) { s_err = "Not enough memory"; return; }
    mbedtls_ssl_conf_ciphersuites(&s_conf, SUITES);
    mbedtls_ssl_conf_groups(&s_conf, GROUPS);
    mbedtls_ssl_conf_session_tickets_cb(&s_conf, mbedtls_ssl_ticket_write, mbedtls_ssl_ticket_parse, &s_tick);
    s_tick_at = HAL_GetTick();
    s_loaded = true;
    listen_443();
}

/* Connections just closed linger on the port for a couple of minutes, which lwIP treats as
 * "in use" unless told otherwise. If it still fails, tls_poll() tries again. */
static void listen_443(void)
{
    struct tcp_pcb *l = tcp_new();
    if (l) ip_set_option(l, SOF_REUSEADDR);
    if (!l || tcp_bind(l, IP_ADDR_ANY, 443) != ERR_OK || !(s_listen = tcp_listen_with_backlog(l, TLS_MAX))) {
        s_err = "Port 443 could not be opened; trying again";
        if (l) tcp_close(l);
        s_listen = NULL;
        s_listen_at = HAL_GetTick() + 5000;
        return;
    }
    tcp_accept(s_listen, cb_accept);
    s_err = "";
    s_live = true;
}

/* The request being answered may itself be on one of these connections. */
static bool     s_soon;
static uint32_t s_soon_at;
static void activate_soon(void) { s_soon = true; s_soon_at = HAL_GetTick() + 500; }

void tls_init(void)
{
    load();
    if (!s_rec.b.enabled && !s_rec.b.req) return;       /* nothing is set up until it is wanted */
    if (!setup()) return;
    activate();
    if (s_rec.b.req && s_next == JOB_NONE) s_next = JOB_CSR;
}

void tls_poll(void)
{
    if (!s_ready) return;
    worker_poll(WORKER_TLS);
    if (s_job != JOB_NONE && !worker_busy(WORKER_TLS)) {
        uint8_t j = s_job;
        s_job = JOB_NONE;
        if (j == JOB_SHAKE) shaken(s_shaking);
        else if (j == JOB_LOAD) {
            go_live(s_job_ok);
            if (s_rec.b.req && !s_csr_len) s_next = JOB_CSR;
        } else if (!s_job_ok) s_err = "The key or certificate could not be made";
        else if (j != JOB_CSR && !save()) s_err = "Could not write to flash";
        else if (j == JOB_SELF) activate();
    }
    if (s_loaded && !s_live && (int32_t)(HAL_GetTick() - s_listen_at) >= 0) listen_443();
    if (s_job == JOB_NONE && s_soon && (int32_t)(HAL_GetTick() - s_soon_at) >= 0) {
        s_soon = false;
        activate();
    }
    if (s_job == JOB_NONE) {
        if (s_next != JOB_NONE) {
            s_job = s_next;
            s_next = JOB_NONE;
            worker_start(WORKER_TLS, run_job, NULL);
        } else if (s_live) {
            if (HAL_GetTick() - s_tick_at > TICKET_MS) {        /* no clock, so on a timer */
                uint8_t r[20];
                s_tick_at = HAL_GetTick();
                if (psa_generate_random(r, sizeof r) == PSA_SUCCESS)
                    mbedtls_ssl_ticket_rotate(&s_tick, r, 4, r + 4, 16, 86400);
            }
            start_shake();
        }
    }
    for (int i = 0; i < TLS_MAX; i++) service(&s_t[i]);
}

/* ---- settings -------------------------------------------------------------------- */
static bool name_ok(const char *n)
{
    size_t l = strlen(n);
    bool dot = false;
    if (l < 3 || l > 63 || n[0] == '.' || n[l - 1] == '.') return false;
    for (const char *p = n; *p; p++) {
        if (*p == '.') { dot = true; if (p[1] == '.') return false; }
        else if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return false;
    }
    return dot;
}

/* All of it goes out in one message. */
_Static_assert(sizeof s_rec.b.chain + 64 <= MBEDTLS_SSL_OUT_CONTENT_LEN, "a full chain could not be sent");

static const char *take_cert(const uint8_t *der, size_t len)
{
    mbedtls_x509_crt c;
    uint8_t pub[PSA_EXPORT_PUBLIC_KEY_MAX_SIZE];
    size_t n = 0;
    const char *why = NULL;
    if (!s_rec.b.req) return "Make a signing request first";
    if (len > sizeof s_rec.b.chain) return "That certificate is too long";
    if (!put_chain(&c, der, len)) why = "That is not a certificate made from the signing request";
    else if (mbedtls_pk_write_pubkey_psa(&c.pk, pub, sizeof pub, &n) || n != sizeof s_rec.b.req_pub ||
             memcmp(pub, s_rec.b.req_pub, n)) why = "That certificate is for a different key";
    else {
        bool named = false;                     /* who signed it is the browser's business */
        size_t l = strlen(s_rec.b.req_name);
        for (const mbedtls_x509_sequence *a = &c.subject_alt_names; a; a = a->next)
            named |= (a->buf.tag & 0x1F) == MBEDTLS_X509_SAN_DNS_NAME && a->buf.len == l && !memcmp(a->buf.p, s_rec.b.req_name, l);
        if (!named) why = "That certificate is for a different name";
    }
    mbedtls_x509_crt_free(&c);
    if (why) return why;
    body_t *b = &s_tmp.b;
    *b = s_rec.b;
    b->enabled = 1;
    b->kind = CERT_ISSUED;
    memcpy(b->key, b->req_key, sizeof b->key);
    memcpy(b->pub, b->req_pub, sizeof b->pub);
    memcpy(b->name, b->req_name, sizeof b->name);
    memset(b->chain, 0, sizeof b->chain);
    memcpy(b->chain, der, len);
    b->chain_len = (uint32_t)len;
    if (!save()) return "Could not write to flash";
    activate_soon();
    return NULL;
}

const char *tls_command(const uint8_t *body, size_t len)
{
    char cmd[80];
    size_t n = 0;
    if (s_job != JOB_NONE && s_job != JOB_SHAKE) return "Still preparing; try again in a moment";
    if (s_next != JOB_NONE || s_soon) return "Still preparing; try again in a moment";
    if (worker_busy(WORKER_TLS)) return "Busy; try again";        /* certificates are shared with a handshake */
    if (len > 5 && !memcmp(body, "cert\n", 5)) return setup() ? take_cert(body + 5, len - 5) : s_err;
    while (n < len && n + 1 < sizeof cmd && body[n] >= 0x20 && body[n] < 0x7f) { cmd[n] = (char)body[n]; n++; }
    cmd[n] = 0;
    if (n != len) return "Not understood";
    if (!setup()) return s_err;
    if (!strcmp(cmd, "on")) {
        if (s_rec.b.kind == CERT_NONE) { s_next = JOB_SELF; return NULL; }
        s_tmp.b = s_rec.b;
        s_tmp.b.enabled = 1;
    } else if (!strcmp(cmd, "off")) {
        s_tmp.b = s_rec.b;
        s_tmp.b.enabled = 0;
    } else if (!strcmp(cmd, "forget")) {
        memset(&s_tmp.b, 0, sizeof s_tmp.b);
        s_csr_len = 0;
    } else if (!strncmp(cmd, "request ", 8)) {
        if (!name_ok(cmd + 8)) return "Enter a full host name in lower case, like gateway.example.com";
        snprintf(s_want_name, sizeof s_want_name, "%.63s", cmd + 8);
        s_csr_len = 0;
        s_next = JOB_REQUEST;
        return NULL;
    } else {
        return "Not understood";
    }
    if (!save()) return "Could not write to flash";
    activate_soon();
    return NULL;
}

static uint32_t unix_of(const mbedtls_x509_time *t)
{
    return (uint32_t)clock_days(t->year, t->mon, t->day) * 86400u + (uint32_t)(t->hour * 3600 + t->min * 60 + t->sec);
}

const char *tls_issued(uint32_t *from, uint32_t *to)
{
    if (!s_live || s_rec.b.kind != CERT_ISSUED) return NULL;
    *from = unix_of(&s_crt.valid_from);
    *to = unix_of(&s_crt.valid_to);
    return s_rec.b.name;
}

void tls_heap(size_t *now, size_t *peak)
{
    size_t blocks;
    *now = *peak = 0;
    if (!s_ready) return;
    mbedtls_memory_buffer_alloc_cur_get(now, &blocks);
    mbedtls_memory_buffer_alloc_max_get(peak, &blocks);
}

const char *tls_requested(void) { return s_rec.b.req ? s_rec.b.req_name : NULL; }

bool tls_busy(void)
{
    return (s_job != JOB_NONE && s_job != JOB_SHAKE) || s_next != JOB_NONE || s_soon || worker_busy(WORKER_TLS);
}

size_t tls_csr_der(uint8_t *out, size_t max)
{
    if (!s_csr_len || s_csr_len > max) return 0;
    memcpy(out, s_csr, s_csr_len);
    return s_csr_len;
}

const char *tls_name(void) { return s_live && s_rec.b.kind == CERT_ISSUED ? s_rec.b.name : NULL; }

size_t tls_csr(char *out, size_t max)
{
    static const char D[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    if (max < (s_csr_len + 2) / 3 * 4 + 16) return 0;
    n = (size_t)snprintf(out, max, "{\"csr\":\"");
    for (size_t i = 0; i < s_csr_len; i += 3) {
        uint32_t v = (uint32_t)s_csr[i] << 16 | (i + 1 < s_csr_len ? (uint32_t)s_csr[i + 1] << 8 : 0) | (i + 2 < s_csr_len ? s_csr[i + 2] : 0);
        out[n++] = D[v >> 18];
        out[n++] = D[v >> 12 & 63];
        out[n++] = i + 1 < s_csr_len ? D[v >> 6 & 63] : '=';
        out[n++] = i + 2 < s_csr_len ? D[v & 63] : '=';
    }
    memcpy(out + n, "\"}\n", 4);
    return n + 3;
}

size_t tls_json(char *out, size_t max)
{
    static const char *const KIND[] = { "none", "self", "issued" };
    const body_t *b = &s_rec.b;
    size_t n = 0, used = 0, peak = 0, blocks;
    int open = 0;
    worker_stats_t w;
    char fp[65] = "", until[12] = "";
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) return 0; \
        n += (size_t)w_; } while (0)
    if (s_ready) {
        mbedtls_memory_buffer_alloc_cur_get(&used, &blocks);
        mbedtls_memory_buffer_alloc_max_get(&peak, &blocks);
    }
    if (s_live) {
        sha256_t s;
        uint8_t d[32];
        sha256_init(&s);
        sha256_update(&s, s_crt.raw.p, s_crt.raw.len);
        sha256_final(&s, d);
        for (int i = 0; i < 32; i++) snprintf(fp + 2 * i, 3, "%02x", d[i]);
        snprintf(until, sizeof until, "%04u-%02u-%02u", (unsigned)s_crt.valid_to.year % 10000u, (unsigned)s_crt.valid_to.mon % 100u, (unsigned)s_crt.valid_to.day % 100u);
    }
    for (int i = 0; i < TLS_MAX; i++) open += s_t[i].state != T_FREE;
    worker_stats(WORKER_TLS, &w);
    bool busy = (s_job != JOB_NONE && s_job != JOB_SHAKE) || s_next != JOB_NONE || s_soon;
    OUT("{\"on\":%s,\"live\":%s,\"busy\":%s,\"kind\":\"%s\",\"name\":\"%s\",\"until\":\"%s\",\"sha256\":\"%s\",\"error\":\"%s\",",
        b->enabled ? "true" : "false", s_live ? "true" : "false", busy ? "true" : "false", KIND[b->kind % 3], b->name, until, fp, s_err);
    OUT("\"request\":\"%s\",\"csr\":%s,\"connections\":%d,\"handshakes\":%lu,\"failed\":%lu,\"refused\":%lu,"
        "\"last_ms\":%lu,\"last_slices\":%lu,\"slice_us\":%lu,\"stack\":[%lu,%lu],\"heap\":[%lu,%lu,%lu]}\n",
        b->req ? b->req_name : "", s_csr_len ? "true" : "false", open, (unsigned long)s_shakes, (unsigned long)s_failed,
        (unsigned long)s_refused, (unsigned long)s_last_ms, (unsigned long)s_last_slices, (unsigned long)w.longest_us,
        (unsigned long)w.stack_used, (unsigned long)w.stack_size, (unsigned long)used, (unsigned long)peak, (unsigned long)HEAP_SIZE);
    return n;
#undef OUT
}
