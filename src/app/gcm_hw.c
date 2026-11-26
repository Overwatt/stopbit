/*
 * gcm_hw.c — AES-GCM for TLS records, on the chip's crypto engine (CRYP).
 *
 * mbedTLS's PSA layer (psa_crypto_aead.c) calls these in place of mbedtls_gcm_*
 * (renamed where they are made; see CMakeLists.txt). A record is one operation:
 * setkey, then crypt_and_tag or auth_decrypt, then free. setkey still sets up
 * mbedTLS's own context, so whatever the engine does not take (an IV other than
 * 12 bytes, a multipart operation, the engine switched off) runs in software.
 *
 * At start the engine is checked: a published test vector, then random lengths
 * both ways against mbedTLS in software. If it disagrees it is not used. If it
 * stops answering later, that operation fails (its output may already be
 * half-written, in place) and the rest go to software.
 */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/gcm.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"
#include "gcm_hw.h"
#include "stm32h7xx.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SLOTS 4                 /* contexts with a key at once: one per operation under way */
#define SPIN  100000u           /* status reads before giving up; a block takes a few dozen */

typedef struct { const mbedtls_gcm_context *ctx; uint16_t bits; uint8_t key[32]; } slot_t;
static slot_t s_slot[SLOTS];
static bool s_on;
static const char *s_why = "not checked yet";
static uint32_t s_cycles, s_hw, s_sw;

uint32_t gcm_hw_cycles(void) { return s_cycles; }

/* ---- the engine ------------------------------------------------------------ */
static bool wait_sr(uint32_t bit, bool set)
{
    for (uint32_t i = 0; i < SPIN; i++)
        if (!(CRYP->SR & bit) != set) return true;
    return false;
}

static bool wait_stopped(void)
{
    for (uint32_t i = 0; i < SPIN; i++)
        if (!(CRYP->CR & CRYP_CR_CRYPEN)) return true;
    return false;
}

static uint32_t be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Data goes in and out as bytes (DATATYPE 8-bit): a word is four bytes as they lie in memory. */
static void put_block(const uint8_t *p, size_t n)
{
    uint32_t w[4] = { 0, 0, 0, 0 };
    memcpy(w, p, n);
    CRYP->DIN = w[0];
    CRYP->DIN = w[1];
    CRYP->DIN = w[2];
    CRYP->DIN = w[3];
}

static void get_block(uint8_t *p, size_t n)
{
    uint32_t w[4];
    w[0] = CRYP->DOUT;
    w[1] = CRYP->DOUT;
    w[2] = CRYP->DOUT;
    w[3] = CRYP->DOUT;
    memcpy(p, w, n);
}

/* One whole GCM operation; the full 16-byte tag. In place is fine: each block is read
 * before its result is written. False if the engine did not answer. */
static bool engine(const uint8_t *key, unsigned bits, bool enc, const uint8_t iv[12],
                   const uint8_t *add, size_t add_len, const uint8_t *in, uint8_t *out, size_t len, uint8_t tag[16])
{
    const uint32_t cr = CRYP_CR_ALGOMODE_AES_GCM | CRYP_CR_DATATYPE_1 |
                        (bits == 256 ? CRYP_CR_KEYSIZE_1 : bits == 192 ? CRYP_CR_KEYSIZE_0 : 0) |
                        (enc ? 0 : CRYP_CR_ALGODIR);
    bool ok = false;
    CRYP->CR = 0;
    CRYP->CR = CRYP_CR_FFLUSH;
    CRYP->CR = cr;                                  /* init phase */
    if (bits == 256) { CRYP->K0LR = be(key); CRYP->K0RR = be(key + 4); key += 8; }
    if (bits >= 192) { CRYP->K1LR = be(key); CRYP->K1RR = be(key + 4); key += 8; }
    CRYP->K2LR = be(key); CRYP->K2RR = be(key + 4); CRYP->K3LR = be(key + 8); CRYP->K3RR = be(key + 12);
    CRYP->IV0LR = be(iv); CRYP->IV0RR = be(iv + 4); CRYP->IV1LR = be(iv + 8); CRYP->IV1RR = 2;   /* counter of the first block */
    CRYP->CR = cr | CRYP_CR_CRYPEN;
    if (!wait_stopped()) goto out;                  /* it works out the hash key, then stops */

    if (add_len) {                                  /* header phase: the additional data */
        CRYP->CR = cr | CRYP_CR_GCM_CCMPH_0 | CRYP_CR_CRYPEN;
        for (size_t i = 0; i < add_len; i += 16) {
            put_block(add + i, add_len - i < 16 ? add_len - i : 16);
            if (!wait_sr(CRYP_SR_IFEM, true)) goto out;
        }
        if (!wait_sr(CRYP_SR_BUSY, false)) goto out;
    }

    CRYP->CR = cr | CRYP_CR_GCM_CCMPH_1;            /* payload phase */
    CRYP->CR = cr | CRYP_CR_GCM_CCMPH_1 | CRYP_CR_CRYPEN;
    for (size_t i = 0; i < len; i += 16) {
        size_t n = len - i < 16 ? len - i : 16;
        if (n < 16 && enc) {                        /* keep the padding out of the tag */
            uint32_t c = cr | CRYP_CR_GCM_CCMPH_1 | (uint32_t)(16 - n) << CRYP_CR_NPBLB_Pos;
            CRYP->CR = c;
            CRYP->CR = c | CRYP_CR_CRYPEN;
        }
        put_block(in + i, n);
        if (!wait_sr(CRYP_SR_OFNE, true)) goto out;
        get_block(out + i, n);
    }
    if (!wait_sr(CRYP_SR_BUSY, false)) goto out;

    const uint32_t fin = (cr & ~CRYP_CR_ALGODIR) | CRYP_CR_GCM_CCMPH;   /* final phase: always "encrypt" */
    CRYP->CR = fin;
    CRYP->CR = fin | CRYP_CR_CRYPEN;
    CRYP->DIN = 0;                                  /* the lengths in bits, as numbers: not byte-swapped */
    CRYP->DIN = (uint32_t)add_len * 8u;
    CRYP->DIN = 0;
    CRYP->DIN = (uint32_t)len * 8u;
    if (!wait_sr(CRYP_SR_OFNE, true)) goto out;
    get_block(tag, 16);
    ok = true;
out:
    CRYP->CR = 0;
    return ok;
}

