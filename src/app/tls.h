/*
 * tls.h — HTTPS on port 443, beside plain HTTP on 80.
 *
 * Handshakes run in the worker; records are handled in the main loop. The web server
 * (http.c) sees a stream: tls.c hands it plaintext only when it asks for some.
 * The private key is made here and only a signing request leaves the gateway.
 */
#ifndef TLS_H
#define TLS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct tls tls_t;

void tls_init(void);                     /* after the flash and network are up */
void tls_poll(void);

/* ---- for http.c ---- */
uint32_t tls_room(const tls_t *t);       /* plaintext it takes right now */
void tls_write(tls_t *t, const void *d, uint32_t n);   /* n <= tls_room() */
void tls_close(tls_t *t);                /* once what was written is sent */
/* The name on the installed certificate, if a browser would accept it, or NULL. */
const char *tls_name(void);
/* Blanks the private keys in a flash dump. */
void tls_mask(int dev, uint32_t addr, uint8_t *buf, uint32_t n);

/* TLS memory: in use now, and at most so far (bytes). */
void tls_heap(size_t *now, size_t *peak);
void tls_counts(uint32_t *ok, uint32_t *failed);   /* handshakes */

/* ---- for acme.c ---- */
/* The issued certificate's name and validity (unix seconds), or NULL if there is none. */
const char *tls_issued(uint32_t *from, uint32_t *to);
const char *tls_requested(void);         /* name of the pending signing request, or NULL */
bool tls_busy(void);                     /* a command would be refused for now */
size_t tls_csr_der(uint8_t *out, size_t max);

/* ---- implemented by http.c ---- */
void *http_tls_open(tls_t *t);           /* NULL: no room for another */
uint32_t http_tls_want(void *c);
void http_tls_data(void *c, const uint8_t *d, uint32_t n);   /* may close */
void http_tls_sent(void *c);             /* there may be room again */
void http_tls_gone(void *c);             /* the connection was lost */
bool http_tls_idle(void *c);             /* between requests */

/* ---- for api.c ---- */
size_t tls_json(char *out, size_t max);
/* on | off | request <name> | cert <DER chain> | forget. NULL, or why not. */
const char *tls_command(const uint8_t *body, size_t len);
/* {"csr":"<base64>"}, empty while there is none. 0 if it did not fit. */
size_t tls_csr(char *out, size_t max);
#endif
