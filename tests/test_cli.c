/* Host tests + fuzzing for the console. Build with -DD2_BSS= and sanitizers. */
#include "cli.h"
#include "traffic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cfg_t C;
static uint8_t sent[256];
static size_t sent_n;
static int sent_port, restarts, wires, keeps;
const cfg_t *cli_cfg(void) { return &C; }
bool cli_change(char *l, char *e, size_t n, bool *p)
{
    cfg_t t = C;
    if (!cfg_parse(&t, l, e, n) || !cfg_validate(&t, e, n)) return false;
    *p = memcmp(&t.net, &C.net, sizeof t.net) != 0;
    C = t;
    return true;
}
bool cli_keep(char *e, size_t n) { (void)e; (void)n; keeps++; return true; }
void cli_port(int i, cli_port_t *p) { memset(p, 0, sizeof *p); p->state = C.port[i].enabled ? "listening" : "off"; p->serial_rx = 4294967295u; }
void cli_sys(cli_sys_t *s)
{
    memset(s, 0, sizeof *s);
    s->version = "test"; s->host = "n540-000000"; s->mqtt_state = "off"; s->mqtt_error = "";
    strcpy(s->ip, "10.0.0.1"); s->uptime_s = 90061; s->now_ms = 5000; s->net_pending_s = -1; s->reset_log[0] = 0xfe0000;
}
bool cli_send(int p, const uint8_t *d, size_t n, char *e, size_t el)
{
    if (!C.port[p].enabled) { snprintf(e, el, "port %d is off", p + 1); return false; }
    sent_port = p; sent_n = n; memcpy(sent, d, n);
    return true;
}
void cli_wiretest(char *o, size_t m) { wires++; snprintf(o, m, "wiretest ran\n"); }
void cli_restart(void) { restarts++; }

static char out[CLI_OUT];
static const char *run(const char *s)
{
    char *b = malloc(strlen(s) + 1);           /* exact size, so ASan sees overreads */
    strcpy(b, s);
    cli_run(b, out, sizeof out);
    free(b);
    assert(strlen(out) < sizeof out);
    return out;
}
#define HAS(cmd, want) do { if (!strstr(run(cmd), want)) { printf("FAIL %s\nwanted %s\ngot: %s\n", cmd, want, out); return 1; } } while (0)
#define SENT(p, str, n) assert(sent_port == p && sent_n == n && !memcmp(sent, str, n))

