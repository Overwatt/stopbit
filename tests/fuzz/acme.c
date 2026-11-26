/*
 * acme.c — Renewal (src/app/acme.c, included, so its steps can be called) against
 * replies from "Let's Encrypt" and "Route 53" that the input makes up: each request
 * gets the next reply. The crypto is the real mbedTLS, so requests are signed as on the
 * gateway; HTTPS and the rest are stand-ins.
 *
 * Input: a byte of set-up (bit 0: an account key exists; bit 1: a test, against
 * staging; bit 2: settings instead), then replies, each:
 *   status (index into ST; bit 7: no Replay-Nonce), Location length + Location,
 *   2-byte body length + body.
 * For settings, the rest is what POST /api/acme carries: the text, then a zero byte
 * and the 64 bytes of the secret, if there is one.
 * Either way the status (GET /api/acme) must then be good JSON.
 */
#include "../../src/app/acme.c"
#include "stm32h7xx_hal.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "../flash_sim.h"
#include "json_ok.h"
#include <assert.h>
#include <unistd.h>

t_dwt_t t_dwt;
uint32_t t_dwt_now(void) { return t_now * 400000u; }
void NVIC_SystemReset(void) { _exit(0); }
uint32_t g_reset_cause;
bool rng_fill(void *d, uint32_t n) { static uint32_t x = 1; for (uint32_t i = 0; i < n; i++) ((uint8_t *)d)[i] = (uint8_t)((x = x * 1103515245u + 12345u) >> 16); return true; }
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *bits, unsigned char *out, size_t len)
{ (void)flags; rng_fill(out, (uint32_t)len); *bits = 8 * len; return 0; }
struct mbedtls_mpi; struct mbedtls_sha256_context;
int mbedtls_mpi_mul_mpi(struct mbedtls_mpi *X, const struct mbedtls_mpi *A, const struct mbedtls_mpi *B);
int tls_mpi_mul_mpi(struct mbedtls_mpi *X, const struct mbedtls_mpi *A, const struct mbedtls_mpi *B) { return mbedtls_mpi_mul_mpi(X, A, B); }
int mbedtls_sha256_update(struct mbedtls_sha256_context *c, const unsigned char *d, size_t n);
int tls_sha256_update(struct mbedtls_sha256_context *c, const unsigned char *d, size_t n) { return mbedtls_sha256_update(c, d, n); }
void *tls_core_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void events_add(ev_kind_t k, int port, uint32_t a, const char *s) { (void)k; (void)port; (void)a; (void)s; }
void worker_yield(void) { t_now += 100; }
void worker_breathe(void) { }
bool worker_start(int w, void (*fn)(void *), void *arg) { (void)w; (void)fn; (void)arg; return false; }
void worker_poll(int w) { (void)w; }
bool worker_busy(int w) { (void)w; return false; }
bool worker_inside(int w) { (void)w; return true; }
void worker_stats(int w, worker_stats_t *o) { (void)w; memset(o, 0, sizeof *o); }

/* HTTPS: what renewal needs of it */
const char *tls_issued(uint32_t *from, uint32_t *to) { *from = 1790000000u; *to = 1797776000u; return "gateway.example.com"; }
const char *tls_requested(void) { return "gateway.example.com"; }
bool tls_busy(void) { return false; }
const char *tls_command(const uint8_t *b, size_t n) { assert(n < 8192); (void)b; return NULL; }
size_t tls_csr_der(uint8_t *out, size_t max) { assert(max >= 300); memset(out, 0x30, 300); return 300; }

/* The replies */
static const uint8_t *s_in;
static size_t s_n, s_at;
uint32_t hc_handshakes(void) { return 0; }
void hc_close(void) { }
const char *hc_request(const char *method, const char *host, const char *path, const char *headers,
                       const void *body, size_t blen, char *buf, size_t max, hc_resp_t *r)
{
    static const int ST[] = { 200, 201, 204, 400, 403, 404, 500, 202 };
    (void)method; (void)headers; (void)body;
    assert(host && path && path[0] == '/' && blen < max);
    memset(r, 0, sizeof *r);
    if (s_at + 4 > s_n) return "no answer";
    uint8_t st = s_in[s_at++];
    r->status = ST[st & 7];
    if (!(st & 0x80)) snprintf(r->nonce, sizeof r->nonce, "nonce%u", (unsigned)s_at);
    size_t ln = s_in[s_at++];
    if (ln > s_n - s_at) ln = s_n - s_at;
    if (ln >= sizeof r->location) ln = sizeof r->location - 1;
    memcpy(r->location, s_in + s_at, ln);
    s_at += ln;
    if (s_at + 2 > s_n) return "answer cut short";
    size_t bn = (size_t)s_in[s_at] | (size_t)s_in[s_at + 1] << 8;
    s_at += 2;
    if (bn > s_n - s_at) bn = s_n - s_at;
    if (bn >= max) bn = max - 1;
    memcpy(buf, s_in + s_at, bn);
    s_at += bn;
    buf[bn] = 0;
    r->body = buf;
    r->len = bn;
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static unsigned char heap[180000];
    if (!n) return 0;
    memset(F, 0xFF, sizeof F);
    mbedtls_memory_buffer_alloc_init(heap, sizeof heap);
    if (psa_crypto_init() != PSA_SUCCESS) return 0;
    t_now = 1000;
    clock_set(1795600000u);
    acme_init();
    memset(&s_rec, 0, sizeof s_rec);
    snprintf(s_rec.b.zone, sizeof s_rec.b.zone, "Z0123456789ABC");
    snprintf(s_rec.b.key_id, sizeof s_rec.b.key_id, "AKIAEXAMPLEEXAMPLE00");
    snprintf(s_rec.b.secret, sizeof s_rec.b.secret, "example-secret");
    s_rec.b.has_secret = 1;
    s_staging = d[0] & 2;
    if (d[0] & 1) {                                   /* an account key kept from before */
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_key_id_t k = 0;
        size_t m;
        psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&a, 256);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_EXPORT);
        if (psa_generate_key(&a, &k) == PSA_SUCCESS && psa_export_key(k, s_rec.b.account, 32, &m) == PSA_SUCCESS &&
            psa_export_public_key(k, s_rec.b.account_pub, 65, &m) == PSA_SUCCESS) s_rec.b.has_account = 1;
        psa_destroy_key(k);
    }
    static char out[2048];
    if (d[0] & 4) {                                   /* settings, as POST /api/acme */
        static char text[4096];
        static uint8_t secret[64];
        const uint8_t *z = memchr(d + 1, 0, n - 1);
        size_t tn = z ? (size_t)(z - d - 1) : n - 1;
        if (tn >= sizeof text) return 0;
        memcpy(text, d + 1, tn);
        text[tn] = 0;
        bool has = z && (size_t)(d + n - z - 1) >= sizeof secret;
        if (has) memcpy(secret, z + 1, sizeof secret);
        const char *why = acme_command(text, has ? secret : NULL, sizeof secret);
        if (why) assert(*why && strlen(why) < 200);
        acme_poll();
    } else {                                          /* a renewal, from the start */
        s_in = d + 1; s_n = n - 1; s_at = 0;
        run(NULL);
        assert(s_result[0] && strlen(s_result) < sizeof s_result);
        if (getenv("ACME_SAY")) fprintf(stderr, "%s\n", s_result);   /* to check a seed goes where meant */
    }
    size_t k = acme_json(out, sizeof out);
    assert(k && json_ok(out));
    return 0;
}
