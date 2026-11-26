/*
 * test_roots.c — The gateway's own trusted roots (src/app/roots.c), with mbedTLS as on the
 * gateway: each parses, and Let's Encrypt's newer intermediates (tests/roots/*.der:
 * YE1-3 under ISRG Root YE, YR1-3 under ISRG Root YR) verify against them, as when a
 * server sends the short chain without the cross-signed root. Without Root YE and YR
 * they must not. Built and run by tests/mqtt_tls/run.sh:
 *   test_roots tests/roots
 */
#include "roots.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* A fixed day inside every certificate here, so the test does not age. */
time_t time(time_t *t) { time_t v = 1795737600; if (t) *t = v; return v; }   /* 2026-11-26 */
mbedtls_ms_time_t mbedtls_ms_time(void) { return 0; }
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *bits, unsigned char *out, size_t len)
{
    (void)flags;
    FILE *f = fopen("/dev/urandom", "rb");
    size_t n = f ? fread(out, 1, len, f) : 0;
    if (f) fclose(f);
    *bits = 8 * n;
    return n == len ? 0 : -1;
}
struct mbedtls_mpi; struct mbedtls_sha256_context;
int mbedtls_mpi_mul_mpi(struct mbedtls_mpi *X, const struct mbedtls_mpi *A, const struct mbedtls_mpi *B);
int tls_mpi_mul_mpi(struct mbedtls_mpi *X, const struct mbedtls_mpi *A, const struct mbedtls_mpi *B) { return mbedtls_mpi_mul_mpi(X, A, B); }
void *tls_core_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int mbedtls_sha256_update(struct mbedtls_sha256_context *c, const unsigned char *d, size_t n);
int tls_sha256_update(struct mbedtls_sha256_context *c, const unsigned char *d, size_t n) { return mbedtls_sha256_update(c, d, n); }

/* Verifies the intermediate in DIR/name.der against roots [0, n); the flags. */
static uint32_t verify(const char *dir, const char *name, unsigned n)
{
    static unsigned char der[4096];
    char p[512];
    snprintf(p, sizeof p, "%s/%s.der", dir, name);
    FILE *f = fopen(p, "rb");
    assert(f);
    size_t len = fread(der, 1, sizeof der, f);
    fclose(f);
    mbedtls_x509_crt roots, c;
    mbedtls_x509_crt_init(&roots);
    mbedtls_x509_crt_init(&c);
    for (unsigned i = 0; i < n; i++) assert(!mbedtls_x509_crt_parse_der_nocopy(&roots, g_roots[i].der, g_roots[i].len));
    assert(!mbedtls_x509_crt_parse_der(&c, der, len));
    uint32_t flags = 0;
    mbedtls_x509_crt_verify(&c, &roots, NULL, NULL, &flags, NULL, NULL);
    mbedtls_x509_crt_free(&c);
    mbedtls_x509_crt_free(&roots);
    return flags;
}

int main(int argc, char **argv)
{
    static unsigned char heap[180000];
    assert(argc == 2);
    mbedtls_memory_buffer_alloc_init(heap, sizeof heap);
    assert(psa_crypto_init() == PSA_SUCCESS);
    assert(g_nroots == 6);
    static const char *const INT[] = { "ye1", "ye2", "ye3", "yr1", "yr2", "yr3" };
    for (unsigned i = 0; i < 6; i++) {
        uint32_t with = verify(argv[1], INT[i], g_nroots), without = verify(argv[1], INT[i], 4);
        printf("%s: %s with Root YE and YR, %s without\n", INT[i], with ? "REFUSED" : "trusted", without & MBEDTLS_X509_BADCERT_NOT_TRUSTED ? "not trusted" : "TRUSTED");
        assert(!with && (without & MBEDTLS_X509_BADCERT_NOT_TRUSTED));
    }
    puts("ok");
    return 0;
}
