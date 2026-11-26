/* test_metrics.c — GET /metrics's text (src/app/metrics.c): every part fits the reply
 * buffer at its widest, and the whole is well-formed Prometheus text format 0.0.4.
 *   cc -Isrc/app tests/test_metrics.c src/app/metrics.c -o /tmp/me && /tmp/me */
#include "metrics.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PART_MAX (4600 - 8 - 2)     /* http.c: RESP_MAX less a chunk's own bytes */

static char all[64000];
static size_t alln;

/* Everything: returns the number of parts with something in them. */
static int write_all(const metrics_t *m, size_t max)
{
    static char b[PART_MAX];
    int parts = 0, n;
    alln = 0;
    for (uint32_t p = 0; (n = metrics_part(m, p, b, max)) != -1; p++) {
        assert(n >= 0);                                   /* -2: it did not fit */
        assert(p < 100);
        memcpy(all + alln, b, (size_t)n);
        alln += (size_t)n;
        parts += n > 0;
    }
    all[alln] = 0;
    return parts;
}

static bool name_ok(const char *s, size_t n)
{
    if (!n || !(isalpha((unsigned char)s[0]) || s[0] == '_' || s[0] == ':')) return false;
    for (size_t i = 1; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == ':')) return false;
    return true;
}

/* The text format, strictly enough: see the checks below. */
static void check(const char *t, int ports_mb)
{
    char fam[64] = "", type[16] = "", seen[64][64];
    int nseen = 0, samples = 0, hist_ports = 0;
    double last_bucket = -1;
    char last_series[128] = "";
    for (const char *l = t; *l; ) {
        const char *e = strchr(l, '\n');
        assert(e);                                         /* every line ends */
        size_t n = (size_t)(e - l);
        char line[600];
        assert(n < sizeof line);
        memcpy(line, l, n); line[n] = 0;
        if (!strncmp(line, "# HELP ", 7)) {
            char *sp = strchr(line + 7, ' ');
            assert(sp && name_ok(line + 7, (size_t)(sp - line - 7)) && sp[1]);
            for (char *c = sp; *c; c++) assert(*c >= 0x20 && *c < 0x7f && *c != '\\');
        } else if (!strncmp(line, "# TYPE ", 7)) {
            char nm[64], ty[16];
            assert(sscanf(line + 7, "%63s %15s", nm, ty) == 2);
            assert(name_ok(nm, strlen(nm)));
            assert(!strcmp(ty, "counter") || !strcmp(ty, "gauge") || !strcmp(ty, "histogram"));
            for (int k = 0; k < nseen; k++) assert(strcmp(seen[k], nm));   /* a family once: its samples together */
            assert(nseen < 64);
            strcpy(seen[nseen++], nm);
            strcpy(fam, nm); strcpy(type, ty);
            if (!strcmp(ty, "counter")) assert(strlen(nm) > 6 && !strcmp(nm + strlen(nm) - 6, "_total"));
        } else {
            /* name{label="value",...} value */
            const char *p = line;
            while (*p && *p != '{' && *p != ' ') p++;
            size_t nl = (size_t)(p - line);
            assert(name_ok(line, nl));
            char nm[64];
            memcpy(nm, line, nl); nm[nl] = 0;
            assert(fam[0]);                                    /* declared first */
            if (!strcmp(type, "histogram")) {
                size_t f = strlen(fam);
                assert(!strncmp(nm, fam, f) && (!strcmp(nm + f, "_bucket") || !strcmp(nm + f, "_sum") || !strcmp(nm + f, "_count")));
            } else {
                assert(!strcmp(nm, fam));
            }
            char series[128] = "", le[32] = "";
            if (*p == '{') {
                p++;
                while (*p != '}') {
                    const char *k = p;
                    while (*p && *p != '=') p++;
                    assert(*p == '=' && name_ok(k, (size_t)(p - k)) && p[1] == '"');
                    bool is_le = p - k == 2 && !strncmp(k, "le", 2);
                    p += 2;
                    char v[64]; size_t vn = 0;
                    while (*p != '"') {
                        assert(*p);
                        if (*p == '\\') { p++; assert(*p == '\\' || *p == '"' || *p == 'n'); }
                        assert(vn < sizeof v - 1);
                        v[vn++] = *p++;
                    }
                    v[vn] = 0;
                    p++;
                    if (is_le) strcpy(le, v);
                    else { strncat(series, k, (size_t)(p - k)); }
                    if (*p == ',') p++;
                    else assert(*p == '}');
                }
                p++;
            }
            assert(*p == ' ');
            p++;
            char *end;
            double v = strtod(p, &end);
            if (!strcmp(p, "+Inf")) v = 1e300; else assert(*end == 0 && end != p && (v >= 0 || !strcmp(type, "gauge")));
            samples++;
            if (!strcmp(type, "histogram")) {
                size_t f = strlen(fam);
                if (!strcmp(nm + f, "_bucket")) {
                    assert(le[0]);
                    if (strcmp(series, last_series)) { last_bucket = -1; strcpy(last_series, series); }
                    assert(v >= last_bucket);                  /* cumulative */
                    last_bucket = v;
                } else if (!strcmp(nm + f, "_count")) {
                    assert(v == last_bucket);                   /* +Inf's count */
                    hist_ports++;
                }
            }
        }
        l = e + 1;
    }
    assert(samples > 50);
    (void)ports_mb;
    printf("  %d families, %d samples, %d histograms\n", nseen, samples, hist_ports);
}

