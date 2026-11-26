/*
 * mqtt_tls.h — TLS under lwIP's MQTT client. When mqtt_connect_client_info_t.tls_config
 * is set, lwIP's altcp_tls_new() makes a TCP connection and wraps it with
 * altcp_tls_wrap(), which this supplies. The broker's certificate must lead to one of
 * roots.c and name the host. The handshake runs as a job (worker.h) in slices; records
 * are handled in the main loop. One connection at a time; the session is kept, so the
 * next connection to the same host resumes it.
 */
#ifndef MQTT_TLS_H
#define MQTT_TLS_H
#include <stdbool.h>
#include <stdint.h>

struct altcp_tls_config;
/* The configuration for a connection to `host` (checked against its certificate). */
struct altcp_tls_config *mqtt_tls_config(const char *host);
/* Main loop: runs the handshake job and tells MQTT how it ended. */
void mqtt_tls_poll(void);
/* Why the last connection failed in TLS, or "" (cleared when a new one starts). */
const char *mqtt_tls_error(void);
/* While connected: the protocol ("TLSv1.2") and the certificate's last day
 * ("2027-02-24"); NULL otherwise. */
const char *mqtt_tls_version(void);
const char *mqtt_tls_until(void);
uint32_t mqtt_tls_handshakes(void);       /* full and resumed */
uint32_t mqtt_tls_resumed(void);
#endif
