#ifndef SHA256_H
#define SHA256_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; } sha256_t;
void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[32]);
/* One-shot HMAC-SHA256 over two concatenated parts (b may be NULL/0). */
void hmac_sha256(const uint8_t key[32], const void *a, size_t alen,
                 const void *b, size_t blen, uint8_t out[32]);
int ct_equal(const void *a, const void *b, size_t n);
#endif
