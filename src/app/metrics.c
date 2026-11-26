/*
 * metrics.c — Prometheus's text format (0.0.4), one family to a part (see metrics.h).
 * Names start n540_; counters end _total; times are in seconds; ports are labelled by
 * number, with their names and modes in n540_port_info.
 */
#include "metrics.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct { char *o; size_t n, max; bool over; } out_t;

static void put(out_t *w, const char *fmt, ...)
{
    if (w->over) return;
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(w->o + w->n, w->max - w->n, fmt, a);
    va_end(a);
    if (k < 0 || (size_t)k >= w->max - w->n) { w->over = true; return; }
    w->n += (size_t)k;
}

static void head(out_t *w, const char *name, const char *type, const char *help)
{
    put(w, "# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
}

/* A label value: \, " and line ends escaped. Names are printable ASCII. */
static void label(out_t *w, const char *v)
{
    for (; *v && !w->over; v++) {
        if (*v == '\\' || *v == '"') put(w, "\\%c", *v);
        else if (*v == '\n') put(w, "\\n");
        else put(w, "%c", *v);
    }
}

static void secs_us(out_t *w, uint64_t us) { put(w, "%llu.%06llu", (unsigned long long)(us / 1000000u), (unsigned long long)(us % 1000000u)); }
static void secs_ms(out_t *w, uint64_t ms) { put(w, "%llu.%03llu", (unsigned long long)(ms / 1000u), (unsigned long long)(ms % 1000u)); }

/* One per port; gauge or counter of a field. */
#define PER_PORT(name, type, help, cond, value) do { \
        head(&w, name, type, help); \
        for (int i = 0; i < PORTS; i++) { const __typeof__(m->port[0]) *p = &m->port[i]; (void)p; \
            if (cond) put(&w, "%s{port=\"%d\"} %lu\n", name, i + 1, (unsigned long)(value)); } \
    } while (0)

static void histogram(out_t *w, const char *name, int port, const uint32_t *n, int buckets,
                      const uint32_t *bound, bool in_ms, uint64_t sum)
{
    uint32_t c = 0;
    for (int b = 0; b <= buckets; b++) {
        c += n[b];
        put(w, "%s_bucket{port=\"%d\",le=\"", name, port);
        if (b == buckets) put(w, "+Inf");
        else if (in_ms) secs_ms(w, bound[b]);
        else secs_us(w, bound[b]);
        put(w, "\"} %lu\n", (unsigned long)c);
    }
    put(w, "%s_sum{port=\"%d\"} ", name, port);
    if (in_ms) secs_ms(w, sum); else secs_us(w, sum);
    put(w, "\n%s_count{port=\"%d\"} %lu\n", name, port, (unsigned long)c);
}

