/*
 * modbus.h — Modbus TCP to Modbus RTU for one port: requests from up to MB_CLIENTS
 * connections go on the line one at a time, and each reply goes back to whoever asked.
 * No network code: bridge.c passes bytes in, with the time, and gets bytes out through
 * mb_io_t. See docs/superpowers/specs/2026-11-26-modbus-gateway-design.md.
 */
#ifndef MODBUS_H
#define MODBUS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MB_CLIENTS  4
#define MB_QUEUE    8           /* waiting at once, across clients */
#define MB_PDU      253         /* function and data */
#define MB_TURN_MS  100u        /* after a broadcast, before the next request */
#define MB_HIST     8           /* reply times: counts per bucket, the last for longer */
#define MB_BOUNDS_MS { 10, 25, 50, 100, 250, 500, 1000, 2500 }

typedef struct {
    uint32_t requests;          /* taken from clients */
    uint32_t replies;           /* from devices, exceptions included */
    uint32_t exceptions;        /* of those, exceptions */
    uint32_t timeouts;          /* no valid reply in time: answered 0x0B */
    uint32_t bad;               /* frames heard with a bad CRC, or for another unit or function */
    uint32_t busy;              /* queue full: answered 0x06 */
    uint32_t refused;           /* a function the client may not use (0x01), or a unit no device can
                                   have (0x0A): 0 without broadcasts, 248 to 255 */
    uint32_t broadcasts;
    uint32_t last_ms, max_ms;   /* response time */
    uint64_t total_ms;          /* over `replies`, for the average */
    uint32_t hist[MB_HIST + 1];
} mb_stats_t;

typedef struct {
    size_t (*line)(void *ctx, const uint8_t *d, size_t n);          /* all of it, or 0: not now */
    void   (*reply)(void *ctx, int client, const uint8_t *d, size_t n);
    void   *ctx;
} mb_io_t;

typedef struct {
    int8_t   client;            /* -1: whoever asked has gone */
    uint8_t  unit;
    uint16_t tid, len;          /* len: of pdu */
    uint8_t  pdu[MB_PDU];
} mb_req_t;

typedef struct {
    mb_io_t  io;
    uint32_t timeout_ms, gap_us;
    bool     broadcast;         /* unit 0 goes to every device; else it is refused (bridge sets it) */
    struct { uint8_t buf[7 + MB_PDU]; uint16_t n; bool open, may_write; } cl[MB_CLIENTS];
    mb_req_t q[MB_QUEUE];       /* q[0] is the one on the line while state != MB_IDLE */
    uint8_t  qn;
    uint8_t  state;
    uint32_t at;                /* ms: when the request went (or finished going) out */
    uint8_t  rx[256];
    uint16_t rxn;
    bool     rx_over;
    mb_stats_t s;
} modbus_t;

enum { MB_IDLE, MB_SENDING, MB_WAITING, MB_TURNAROUND };

uint16_t modbus_crc(const uint8_t *d, size_t n);

/* gap_us: the silence that ends a frame (3.5 characters, or the port's setting). */
void modbus_init(modbus_t *m, const mb_io_t *io, uint32_t timeout_ms, uint32_t gap_us);
void modbus_open(modbus_t *m, int client, bool may_write);
void modbus_may_write(modbus_t *m, int client, bool may_write);    /* its access changed */
/* Gone: what it had waiting is dropped; one on the line finishes, unanswered. */
void modbus_close(modbus_t *m, int client);
/* Bytes from a client, in any pieces. False: not Modbus TCP; close the connection. */
bool modbus_from_client(modbus_t *m, int client, const uint8_t *d, size_t n, uint32_t now_ms);
/* Bytes heard on the line. */
void modbus_from_line(modbus_t *m, const uint8_t *d, size_t n);
/* quiet_us: since the last byte heard; tx_idle: all sent, the line released. */
void modbus_poll(modbus_t *m, uint32_t now_ms, uint32_t quiet_us, bool tx_idle);

#endif
