/*
 * cli.h — The command console. Pure text in, text out: no I/O and no hardware,
 * so any transport can carry it and tests/test_cli.c can fuzz it on the host.
 * The cli_* hooks below are implemented by api.c (or by the test).
 */
#ifndef CLI_H
#define CLI_H
#include "cfg.h"

#define CLI_OUT 4000

/* Runs one command line (modified in place). Always leaves a string in `out`. */
void cli_run(char *line, char *out, size_t max);

typedef struct {
    const char *state;
    int clients;
    char peers[112];
    uint32_t serial_rx, serial_tx, net_lost, rx_lost, parity, framing, noise, overrun, refused;
} cli_port_t;

typedef struct {
    const char *version, *host, *mqtt_state, *mqtt_error;
    char ip[16], mask[16], gateway[16], mac[18];
    uint32_t uptime_s, now_ms, boots, crashes, reset_log[6];
    int slot, speed, net_pending_s;              /* -1: not pending */
    bool full_duplex;
} cli_sys_t;

const cfg_t *cli_cfg(void);
/* Applies "key=value" lines as one change. *pending: network is on trial. */
bool cli_change(char *lines, char *err, size_t errlen, bool *pending);
bool cli_keep(char *err, size_t errlen);
void cli_port(int port, cli_port_t *out);
void cli_sys(cli_sys_t *out);
/* Returns false with a reason if the port is off or the bytes do not fit. */
bool cli_send(int port, const uint8_t *d, size_t n, char *err, size_t errlen);
/* Sends a pattern on each port and reports who hears it. */
void cli_wiretest(char *out, size_t max);
void cli_restart(void);
#endif
