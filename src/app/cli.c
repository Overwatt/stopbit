#include "cli.h"
#include "traffic.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_ARGS 24

typedef struct { char *buf; size_t max, len; bool full; } out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    if (o->full) return;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->max - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->max - o->len) {
        o->full = true;
        o->buf[o->len] = 0;                 /* drop the partial line, not the rest */
        return;
    }
    o->len += (size_t)n;
}

/* Splits on spaces; "double quotes" keep them, backslash escapes \" and \\.
 * Other backslashes pass through for `send`. Returns -1 if malformed. */
static int split(char *s, char **argv)
{
    int n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s) return n;
        if (n == MAX_ARGS) return -1;
        char *w = argv[n++] = s;
        bool q = false;
        while (*s && (q || (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'))) {
            if (*s == '"') { q = !q; s++; continue; }
            if (*s == '\\' && (s[1] == '"' || (q && s[1] == '\\'))) {
                if (s[1] == '\\') { *w++ = *s++; }   /* keep both for `send` */
                else s++;
            }
            *w++ = *s++;
        }
        if (q) return -1;
        if (*s) s++;
        *w = 0;
    }
}

static int port_arg(const char *s)
{
    return s && s[0] >= '1' && s[0] < '1' + PORTS && !s[1] ? s[0] - '1' : -1;
}