static void widest(metrics_t *m, bool modbus)
{
    memset(m, 0, sizeof *m);
    m->version = "1234567890abcdef-dirty";
    m->uptime_s = m->starts = m->failed_starts = m->loop_recent_us = m->loop_max_us = UINT32_MAX;
    m->temp_ok = m->link = m->mqtt = m->mqtt_connected = m->clock_synced = true;
    m->temp_dc = -400;
    m->eth_rx = m->eth_tx = m->eth_rx_err = m->eth_tx_drop = m->handshakes = m->handshakes_failed = m->aes_hw = m->aes_sw = UINT32_MAX;
    static const char *const NAMES[PORTS] = { "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"", "\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\", "TigoBus", "" };
    for (int i = 0; i < PORTS; i++) {
        m->port[i].name = NAMES[i];
        m->port[i].mode = modbus ? "modbus" : "server";
        m->port[i].up = m->port[i].modbus = modbus;
        m->port[i].clients = 4;
        m->port[i].line_rx = m->port[i].line_tx = m->port[i].net_rx = m->port[i].net_tx = m->port[i].turned_away = UINT32_MAX;
        for (int k = 0; k < ERR_KINDS; k++) m->port[i].err[k] = UINT32_MAX;
        m->port[i].heard_ms = INT32_MAX;
        for (int b = 0; b <= FWD_BUCKETS; b++) m->port[i].fwd.n[b] = UINT32_MAX / (FWD_BUCKETS + 1);
        m->port[i].fwd.sum_us = UINT64_MAX;
        memset(&m->port[i].mb, 0xFF, sizeof m->port[i].mb);
        for (int b = 0; b <= MB_HIST; b++) m->port[i].mb.hist[b] = UINT32_MAX / (MB_HIST + 1);
        m->port[i].mb_waiting = MB_QUEUE;
    }
}

int main(void)
{
    static metrics_t m;
    widest(&m, false);
    int plain = write_all(&m, PART_MAX);
    printf("plain ports: %d parts, %zu bytes\n", plain, alln);
    check(all, 0);
    assert(!strstr(all, "n540_modbus_"));
    assert(strstr(all, "n540_port_info{port=\"1\",name=\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\\\"\",mode=\"server\"} 1\n"));
    assert(strstr(all, "n540_temperature_celsius -40.0\n"));
    assert(strstr(all, "n540_port_forward_seconds_bucket{port=\"1\",le=\"0.001000\"} 536870911\n"));
    assert(strstr(all, "n540_port_forward_seconds_bucket{port=\"4\",le=\"+Inf\"} 4294967288\n"));
    assert(strstr(all, "n540_port_forward_seconds_sum{port=\"1\"} 18446744073709.551615\n"));

    widest(&m, true);
    int mb = write_all(&m, PART_MAX);
    printf("Modbus ports: %d parts, %zu bytes\n", mb, alln);
    check(all, 4);
    assert(strstr(all, "n540_modbus_reply_seconds_bucket{port=\"2\",le=\"0.010\"} 477218588\n"));
    assert(strstr(all, "n540_modbus_waiting{port=\"3\"} 8\n"));
    assert(!strstr(all, "n540_port_forward_seconds_bucket"));   /* not for Modbus ports */

    /* Too small: -2, never a part cut short. */
    static char b[2000];
    assert(metrics_part(&m, 18, b, 200) == -2 && metrics_part(&m, 0, b, 0) == -2);
    /* A part's length at every size up to it: -2 below, whole from it. */
    int full = metrics_part(&m, 13, b, sizeof b);
    assert(full > 0);
    for (int k = 0; k <= full; k++) assert(metrics_part(&m, 13, b, (size_t)k + 1) == (k == full ? full : -2));   /* and its NUL */
    puts("ok");
    return 0;
}