int metrics_part(const metrics_t *m, uint32_t part, char *out, size_t max)
{
    static const char *const ERR[ERR_KINDS] = { "parity", "framing", "noise", "overrun", "buffer_full", "not_delivered", "refused" };
    static const uint32_t FWD_B[FWD_BUCKETS] = FWD_BOUNDS_US, MB_B[MB_HIST] = MB_BOUNDS_MS;
    out_t w = { out, 0, max, max == 0 };
    bool any_mb = false;
    for (int i = 0; i < PORTS; i++) any_mb |= m->port[i].modbus;
    switch (part) {
    case 0:
        head(&w, "n540_build_info", "gauge", "The firmware running.");
        put(&w, "n540_build_info{version=\"");
        label(&w, m->version);
        put(&w, "\"} 1\n");
        break;
    case 1:
        head(&w, "n540_uptime_seconds", "gauge", "Seconds since the gateway started.");
        put(&w, "n540_uptime_seconds %lu\n", (unsigned long)m->uptime_s);
        break;
    case 2:
        head(&w, "n540_starts_total", "counter", "Starts since the backup registers were cleared.");
        put(&w, "n540_starts_total %lu\n", (unsigned long)m->starts);
        break;
    case 3:
        head(&w, "n540_failed_starts_total", "counter", "Starts that did not run long enough to count.");
        put(&w, "n540_failed_starts_total %lu\n", (unsigned long)m->failed_starts);
        break;
    case 4:
        head(&w, "n540_loop_pause_seconds", "gauge", "Longest pause of the main loop, in the last 10 to 20 s and since the start.");
        put(&w, "n540_loop_pause_seconds{window=\"recent\"} ");
        secs_us(&w, m->loop_recent_us);
        put(&w, "\nn540_loop_pause_seconds{window=\"since_start\"} ");
        secs_us(&w, m->loop_max_us);
        put(&w, "\n");
        break;
    case 5:
        if (!m->temp_ok) break;
        head(&w, "n540_temperature_celsius", "gauge", "The processor's own sensor; above room temperature.");
        put(&w, "n540_temperature_celsius %s%ld.%ld\n", m->temp_dc < 0 ? "-" : "",
            (long)((m->temp_dc < 0 ? -m->temp_dc : m->temp_dc) / 10), (long)((m->temp_dc < 0 ? -m->temp_dc : m->temp_dc) % 10));
        break;
    case 6:
        head(&w, "n540_ethernet_up", "gauge", "1 while the Ethernet link is up.");
        put(&w, "n540_ethernet_up %d\n", m->link ? 1 : 0);
        break;
    case 7:
        head(&w, "n540_ethernet_frames_total", "counter", "Ethernet frames.");
        put(&w, "n540_ethernet_frames_total{dir=\"in\"} %lu\nn540_ethernet_frames_total{dir=\"out\"} %lu\n",
            (unsigned long)m->eth_rx, (unsigned long)m->eth_tx);
        break;
    case 8:
        head(&w, "n540_ethernet_errors_total", "counter", "Frames received with errors, and frames that could not be sent.");
        put(&w, "n540_ethernet_errors_total{kind=\"receive\"} %lu\nn540_ethernet_errors_total{kind=\"not_sent\"} %lu\n",
            (unsigned long)m->eth_rx_err, (unsigned long)m->eth_tx_drop);
        break;
    case 9:
        head(&w, "n540_https_handshakes_total", "counter", "HTTPS handshakes, completed and failed.");
        put(&w, "n540_https_handshakes_total{result=\"ok\"} %lu\nn540_https_handshakes_total{result=\"failed\"} %lu\n",
            (unsigned long)m->handshakes, (unsigned long)m->handshakes_failed);
        break;
    case 10:
        head(&w, "n540_aes_records_total", "counter", "TLS records encrypted or decrypted, by the crypto engine or in software.");
        put(&w, "n540_aes_records_total{engine=\"hardware\"} %lu\nn540_aes_records_total{engine=\"software\"} %lu\n",
            (unsigned long)m->aes_hw, (unsigned long)m->aes_sw);
        break;
    case 11:
        if (!m->mqtt) break;
        head(&w, "n540_mqtt_connected", "gauge", "1 while connected to the MQTT broker.");
        put(&w, "n540_mqtt_connected %d\n", m->mqtt_connected ? 1 : 0);
        break;
    case 12:
        head(&w, "n540_clock_synced", "gauge", "1 once a time server has answered since the start.");
        put(&w, "n540_clock_synced %d\n", m->clock_synced ? 1 : 0);
        break;
    case 13:
        head(&w, "n540_port_info", "gauge", "Each port's name and mode.");
        for (int i = 0; i < PORTS; i++) {
            put(&w, "n540_port_info{port=\"%d\",name=\"", i + 1);
            label(&w, m->port[i].name);
            put(&w, "\",mode=\"%s\"} 1\n", m->port[i].mode);
        }
        break;
    case 14: PER_PORT("n540_port_up", "gauge", "1 while the port is open and its network side is ready.", true, p->up); break;
    case 15: PER_PORT("n540_port_clients", "gauge", "Network clients connected to the port.", true, p->clients); break;
    case 16:
        head(&w, "n540_port_line_bytes_total", "counter", "Bytes heard and sent on the serial line.");
        for (int i = 0; i < PORTS; i++)
            put(&w, "n540_port_line_bytes_total{port=\"%d\",dir=\"heard\"} %lu\nn540_port_line_bytes_total{port=\"%d\",dir=\"sent\"} %lu\n",
                i + 1, (unsigned long)m->port[i].line_rx, i + 1, (unsigned long)m->port[i].line_tx);
        break;
    case 17:
        head(&w, "n540_port_network_bytes_total", "counter", "Bytes from and to the network.");
        for (int i = 0; i < PORTS; i++)
            put(&w, "n540_port_network_bytes_total{port=\"%d\",dir=\"in\"} %lu\nn540_port_network_bytes_total{port=\"%d\",dir=\"out\"} %lu\n",
                i + 1, (unsigned long)m->port[i].net_rx, i + 1, (unsigned long)m->port[i].net_tx);
        break;
    case 18:
        head(&w, "n540_port_errors_total", "counter", "Line errors and data that could not be delivered, by kind.");
        for (int i = 0; i < PORTS; i++)
            for (int k = 0; k < ERR_KINDS; k++)
                put(&w, "n540_port_errors_total{port=\"%d\",kind=\"%s\"} %lu\n", i + 1, ERR[k], (unsigned long)m->port[i].err[k]);
        break;
    case 19: PER_PORT("n540_port_turned_away_total", "counter", "Connections and datagrams the access list turned away.", true, p->turned_away); break;
    case 20:
        head(&w, "n540_port_heard_age_seconds", "gauge", "Seconds since anything was heard on the line.");
        for (int i = 0; i < PORTS; i++)
            if (m->port[i].heard_ms >= 0) {
                put(&w, "n540_port_heard_age_seconds{port=\"%d\"} ", i + 1);
                secs_ms(&w, (uint32_t)m->port[i].heard_ms);
                put(&w, "\n");
            }
        break;
    case 21:
        head(&w, "n540_port_forward_seconds", "histogram", "From a packet's last byte on the line (the silence that ends it included) to its sending on the network.");
        for (int i = 0; i < PORTS; i++)
            if (!m->port[i].modbus)
                histogram(&w, "n540_port_forward_seconds", i + 1, m->port[i].fwd.n, FWD_BUCKETS, FWD_B, false, m->port[i].fwd.sum_us);
        break;
    default: {
        /* Modbus gateway ports only, and only if there are any. */
        static const struct { const char *name, *help; size_t off; } MB[] = {
            { "n540_modbus_requests_total",      "Requests taken from Modbus TCP clients.",               offsetof(mb_stats_t, requests) },
            { "n540_modbus_replies_total",       "Replies from devices, error replies included.",          offsetof(mb_stats_t, replies) },
            { "n540_modbus_device_errors_total", "Error replies (exceptions) from devices.",               offsetof(mb_stats_t, exceptions) },
            { "n540_modbus_timeouts_total",      "Requests no device answered in time.",                   offsetof(mb_stats_t, timeouts) },
            { "n540_modbus_bad_frames_total",    "Frames heard with a bad CRC, or for another unit or function.", offsetof(mb_stats_t, bad) },
            { "n540_modbus_busy_total",          "Requests turned away because 8 were waiting.",           offsetof(mb_stats_t, busy) },
            { "n540_modbus_refused_total",       "Requests refused: a function the client may not use, or a unit no device can have.", offsetof(mb_stats_t, refused) },
            { "n540_modbus_broadcasts_total",    "Broadcasts (unit 0) sent.",                              offsetof(mb_stats_t, broadcasts) },
        };
        uint32_t k = part - 22;
        if (k > sizeof MB / sizeof MB[0] + 1) return -1;
        if (!any_mb) break;
        if (k < sizeof MB / sizeof MB[0]) {
            PER_PORT(MB[k].name, "counter", MB[k].help, p->modbus, *(const uint32_t *)((const uint8_t *)&p->mb + MB[k].off));
        } else if (k == sizeof MB / sizeof MB[0]) {
            PER_PORT("n540_modbus_waiting", "gauge", "Requests waiting for the line.", p->modbus, p->mb_waiting);
        } else {
            head(&w, "n540_modbus_reply_seconds", "histogram", "From a request's sending to its reply.");
            for (int i = 0; i < PORTS; i++)
                if (m->port[i].modbus)
                    histogram(&w, "n540_modbus_reply_seconds", i + 1, m->port[i].mb.hist, MB_HIST, MB_B, true, m->port[i].mb.total_ms);
        }
    }
    }
    return w.over ? -2 : (int)w.n;
}
