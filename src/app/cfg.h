/*
 * cfg.h — Device configuration: schema, text parsing, validation, JSON.
 * Pure C with no hardware dependencies; tests/test_cfg.c runs it on the host.
 *
 * Changes arrive as "key=value" lines (port1.baud=19200, net.mode=static).
 * A change set is applied to a copy, validated as a whole, then committed.
 */
#ifndef CFG_H
#define CFG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PORTS 4

enum { MODE_SERVER, MODE_CLIENT, MODE_UDP };
enum { PARITY_NONE, PARITY_EVEN, PARITY_ODD };

typedef struct {
    char     name[24];
    char     remote_host[48];      /* client: required. udp: blank = last sender */
    uint32_t baud;
    uint16_t local_port, remote_port;
    uint16_t gap_ms;               /* silence that ends a packet; 0 = 3.5 characters */
    uint16_t packet_len;           /* flush at this many bytes regardless */
    uint8_t  enabled, mode, data_bits, parity, stop_bits;
    uint8_t  echo_cancel;          /* drop what we hear while transmitting */
    uint8_t  loopback;             /* on-chip TX->RX, for testing without wiring */
} port_cfg_t;

typedef struct {
    uint8_t  dhcp;
    uint32_t ip, mask, gw, dns;    /* network byte order */
} net_cfg_t;

typedef struct {
    char       device_name[32];
    net_cfg_t  net;
    port_cfg_t port[PORTS];
    uint8_t    web_set;            /* a web password exists */
    uint8_t    web_salt[16], web_key[32];
} cfg_t;

void cfg_defaults(cfg_t *c);
/* Applies lines in `body` (modified in place) to *c. False + message on error. */
bool cfg_parse(cfg_t *c, char *body, char *err, size_t errlen);
bool cfg_validate(const cfg_t *c, char *err, size_t errlen);
/* JSON without secrets. Returns length, or 0 if it did not fit. */
size_t cfg_json(const cfg_t *c, char *out, size_t max);
/* "..." with escapes; returns chars written (no NUL counted). */
size_t json_str(const char *s, char *out, size_t max);
bool ip4_parse(const char *s, uint32_t *out);
void ip4_fmt(uint32_t ip, char out[16]);
#endif
