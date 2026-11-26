/*
 * gcm_hw.c — AES-GCM for TLS records. mbedTLS's PSA layer (psa_crypto_aead.c) calls
 * these in place of mbedtls_gcm_* (renamed where they are made; see CMakeLists.txt).
 * For now they only time the calls, so the loop figures can say how much of a pass
 * was AES-GCM.
 */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/gcm.h"
#include "gcm_hw.h"
#include "stm32h7xx.h"

static uint32_t s_cycles;

uint32_t gcm_hw_cycles(void) { return s_cycles; }

int tls_gcm_setkey(mbedtls_gcm_context *ctx, mbedtls_cipher_id_t cipher, const unsigned char *key, unsigned int keybits);
int tls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode, size_t length, const unsigned char *iv, size_t iv_len,
                          const unsigned char *add, size_t add_len, const unsigned char *input, unsigned char *output,
                          size_t tag_len, unsigned char *tag);
int tls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                         const unsigned char *add, size_t add_len, const unsigned char *tag, size_t tag_len,
                         const unsigned char *input, unsigned char *output);
void tls_gcm_free(mbedtls_gcm_context *ctx);

int tls_gcm_setkey(mbedtls_gcm_context *ctx, mbedtls_cipher_id_t cipher, const unsigned char *key, unsigned int keybits)
{
    uint32_t t = DWT->CYCCNT;
    int r = mbedtls_gcm_setkey(ctx, cipher, key, keybits);
    s_cycles += DWT->CYCCNT - t;
    return r;
}

int tls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode, size_t length, const unsigned char *iv, size_t iv_len,
                          const unsigned char *add, size_t add_len, const unsigned char *input, unsigned char *output,
                          size_t tag_len, unsigned char *tag)
{
    uint32_t t = DWT->CYCCNT;
    int r = mbedtls_gcm_crypt_and_tag(ctx, mode, length, iv, iv_len, add, add_len, input, output, tag_len, tag);
    s_cycles += DWT->CYCCNT - t;
    return r;
}

int tls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                         const unsigned char *add, size_t add_len, const unsigned char *tag, size_t tag_len,
                         const unsigned char *input, unsigned char *output)
{
    uint32_t t = DWT->CYCCNT;
    int r = mbedtls_gcm_auth_decrypt(ctx, length, iv, iv_len, add, add_len, tag, tag_len, input, output);
    s_cycles += DWT->CYCCNT - t;
    return r;
}

void tls_gcm_free(mbedtls_gcm_context *ctx)
{
    mbedtls_gcm_free(ctx);
}