/* ---- the calls mbedTLS makes ---------------------------------------------------- */
static slot_t *find(const mbedtls_gcm_context *ctx)
{
    for (int i = 0; i < SLOTS; i++)
        if (s_slot[i].ctx == ctx) return &s_slot[i];
    return NULL;
}

static void forget(slot_t *s)
{
    mbedtls_platform_zeroize(s, sizeof *s);
}

static void stop(const char *why)
{
    s_on = false;
    s_why = why;
    for (int i = 0; i < SLOTS; i++) forget(&s_slot[i]);
}

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
    int r = mbedtls_gcm_setkey(ctx, cipher, key, keybits);
    slot_t *s = find(ctx);
    if (s) forget(s);
    if (r || !s_on || cipher != MBEDTLS_CIPHER_ID_AES || (keybits != 128 && keybits != 192 && keybits != 256)) return r;
    if (!s && !(s = find(NULL))) return r;          /* all in use: this one in software */
    s->ctx = ctx;
    s->bits = (uint16_t)keybits;
    memcpy(s->key, key, keybits / 8u);
    return r;
}

void tls_gcm_free(mbedtls_gcm_context *ctx)
{
    slot_t *s = find(ctx);
    if (s && ctx) forget(s);
    mbedtls_gcm_free(ctx);
}

/* The engine's slot for this operation, or NULL: software. */
static slot_t *usable(const mbedtls_gcm_context *ctx, size_t iv_len, size_t tag_len, const void *add, size_t add_len)
{
    if (!s_on || !ctx || iv_len != 12 || tag_len < 4 || tag_len > 16 || (add_len && !add)) return NULL;
    return find(ctx);
}

int tls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode, size_t length, const unsigned char *iv, size_t iv_len,
                          const unsigned char *add, size_t add_len, const unsigned char *input, unsigned char *output,
                          size_t tag_len, unsigned char *tag)
{
    uint32_t t = DWT->CYCCNT;
    int r;
    slot_t *s = usable(ctx, iv_len, tag_len, add, add_len);
    if (s) {
        uint8_t full[16];
        if (engine(s->key, s->bits, mode == MBEDTLS_GCM_ENCRYPT, iv, add, add_len, input, output, length, full)) {
            memcpy(tag, full, tag_len);
            r = 0;
        } else {
            stop("It stopped answering");
            mbedtls_platform_zeroize(output, length);
            r = PSA_ERROR_HARDWARE_FAILURE;
        }
        mbedtls_platform_zeroize(full, sizeof full);
        s_hw++;
    } else {
        r = mbedtls_gcm_crypt_and_tag(ctx, mode, length, iv, iv_len, add, add_len, input, output, tag_len, tag);
        s_sw++;
    }
    s_cycles += DWT->CYCCNT - t;
    return r;
}