int main(int argc, char **argv)
{
    cfg_defaults(&C);
    traffic_init();
    HAS("help", "wiretest"); HAS("  status ", "1d 1h"); HAS("status", "power on"); HAS("ports", "TCP server 4001");
    HAS("port 1", "Port 1"); HAS("port", "usage"); HAS("port 5", "usage"); HAS("port 12", "usage"); HAS("port 0", "usage");
    HAS("nope", "unknown command"); assert(!*run("")); assert(!*run("  \t "));
    HAS("set port1.baud=19200 port1.parity=even", "ok"); assert(C.port[0].baud == 19200 && C.port[0].parity == PARITY_EVEN);
    HAS("set port1.baud 38400", "ok"); assert(C.port[0].baud == 38400);
    HAS("set \"device.name=Pump house\"", "ok"); assert(!strcmp(C.device_name, "Pump house"));
    HAS("set device.name \"a \\\"b\\\" c\"", "ok"); assert(!strcmp(C.device_name, "a \"b\" c"));
    HAS("set port1.remote_host \"\"", "ok");
    HAS("set port1.baud=5", "error"); HAS("set", "usage"); HAS("set x", "expected key=value"); HAS("set \"oops", "unbalanced");
    cfg_t before = C;
    HAS("set port1.baud=9600 port2.baud=1", "error"); assert(!memcmp(&before, &C, sizeof C));
    HAS("set net.mode=static net.ip=10.0.0.9 net.mask=255.255.255.0", "on trial"); HAS("keep", "ok"); assert(keeps == 1);
    HAS("get port1.baud", "port1.baud=38400\n"); HAS("get net", "net.ip=10.0.0.9"); HAS("get zzz", "error"); HAS("get", "port4.listen_only=0");
    strcpy(C.mqtt.password, "hunter2"); strcpy(C.mqtt.user, "u");
    assert(!strstr(run("get"), "hunter2") && !strstr(run("get"), "web_"));
    /* `get` output must load back to the same settings. */
    { static char b[2600]; char e[96]; cfg_t t; cfg_defaults(&t); assert(cfg_lines(&C, b, sizeof b)); assert(cfg_parse(&t, b, e, sizeof e));
      memcpy(t.mqtt.password, C.mqtt.password, 64); assert(!memcmp(&t, &C, sizeof t)); }
    for (size_t k = 0; k < 2000; k++) { char *b = malloc(k + 1); size_t r = cfg_lines(&C, b, k); assert(r < k + 1); free(b); if (r) break; }

    HAS("send 1 hi", "port 1 is off"); HAS("enable 1", "ok"); assert(C.port[0].enabled); HAS("enable", "usage"); HAS("enable 9", "usage");
    HAS("send 1 hello", "ok, 5 bytes"); SENT(0, "hello", 5);
    HAS("send 1 \"a b\\r\\n\"", "ok, 5"); SENT(0, "a b\r\n", 5);
    HAS("send 1 \\x00\\xff\\0\\\\", "ok, 4"); SENT(0, "\0\xff\0\\", 4);
    HAS("send 1 \"q\\\\n\"", "ok, 3"); SENT(0, "q\\n", 3);
    HAS("send 1 hex 01 03 00FF", "ok, 4"); SENT(0, "\1\3\0\xff", 4);
    HAS("send 1 hex 0", "error"); HAS("send 1 hex zz", "error"); HAS("send 1 hex", "error"); HAS("send 1 \"\"", "error");
    HAS("send 1 \\x1", "error"); HAS("send 1 \\q", "error"); HAS("send 1 a b", "double quotes"); HAS("send 1 tail\\", "error");
    { char b[700]; strcpy(b, "send 1 "); memset(b + 7, 'x', 256); b[263] = 0; HAS(b, "ok, 256"); b[263] = 'x'; b[264] = 0; HAS(b, "error"); }
    HAS("wiretest", "Disconnect"); HAS("wiretest yes", "port 1 is on"); HAS("disable 1", "ok"); assert(!wires);
    HAS("wiretest yes", "wiretest ran"); assert(wires == 1); HAS("wiretest no", "Disconnect");
    HAS("restart", "restart yes"); assert(!restarts); HAS("restart yes", "restarting"); assert(restarts == 1); HAS("resets", "power on");

    HAS("log 1", "nothing seen"); HAS("log", "usage"); HAS("log 1 x", "usage"); HAS("log 1 clear x", "usage");
    uint8_t d[200]; memset(d, 0x41, sizeof d);
    traffic_add(0, true, d, 3, 1000); traffic_add(0, false, d, 200, 4500);
    HAS("log 1", "from line  200"); HAS("log 1", "4.000s ago  to line      3  41 41 41  |AAA|");
    for (int k = 0; k < 100; k++) traffic_add(0, k & 1, d, 48, (uint32_t)k);
    run("log 1"); assert(strstr(out, "|\n") && !strstr(out, "cut short"));
    HAS("log 1 clear", "ok"); HAS("log 1", "nothing seen");
    for (int i = 0; i < PORTS; i++) { memset(C.port[i].name, '%', 23); memset(C.port[i].remote_host, 'h', 47); C.port[i].mode = MODE_UDP; C.port[i].remote_port = 65535; }
    memset(C.device_name, 's', 31); run("ports"); assert(strstr(out, "%%%%")); run("get"); assert(!strstr(out, "cut short"));
    { char tiny[64]; char c[] = "help"; cli_run(c, tiny, sizeof tiny); assert(strstr(tiny, "cut short") && strlen(tiny) < sizeof tiny); }

    /* Fuzz: random bytes, and random splices of real words. */
    long N = argc > 1 ? atol(argv[1]) : 300000;
    static const char *W[] = { "help","status","ports","port","get","set","enable","disable","keep","send","log","resets","wiretest","yes","1","2","4","hex","clear",
        "port1.baud=9600","port1.name=","\"","\\","\\x","\\x4","=","%s%n","net.ip=1.2.3.4","00","ff","device.name=\"a","\t","\n","\r","\"\"","\xff\xfe", "port1.", "=x" };
    srand(1);
    for (long k = 0; k < N; k++) {
        char b[600]; size_t n = 0; int words = rand() % 30;
        if (k % 3 == 0) { n = (size_t)(rand() % 300); for (size_t j = 0; j < n; j++) b[j] = (char)(1 + rand() % 255); b[n] = 0; }
        else { b[0] = 0; for (int j = 0; j < words && n < 560; j++) n += (size_t)snprintf(b + n, sizeof b - n, "%s%s", W[(size_t)rand() % (sizeof W / sizeof *W)], rand() % 4 ? " " : ""); }
        run(b);
        char e[96]; assert(cfg_validate(&C, e, sizeof e));      /* never left invalid */
    }
    printf("ok, %ld fuzz lines\n", N);
    (void)argv;
    return 0;
}
