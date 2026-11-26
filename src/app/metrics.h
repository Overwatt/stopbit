/*
 * metrics.h — The gateway's figures in Prometheus's text format, for GET /metrics. A
 * snapshot (metrics_t, filled by api.c) is written out a part at a time, one metric
 * family to a part, as the connection has room (http.c sends them as chunks).
 */
#ifndef METRICS_H
#define METRICS_H
#include "bridge.h"
#include "modbus.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ERR_PARITY, ERR_FRAMING, ERR_NOISE, ERR_OVERRUN, ERR_BUFFER_FULL, ERR_NOT_DELIVERED, ERR_REFUSED, ERR_KINDS };

typedef struct {
    const char *version;
    uint32_t uptime_s, starts, failed_starts;
    uint32_t loop_recent_us, loop_max_us;
    bool     temp_ok, link, mqtt, mqtt_connected, clock_synced;
    int32_t  temp_dc;                       /* tenths of a degree */
    uint32_t eth_rx, eth_tx, eth_rx_err, eth_tx_drop;
    uint32_t handshakes, handshakes_failed, aes_hw, aes_sw;
    struct {
        const char *name, *mode;
        bool     up, modbus;
        uint32_t clients, line_rx, line_tx, net_rx, net_tx, turned_away;
        uint32_t err[ERR_KINDS];
        int32_t  heard_ms;                  /* -1: nothing yet */
        fwd_t    fwd;
        mb_stats_t mb;
        uint8_t  mb_waiting;
    } port[PORTS];
} metrics_t;

/* Part `part`: its length (0 if it has nothing to say), -1 past the last, -2 if it does
 * not fit in max. Each part is one family, so its samples stay together. */
int metrics_part(const metrics_t *m, uint32_t part, char *out, size_t max);

#endif