static void ago(uint32_t s, char *b, size_t n)
{
    if (s < 60) snprintf(b, n, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(b, n, "%lum %lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else if (s < 86400) snprintf(b, n, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else snprintf(b, n, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
}

static const char *reset_name(uint32_t r)
{
    return r & 0x800000u ? "power on" : r & 0x1000000u ? "restart" :
           r & 0x400000u ? "watchdog or reset pin" : r ? "other" : "-";
}

static void line_fmt(const port_cfg_t *c, char *b, size_t n)
{
    snprintf(b, n, "%lu %u%c%u", (unsigned long)c->baud, c->data_bits, "NEO"[c->parity % 3], c->stop_bits);
}

static void net_fmt(const port_cfg_t *c, char *b, size_t n)
{
    if (c->mode == MODE_CLIENT) snprintf(b, n, "TCP to %s:%u", c->remote_host[0] ? c->remote_host : "?", c->remote_port);
    else if (c->mode == MODE_UDP && c->remote_host[0])
        snprintf(b, n, "UDP %u to %s:%u", c->local_port, c->remote_host, c->remote_port);
    else snprintf(b, n, "%s %u", c->mode == MODE_UDP ? "UDP" : "TCP server", c->local_port);
}

static uint32_t errors(const cli_port_t *p)
{
    return p->net_lost + p->rx_lost + p->parity + p->framing + p->noise + p->overrun + p->refused;
}

/* ---- commands ------------------------------------------------------------ */
static void c_help(out_t *o, int argc, char **argv);

static void c_status(out_t *o, int argc, char **argv)
{
    cli_sys_t s;
    char up[24];
    (void)argc; (void)argv;
    cli_sys(&s);
    ago(s.uptime_s, up, sizeof up);
    put(o, "%s (%s)\n", cli_cfg()->device_name, s.host);
    put(o, "firmware  %s, slot %c\n", s.version, s.slot == 0 ? 'A' : s.slot == 1 ? 'B' : '-');
    put(o, "up        %s, %lu boots, last start: %s\n", up, (unsigned long)s.boots, reset_name(s.reset_log[0]));
    put(o, "address   %s / %s, gateway %s (%s)\n", s.ip, s.mask, s.gateway, cli_cfg()->net.dhcp ? "DHCP" : "static");
    put(o, "ethernet  %s, %d Mb/s %s duplex\n", s.mac, s.speed, s.full_duplex ? "full" : "half");
    put(o, "mqtt      %s%s%s\n", s.mqtt_state, s.mqtt_error[0] ? ": " : "", s.mqtt_error);
    if (s.net_pending_s >= 0)
        put(o, "NETWORK SETTINGS ON TRIAL: revert in %d s unless you run: keep\n", s.net_pending_s);
}

static void c_ports(out_t *o, int argc, char **argv)
{
    (void)argc; (void)argv;
    put(o, "#  %-23s %-11s %-13s %-28s %10s %10s %6s\n", "name", "status", "line", "network", "from line", "to line", "errors");
    for (int i = 0; i < PORTS; i++) {
        const port_cfg_t *c = &cli_cfg()->port[i];
        cli_port_t p;
        char l[24], n[80];
        cli_port(i, &p);
        line_fmt(c, l, sizeof l);
        net_fmt(c, n, sizeof n);
        put(o, "%d  %-23s %-11s %-13s %-28s %10lu %10lu %6lu\n", i + 1, c->name, p.state, l, n,
            (unsigned long)p.serial_rx, (unsigned long)p.serial_tx, (unsigned long)errors(&p));
    }
}

static void c_port(out_t *o, int argc, char **argv)
{
    int i = argc == 2 ? port_arg(argv[1]) : -1;
    if (i < 0) { put(o, "error: usage: port <1-4>\n"); return; }
    const port_cfg_t *c = &cli_cfg()->port[i];
    cli_port_t p;
    char l[24], n[80];
    cli_port(i, &p);
    line_fmt(c, l, sizeof l);
    net_fmt(c, n, sizeof n);
    put(o, "port %d    %s\nstatus    %s, %d connected%s%s\nline      %s%s%s\nnetwork   %s\n", i + 1, c->name, p.state,
        p.clients, p.peers[0] ? ": " : "", p.peers, l, c->loopback ? ", LOOPBACK TEST ON" : "",
        c->listen_only ? ", listen only" : c->echo_cancel ? ", ignoring own echo" : "", n);
    put(o, "bytes     %lu from line, %lu to line\n", (unsigned long)p.serial_rx, (unsigned long)p.serial_tx);
    put(o, "errors    %lu parity, %lu framing, %lu noise, %lu overrun, %lu buffer full, %lu not delivered, %lu refused\n",
        (unsigned long)p.parity, (unsigned long)p.framing, (unsigned long)p.noise, (unsigned long)p.overrun,
        (unsigned long)p.rx_lost, (unsigned long)p.net_lost, (unsigned long)p.refused);
}

static void c_get(out_t *o, int argc, char **argv)
{
    static char b[2600];
    size_t n = cfg_lines(cli_cfg(), b, sizeof b), shown = 0;
    if (!n) { put(o, "error: settings too long to show\n"); return; }
    const char *want = argc > 1 ? argv[1] : "";
    for (char *l = b, *e; *l; l = e + 1) {
        e = strchr(l, '\n');
        *e = 0;
        if (!strncmp(l, want, strlen(want))) { put(o, "%s\n", l); shown++; }
    }
    if (!shown) put(o, "error: no setting starts with %s\n", want);
}

static void apply(out_t *o, char *lines)
{
    char err[96];
    bool pending = false;
    if (!cli_change(lines, err, sizeof err, &pending)) { put(o, "error: %s\n", err); return; }
    put(o, pending ? "ok. Network settings are on trial: run keep within 2 minutes or they revert.\n" : "ok\n");
}

static void c_set(out_t *o, int argc, char **argv)
{
    char b[1024];
    size_t n = 0;
    if (argc < 2) { put(o, "error: usage: set <key>=<value> ...   or   set <key> <value>\n"); return; }
    if (argc == 3 && !strchr(argv[1], '=')) {
        n = (size_t)snprintf(b, sizeof b, "%s=%s\n", argv[1], argv[2]);
    } else {
        for (int i = 1; i < argc; i++) {
            if (!strchr(argv[i], '=')) { put(o, "error: expected key=value: %s\n", argv[i]); return; }
            int w = snprintf(b + n, sizeof b - n, "%s\n", argv[i]);
            if (w < 0 || (size_t)w >= sizeof b - n) { n = sizeof b; break; }
            n += (size_t)w;
        }
    }
    if (n >= sizeof b) { put(o, "error: too many settings at once\n"); return; }
    apply(o, b);
}

static void onoff(out_t *o, int argc, char **argv, int on)
{
    char b[32];
    int i = argc == 2 ? port_arg(argv[1]) : -1;
    if (i < 0) { put(o, "error: usage: %s <1-4>\n", argv[0]); return; }
    snprintf(b, sizeof b, "port%d.enabled=%d\n", i + 1, on);
    apply(o, b);
}
static void c_enable(out_t *o, int argc, char **argv) { onoff(o, argc, argv, 1); }
static void c_disable(out_t *o, int argc, char **argv) { onoff(o, argc, argv, 0); }

static void c_keep(out_t *o, int argc, char **argv)
{
    char err[96];
    (void)argc; (void)argv;
    if (cli_keep(err, sizeof err)) put(o, "ok\n");
    else put(o, "error: %s\n", err);
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* \r \n \t \\ \0 and \xNN. Returns the length, or -1 if malformed. */
static int unescape(const char *s, uint8_t *d, size_t max)
{
    size_t n = 0;
    while (*s) {
        uint8_t v = (uint8_t)*s++;
        if (v == '\\') {
            char c = *s;
            if (c) s++;
            if (c == 'r') v = '\r';
            else if (c == 'n') v = '\n';
            else if (c == 't') v = '\t';
            else if (c == '0') v = 0;
            else if (c == '\\') v = '\\';
            else if (c == 'x') {
                int a = hexv(s[0]), b = a < 0 ? -1 : hexv(s[1]);
                if (b < 0) return -1;
                v = (uint8_t)(a << 4 | b);
                s += 2;
            } else return -1;
        }
        if (n == max) return -1;
        d[n++] = v;
    }
    return (int)n;
}

static void c_send(out_t *o, int argc, char **argv)
{
    uint8_t d[256];
    char err[96];
    int i = argc >= 3 ? port_arg(argv[1]) : -1;
    if (i < 0) { put(o, "error: usage: send <1-4> \"text\\r\\n\"   or   send <1-4> hex 01 03 00 ff\n"); return; }
    int n = 0;
    if (!strcmp(argv[2], "hex")) {
        for (int k = 3; k < argc && n >= 0; k++)
            for (const char *s = argv[k]; *s && n >= 0; s += 2) {
                int a = hexv(s[0]), b = a < 0 ? -1 : hexv(s[1]);
                if (b < 0 || n == (int)sizeof d) { n = -1; break; }
                d[n++] = (uint8_t)(a << 4 | b);
            }
    } else if (argc == 3) {
        n = unescape(argv[2], d, sizeof d);
    } else {
        put(o, "error: put text with spaces in double quotes\n");
        return;
    }
    if (n <= 0) { put(o, "error: nothing sendable there (up to 256 bytes)\n"); return; }
    if (!cli_send(i, d, (size_t)n, err, sizeof err)) { put(o, "error: %s\n", err); return; }
    put(o, "ok, %d byte%s\n", n, n == 1 ? "" : "s");
}

static void c_log(out_t *o, int argc, char **argv)
{
    int i = argc >= 2 ? port_arg(argv[1]) : -1;
    if (i < 0 || argc > 3) { put(o, "error: usage: log <1-4> [clear]\n"); return; }
    if (argc == 3) {
        if (strcmp(argv[2], "clear")) { put(o, "error: usage: log <1-4> [clear]\n"); return; }
        traffic_clear(i);
        put(o, "ok\n");
        return;
    }
    cli_sys_t s;
    traffic_t t;
    cli_sys(&s);
    int n = 0;
    while (traffic_get(i, n, &t)) n++;
    if (!n) { put(o, "nothing seen on port %d yet\n", i + 1); return; }
    if (n > 16) n = 16;                     /* what fits; newest at the bottom */
    for (int k = n - 1; k >= 0; k--) {
        traffic_get(i, k, &t);
        uint32_t d = s.now_ms - t.ms;
        size_t m = t.len < 24 ? t.len : 24;
        put(o, "%7lu.%03lus ago  %s %4u  ", (unsigned long)(d / 1000), (unsigned long)(d % 1000), t.tx ? "to line  " : "from line", t.len);
        for (size_t j = 0; j < m; j++) put(o, "%02x ", t.data[j]);
        put(o, "%s |", t.len > m ? ".." : "");
        for (size_t j = 0; j < m; j++) put(o, "%c", t.data[j] >= 0x20 && t.data[j] < 0x7f ? t.data[j] : '.');
        put(o, "|\n");
    }
}

static void c_wiretest(out_t *o, int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes")) {
        put(o, "Sends a short pattern on every port at 9600 8N1 and reports which ports hear it.\n"
               "Disconnect any equipment first. All ports must be off. To go ahead: wiretest yes\n");
        return;
    }
    for (int i = 0; i < PORTS; i++)
        if (cli_cfg()->port[i].enabled) { put(o, "error: port %d is on; turn every port off first\n", i + 1); return; }
    for (int i = 0; i < PORTS; i++)
        if (cli_cfg()->port[i].listen_only) { put(o, "error: port %d is listen only, and the test transmits\n", i + 1); return; }
    cli_wiretest(o->buf + o->len, o->max - o->len);
    o->len += strlen(o->buf + o->len);
}

static void c_resets(out_t *o, int argc, char **argv)
{
    cli_sys_t s;
    (void)argc; (void)argv;
    cli_sys(&s);
    put(o, "%lu boots, %lu without 30 s of uptime since\nnewest first:\n", (unsigned long)s.boots, (unsigned long)s.crashes);
    for (int i = 0; i < 6; i++) put(o, "  %08lx  %s\n", (unsigned long)s.reset_log[i], reset_name(s.reset_log[i]));
}

static void c_restart(out_t *o, int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes")) { put(o, "To restart the gateway: restart yes\n"); return; }
    cli_restart();
    put(o, "restarting\n");
}

static const struct { const char *name, *usage, *help; void (*fn)(out_t *, int, char **); } CMD[] = {
    { "help",     "",                 "this list", c_help },
    { "status",   "",                 "device, network and MQTT", c_status },
    { "ports",    "",                 "all ports at a glance", c_ports },
    { "port",     "<1-4>",            "one port in detail", c_port },
    { "get",      "[prefix]",         "settings, e.g. get port1", c_get },
    { "set",      "<key>=<value> ...", "change settings; all apply or none do", c_set },
    { "enable",   "<1-4>",            "turn a port on", c_enable },
    { "disable",  "<1-4>",            "turn a port off", c_disable },
    { "keep",     "",                 "keep network settings that are on trial", c_keep },
    { "send",     "<1-4> \"text\\r\"",  "send on the line; or: send <1-4> hex 01 03", c_send },
    { "log",      "<1-4> [clear]",    "recent traffic on a port", c_log },
    { "wiretest", "",                 "which ports hear which (see its warning)", c_wiretest },
    { "resets",   "",                 "why it last restarted", c_resets },
    { "restart",  "",                 "restart the gateway", c_restart },
};
#define NCMD (sizeof CMD / sizeof CMD[0])

static void c_help(out_t *o, int argc, char **argv)
{
    (void)argc; (void)argv;
    for (size_t i = 0; i < NCMD; i++) put(o, "%-9s %-18s %s\n", CMD[i].name, CMD[i].usage, CMD[i].help);
}

void cli_run(char *line, char *out, size_t max)
{
    static const char MORE[] = "... (cut short)\n";
    char *argv[MAX_ARGS];
    out_t o = { out, max - sizeof MORE, 0, false };
    out[0] = 0;
    int argc = split(line, argv);
    if (argc < 0) { put(&o, "error: unbalanced quotes, or too many words\n"); return; }
    if (argc == 0) return;
    for (size_t i = 0; i < NCMD; i++) {
        if (strcmp(CMD[i].name, argv[0])) continue;
        CMD[i].fn(&o, argc, argv);
        if (o.full) memcpy(out + o.len, MORE, sizeof MORE);
        return;
    }
    put(&o, "error: unknown command %.20s; try help\n", argv[0]);
}