int tls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length, const unsigned char *iv, size_t iv_len,
                         const unsigned char *add, size_t add_len, const unsigned char *tag, size_t tag_len,
                         const unsigned char *input, unsigned char *output)
{
    uint32_t t = DWT->CYCCNT;
    int r;
    slot_t *s = usable(ctx, iv_len, tag_len, add, add_len);
    if (s) {
        uint8_t full[16], diff = 0;
        if (engine(s->key, s->bits, false, iv, add, add_len, input, output, length, full)) {
            for (size_t i = 0; i < tag_len; i++) diff |= (uint8_t)(full[i] ^ tag[i]);
            r = diff ? MBEDTLS_ERR_GCM_AUTH_FAILED : 0;
        } else {
            stop("It stopped answering");
            r = PSA_ERROR_HARDWARE_FAILURE;
        }
        if (r) mbedtls_platform_zeroize(output, length);
        mbedtls_platform_zeroize(full, sizeof full);
        s_hw++;
    } else {
        r = mbedtls_gcm_auth_decrypt(ctx, length, iv, iv_len, add, add_len, tag, tag_len, input, output);
        s_sw++;
    }
    s_cycles += DWT->CYCCNT - t;
    return r;
}

/* ---- the check at start ---------------------------------------------------------- */
static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* One case against mbedTLS in software: encrypt, then decrypt (in place) what software made. */
static bool agrees(const uint8_t *key, unsigned bits, const uint8_t iv[12], const uint8_t *add, size_t add_len,
                   const uint8_t *pt, size_t len, const uint8_t *want_ct, const uint8_t *want_tag)
{
    static uint8_t sw[96], hw[96];
    uint8_t swt[16], hwt[16];
    if (len > sizeof sw) return false;
    if (!want_ct) {
        mbedtls_gcm_context c;
        mbedtls_gcm_init(&c);
        int r = mbedtls_gcm_setkey(&c, MBEDTLS_CIPHER_ID_AES, key, bits) ||
                mbedtls_gcm_crypt_and_tag(&c, MBEDTLS_GCM_ENCRYPT, len, iv, 12, add, add_len, pt, sw, 16, swt);
        mbedtls_gcm_free(&c);
        if (r) return false;
        want_ct = sw;
        want_tag = swt;
    }
    if (!engine(key, bits, true, iv, add, add_len, pt, hw, len, hwt)) return false;
    if (memcmp(hw, want_ct, len) || memcmp(hwt, want_tag, 16)) return false;
    memcpy(hw, want_ct, len);
    if (!engine(key, bits, false, iv, add, add_len, hw, hw, len, hwt)) return false;
    return !memcmp(hw, pt, len) && !memcmp(hwt, want_tag, 16);
}

/* The GCM paper's test case 4: AES-128, 60 bytes (a short last block), 20 bytes of additional data. */
static const uint8_t K4[16] = { 0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08 };
static const uint8_t IV4[12] = { 0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88 };
static const uint8_t A4[20] = { 0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xab,0xad,0xda,0xd2 };
static const uint8_t P4[60] = {
    0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
    0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
    0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
    0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39 };
static const uint8_t C4[60] = {
    0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,0x84,0xd0,0xd4,0x9c,
    0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,
    0x21,0xd5,0x14,0xb2,0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
    0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91 };
static const uint8_t T4[16] = { 0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47 };

void gcm_hw_init(void)
{
    if ((DBGMCU->IDCODE >> 16) < 0x2000u) { s_why = "This revision of the chip lacks what GCM needs"; return; }
    RCC->AHB2ENR |= RCC_AHB2ENR_CRYPEN;
    (void)RCC->AHB2ENR;
    RCC->AHB2RSTR |= RCC_AHB2RSTR_CRYPRST;
    RCC->AHB2RSTR &= ~RCC_AHB2RSTR_CRYPRST;
    if (!agrees(K4, 128, IV4, A4, sizeof A4, P4, sizeof P4, C4, T4)) { s_why = "It failed the published test"; return; }
    static uint8_t key[32], iv[12], add[40], pt[96];
    for (int k = 0; k < 96; k++) {
        for (unsigned i = 0; i < sizeof key; i++) key[i] = (uint8_t)rnd();
        for (unsigned i = 0; i < sizeof iv; i++) iv[i] = (uint8_t)rnd();
        for (unsigned i = 0; i < sizeof add; i++) add[i] = (uint8_t)rnd();
        for (unsigned i = 0; i < sizeof pt; i++) pt[i] = (uint8_t)rnd();
        size_t len = k < 34 ? (size_t)k : rnd() % 97u, add_len = rnd() % 41u;   /* every short length, then any */
        if (!agrees(key, k % 2 ? 256 : 128, iv, add, add_len, pt, len, NULL, NULL)) {
            s_why = "It disagreed with software";
            return;
        }
    }
    s_on = true;
    s_why = "";
}

size_t gcm_hw_json(char *out, size_t max)
{
    int n = snprintf(out, max, "{\"engine\":%s,\"why\":\"%s\",\"hw\":%lu,\"sw\":%lu}",
                     s_on ? "true" : "false", s_why, (unsigned long)s_hw, (unsigned long)s_sw);
    return n < 0 || (size_t)n >= max ? 0 : (size_t)n;
}
