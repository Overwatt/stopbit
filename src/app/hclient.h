/*
 * hclient.h — HTTPS requests out, from a worker job (worker.h): each call waits by
 * yielding to the main loop. The server's certificate must lead to one of roots.c, and
 * the clock must be known. One connection at a time, kept open between requests to
 * the same host.
 */
#ifndef HCLIENT_H
#define HCLIENT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int    status;
    char  *body;                     /* within the caller's buffer, NUL-terminated */
    size_t len;
    char   location[160];
    char   nonce[96];                /* Replay-Nonce */
} hc_resp_t;

/* `headers`: extra lines, each ending "\r\n", or NULL. NULL, or what went wrong. */
const char *hc_request(const char *method, const char *host, const char *path, const char *headers,
                       const void *body, size_t blen, char *buf, size_t max, hc_resp_t *r);
void hc_close(void);
uint32_t hc_handshakes(void);
#endif
