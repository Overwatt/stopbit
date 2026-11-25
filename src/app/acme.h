/*
 * acme.h — Renews the HTTPS certificate with Let's Encrypt (ACME, RFC 8555), proving
 * the name with a DNS record in Route 53 (dns-01). Runs as a worker job, so it waits
 * on the network without holding anything up.
 *
 * The AWS key should be allowed nothing but that one TXT record; see docs/HTTPS.md.
 * A new private key is made on the gateway for each certificate.
 */
#ifndef ACME_H
#define ACME_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

void acme_init(void);
void acme_poll(void);                /* main loop: starts a renewal when one is due */
size_t acme_json(char *out, size_t max);
/* Lines of "zone=", "key_id=", "auto=0|1", "secret=<64 bytes>" (the caller decrypts
 * it), or one of "renew" (now), "test" (staging, installs nothing). NULL, or why not. */
const char *acme_command(char *body, const uint8_t *secret, size_t secret_len);
/* Blanks the secrets in a flash dump. */
void acme_mask(int dev, uint32_t addr, uint8_t *buf, uint32_t n);
#endif
