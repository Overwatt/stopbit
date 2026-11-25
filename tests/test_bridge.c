/* cc -w -DD2= -DD2_BSS= -Itests/stub_net -Itests/stub -Isrc/app tests/test_bridge.c \
 *    src/app/{bridge,cfg,acl,knock,traffic,history}.c -o /tmp/br && /tmp/br
 * The access list in every mode: who connects, who is heard, what reaches the line. */
#include "bridge.h"
#include "uart485.h"
#include "knock.h"
#include "stm32h7xx_hal.h"
#include "lwip/tcp.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t t_now = 1000; t_regs_t t_regs; t_rtc_t t_rtc; uint32_t SystemCoreClock;
void NVIC_SystemReset(void) { abort(); }

/* ---- serial side ---- */
volatile uart_stats_t g_uart[PORTS];
static uint8_t L[PORTS][65536]; static size_t Ln[PORTS];       /* what went out on the line */
static uint8_t R[PORTS][2000]; static size_t Rn[PORTS];
static int opened[PORTS];
void uart485_init(void) { }
void uart485_open(int p, const port_cfg_t *c) { (void)c; opened[p]++; }
void uart485_close(int p) { (void)p; }
size_t uart485_rx_avail(int p) { return Rn[p]; }
size_t uart485_read(int p, uint8_t *b, size_t n) { if (n > Rn[p]) n = Rn[p]; memcpy(b, R[p], n); Rn[p] = 0; return n; }
uint32_t uart485_rx_idle_us(int p) { (void)p; return 1000000; }
size_t uart485_tx_room(int p) { (void)p; return 4096; }
size_t uart485_write(int p, const uint8_t *b, size_t n) { memcpy(L[p] + Ln[p], b, n); Ln[p] += n; return n; }
bool uart485_tx_idle(int p) { (void)p; return true; }

/* ---- network side ---- */
static struct tcp_pcb T[256]; static struct udp_pcb U[4];
static struct { uint32_t ip; u16_t port; int n; } US[64]; static int USn;
static uint32_t dns_answer; static int connects;
struct tcp_pcb *tcp_new(void) { for (int i = 0; i < 256; i++) if (!T[i].used) { memset(&T[i], 0, sizeof T[i]); T[i].used = true; return &T[i]; } abort(); }
err_t tcp_bind(struct tcp_pcb *p, const void *a, u16_t port) { p->port = port; return ERR_OK; }
struct tcp_pcb *tcp_listen_with_backlog(struct tcp_pcb *p, u8_t n) { p->listening = true; return p; }
void tcp_accept(struct tcp_pcb *p, acc_fn f) { p->acc = f; }
void tcp_arg(struct tcp_pcb *p, void *a) { p->arg = a; }
void tcp_recv(struct tcp_pcb *p, rcv_fn f) { p->rx = f; }
void tcp_err(struct tcp_pcb *p, err_fn f) { p->err = f; }
u16_t tcp_sndbuf(struct tcp_pcb *p) { return 8000; }
err_t tcp_write(struct tcp_pcb *p, const void *d, u16_t n, u8_t f) { assert(p->used && !p->closed && !p->aborted); memcpy(p->out + p->out_n, d, n); p->out_n += n; return ERR_OK; }
err_t tcp_output(struct tcp_pcb *p) { return ERR_OK; }
void tcp_recved(struct tcp_pcb *p, u16_t n) { p->unacked -= n; assert(p->unacked >= 0); }
err_t tcp_close(struct tcp_pcb *p) { p->closed = true; return ERR_OK; }
void tcp_abort(struct tcp_pcb *p) { p->aborted = true; }
err_t tcp_connect(struct tcp_pcb *p, const ip_addr_t *ip, u16_t port, err_t (*f)(void *, struct tcp_pcb *, err_t))
{ p->remote_ip = *ip; p->remote_port = port; p->connecting = true; p->conn = f; connects++; return ERR_OK; }
struct pbuf *pbuf_alloc(int a, u16_t n, int b) { struct pbuf *q = calloc(1, sizeof *q); q->payload = malloc(n); q->len = q->tot_len = n; return q; }
void pbuf_take(struct pbuf *q, const void *d, u16_t n) { memcpy(q->payload, d, n); }
void pbuf_free(struct pbuf *q) { while (q) { struct pbuf *n = q->next; free(q->payload); free(q); q = n; } }
struct udp_pcb *udp_new(void) { for (int i = 0; i < 4; i++) if (!U[i].used) { U[i].used = true; return &U[i]; } abort(); }
err_t udp_bind(struct udp_pcb *u, const void *a, u16_t p) { return ERR_OK; }
void udp_remove(struct udp_pcb *u) { u->used = false; }
void udp_recv(struct udp_pcb *u, udp_fn f, void *a) { u->rx = f; u->arg = a; }
err_t udp_sendto(struct udp_pcb *u, struct pbuf *q, const ip_addr_t *ip, u16_t port) { US[USn].ip = ip->addr; US[USn].port = port; US[USn++].n = q->tot_len; return ERR_OK; }
err_t dns_gethostbyname(const char *n, ip_addr_t *ip, void (*f)(const char *, const ip_addr_t *, void *), void *a) { ip->addr = dns_answer; return ERR_OK; }
char *ipaddr_ntoa_r(const ip_addr_t *ip, char *b, int n) { inet_ntop(AF_INET, &ip->addr, b, (socklen_t)n); return b; }

/* ---- helpers ---- */
static cfg_t C;
static uint32_t ip(const char *s) { return inet_addr(s); }
static void set(const char *t)
{ char b[512], e[96]; strcpy(b, t); if (!cfg_parse(&C, b, e, sizeof e) || !cfg_validate(&C, e, sizeof e)) { puts(e); abort(); } bridge_configure(&C); bridge_poll(); }
static struct tcp_pcb *listener(u16_t port) { for (int i = 0; i < 256; i++) if (T[i].used && T[i].listening && !T[i].closed && T[i].port == port) return &T[i]; return NULL; }
static struct tcp_pcb *from(u16_t port, const char *src)
{
    static u16_t n = 50000;
    struct tcp_pcb *l = listener(port), *p = tcp_new();
    p->remote_ip.addr = ip(src); p->remote_port = n++;
    err_t e = l->acc(l->arg, p, ERR_OK);
    assert((e == ERR_ABRT) == p->aborted);
    return p;
}
static void say(struct tcp_pcb *p, const char *s)
{
    struct pbuf *q = pbuf_alloc(0, (u16_t)strlen(s), 0);
    memcpy(q->payload, s, strlen(s)); p->unacked += (int)strlen(s);
    assert(p->rx(p->arg, p, q, ERR_OK) == ERR_OK);
    bridge_poll();
}
static void dgram(struct udp_pcb *u, const char *src, u16_t port, const char *s)
{
    struct pbuf *q = pbuf_alloc(0, (u16_t)strlen(s), 0);
    ip_addr_t a = { ip(src) };
    memcpy(q->payload, s, strlen(s));
    u->rx(u->arg, u, q, &a, port);
    bridge_poll();
}
static void heard(int p, const char *s) { strcpy((char *)R[p], s); Rn[p] = strlen(s); bridge_poll(); }
static bool line(int p, const char *s) { bool r = Ln[p] == strlen(s) && !memcmp(L[p], s, Ln[p]); if (!r) printf("line %d: %.*s\n", p + 1, (int)Ln[p], L[p]); Ln[p] = 0; return r; }
static bridge_info_t info(int p) { bridge_info_t b; bridge_info(p, &b); return b; }
static struct udp_pcb *udp(void) { for (int i = 0; i < 4; i++) if (U[i].used) return &U[i]; return NULL; }

int main(void)
{
    char j[4344];
    cfg_defaults(&C);
    bridge_init();

    /* ---- TCP server ---- */
    set("port1.enabled=1");
    struct tcp_pcb *a = from(4001, "10.0.0.5"), *b = from(4001, "10.0.0.6");
    say(a, "AA"); say(b, "BB");
    assert(line(0, "AABB"));                                            /* empty list: anyone, as before */
    int was = opened[0];

    /* a list appears while both are connected */
    set("port1.access=10.0.0.5 read");
    assert(opened[0] == was && listener(4001));                         /* the port was not restarted */
    assert(!a->closed && b->closed && info(0).denied == 1 && !strcmp(info(0).denied_ip, "10.0.0.6"));
    say(a, "nope");
    assert(line(0, "") && a->unacked == 0 && info(0).ignored == 4);     /* taken, dropped, not held up */
    heard(0, "bus data");
    assert(a->out_n == 8);                                              /* still hears the bus */
    for (int i = 0; i < 50; i++) say(a, "a long run of bytes that must never reach the line or stall it");
    assert(line(0, "") && a->unacked == 0 && !a->closed);

    struct tcp_pcb *x = from(4001, "10.0.0.7");
    assert(x->aborted && info(0).denied == 2 && info(0).clients == 1);
    assert(info(0).errors == 0 && info(0).refused == 0);                /* not an error */
    assert(bridge_json(j, sizeof j) && strstr(j, "\"denied\":2,\"denied_ip\":\"10.0.0.7\"") && strstr(j, "\"read_only\":1"));

    /* promoted and demoted without reconnecting */
    set("port1.access=10.0.0.5 write");
    say(a, "go"); assert(line(0, "go") && !a->closed);
    set("port1.access=10.0.0.5 read");
    say(a, "stop"); assert(line(0, ""));
    /* listen-only wins over write */
    set("port1.access=10.0.0.5 write\nport1.listen_only=1");
    a = from(4001, "10.0.0.5");
    say(a, "no"); assert(line(0, ""));
    set("port1.listen_only=0");
    /* subnet reads, one host writes */
    set("port1.access=10.0.0.0/24 read, 10.0.0.9 write");
    a = from(4001, "10.0.0.5"); b = from(4001, "10.0.0.9");
    say(a, "x"); say(b, "y"); assert(line(0, "y"));
    assert(bridge_json(j, sizeof j) && strstr(j, "\"read_only\":1"));
    assert(from(4001, "10.0.1.5")->aborted);
    /* refused ones do not use up the four places */
    for (int i = 0; i < 20; i++) assert(from(4001, "192.168.9.9")->aborted);
    assert(!from(4001, "10.0.0.6")->aborted && !from(4001, "10.0.0.7")->aborted && from(4001, "10.0.0.8")->aborted && info(0).refused == 1);
    /* a rename does not disturb anyone; another port's list does not matter here */
    set("port1.name=Bus\nport2.access=1.1.1.1 read");
    assert(!a->closed && !b->closed);
    /* the list goes away again */
    set("port1.access=");
    say(a, "z"); assert(line(0, "z"));
    /* bytes already buffered when a client loses write are dropped, not sent */
    set("port1.access=10.0.0.5 write, 10.0.0.9 write");
    { struct pbuf *q = pbuf_alloc(0, 3, 0); memcpy(q->payload, "old", 3); a->unacked += 3; a->rx(a->arg, a, q, ERR_OK); }
    set("port1.access=10.0.0.5 read");
    bridge_poll();
    assert(line(0, "") && b->closed);
    set("port1.enabled=0\nport1.access=");

    /* ---- TCP client: the far end must be listed ---- */
    dns_answer = ip("10.0.0.50");
    connects = 0;
    set("port3.mode=client\nport3.remote_host=far.lan\nport3.remote_port=502\nport3.enabled=1\nport3.access=10.0.0.9 write");
    assert(connects == 0 && info(2).denied == 1 && !strcmp(info(2).denied_ip, "10.0.0.50") && !strcmp(info(2).state, "retrying"));
    set("port3.access=10.0.0.50 read");
    t_now += 40000; bridge_poll();
    assert(connects == 1);
    struct tcp_pcb *f = NULL;
    for (int i = 0; i < 256; i++) if (T[i].connecting) f = &T[i];
    f->conn(f->arg, f, ERR_OK);
    say(f, "cmd"); assert(line(2, "") && info(2).ignored == 3);
    heard(2, "up"); assert(f->out_n == 2);
    set("port3.access=10.0.0.50 write");
    say(f, "cmd"); assert(line(2, "cmd"));
    set("port3.access=10.0.0.51 write");
    assert(f->closed && !strcmp(info(2).state, "retrying"));
    /* pointing it elsewhere, as MQTT could, does not get round the list */
    set("port3.access=10.0.0.50 write\nport3.remote_host=evil.lan");
    connects = 0; dns_answer = ip("6.6.6.6");
    t_now += 40000; set("port3.remote_host=evil2.lan");
    assert(connects == 0);
    set("port3.enabled=0");

    /* ---- UDP, no remote host: only listed senders are heard or answered ---- */
    set("port4.mode=udp\nport4.enabled=1\nport4.access=10.0.0.5 read, 10.0.0.9 write");
    struct udp_pcb *u = udp();
    dgram(u, "10.0.0.7", 9, "evil"); heard(3, "r");
    assert(line(3, "") && USn == 0 && info(3).denied == 1);
    dgram(u, "10.0.0.9", 99, "w"); heard(3, "r");
    assert(line(3, "w") && USn == 1 && US[0].ip == ip("10.0.0.9") && US[0].port == 99);
    dgram(u, "10.0.0.7", 9, "evil"); heard(3, "r");
    assert(line(3, "") && USn == 2 && US[1].ip == ip("10.0.0.9"));      /* cannot take over the answers */
    dgram(u, "10.0.0.5", 55, "hello"); heard(3, "r");
    assert(line(3, "") && USn == 3 && US[2].ip == ip("10.0.0.5") && info(3).ignored == 5);
    set("port4.access=10.0.0.9 write"); heard(3, "r");
    assert(USn == 3);                                                   /* the reader lost its place at once */
    /* with a remote host */
    dns_answer = ip("10.0.0.60");
    set("port4.remote_host=h.lan\nport4.remote_port=7\nport4.access=10.0.0.9 write");
    u = udp(); USn = 0; heard(3, "r");
    assert(USn == 0 && info(3).denied >= 1);
    set("port4.access=10.0.0.60 read"); t_now += 40000; bridge_poll(); heard(3, "r");
    assert(USn == 1 && US[0].ip == ip("10.0.0.60"));
    dgram(udp(), "10.0.0.60", 7, "w"); assert(line(3, ""));
    dgram(udp(), "10.0.0.9", 7, "w"); assert(line(3, ""));

    /* ---- who was turned away, and the alert ---- */
    cfg_defaults(&C);
    bridge_init();
    set("port1.enabled=1\nport1.access=10.0.0.5 read");
    a = from(4001, "10.0.0.5");
    for (int i = 0; i < 50; i++) { t_now += 1000; from(4001, "10.0.0.66"); }
    bridge_poll();
    assert(!info(0).alert[0]);                                          /* the usual client is there: no alarm */
    set("port1.access=10.0.0.6 read");                                  /* as if it had moved: it is dropped, */
    assert(a->closed);
    for (int i = 0; i < 2; i++) { t_now += 1000; from(4001, "10.0.0.5"); }  /* and keeps trying */
    bridge_poll();
    assert(!strcmp(info(0).alert, "turned_away"));
    assert(bridge_access_json(0, j, sizeof j) && strstr(j, "{\"denied\":53,\"turned_away\":[{\"ip\":\"10.0.0.5\",\"count\":3,\"ms\":0},{\"ip\":\"10.0.0.66\",\"count\":50,\"ms\":2000}]}"));
    assert(bridge_access_json(1, j, sizeof j) && !strcmp(j, "{\"denied\":0,\"turned_away\":[]}\n"));
    for (size_t k = 0; k < 60; k++) assert(!bridge_access_json(0, j, k));
    /* listing it clears the alarm and the entry; the other one is still news */
    set("port1.access=10.0.0.5 read");
    a = from(4001, "10.0.0.5");
    bridge_poll();
    assert(!info(0).alert[0] && !strcmp(info(0).denied_ip, "10.0.0.66") && info(0).denied == 53);
    /* a scan of one or two tries on a port with no client is not an alarm; three is */
    set("port2.enabled=1\nport2.access=10.0.0.5 read");
    from(4002, "9.9.9.9"); from(4002, "9.9.9.9"); from(4002, "9.9.9.8"); bridge_poll();
    assert(!info(1).alert[0]);
    from(4002, "9.9.9.9"); bridge_poll();
    assert(!strcmp(info(1).alert, "turned_away"));
    t_now += KNOCK_RECENT; bridge_poll();
    assert(!info(1).alert[0]);                                          /* and it passes */
    /* no list, no alarm; emptying the list clears what there was */
    for (int i = 0; i < 3; i++) from(4002, "9.9.9.9");
    bridge_poll(); assert(info(1).alert[0]);
    for (int k = 0; k < 12 * 24 * 60; k++) { t_now += 300000; bridge_poll(); if (k > 1) assert(!info(1).alert[0]); }   /* nor again when the clock wraps at 49 days */
    for (int i = 0; i < 3; i++) from(4002, "9.9.9.9");
    bridge_poll(); assert(info(1).alert[0]);
    set("port2.access="); assert(!info(1).alert[0] && !info(1).denied_ip[0]);
    /* a silent bus is said first */
    set("port2.access=10.0.0.5 read\nport2.quiet_min=1");
    t_now += 61000; for (int i = 0; i < 3; i++) from(4002, "9.9.9.9");
    bridge_poll(); assert(!strcmp(info(1).alert, "silent"));
    heard(1, "x"); assert(!strcmp(info(1).alert, "turned_away"));
    set("port2.enabled=0"); assert(!info(1).alert[0]);
    /* a TCP client whose far end is not listed; UDP with nobody to answer */
    dns_answer = ip("10.0.0.50");
    set("port3.mode=client\nport3.remote_host=far.lan\nport3.remote_port=502\nport3.enabled=1\nport3.access=10.0.0.9 write");
    for (int i = 0; i < 4; i++) { t_now += 31000; bridge_poll(); }
    assert(!strcmp(info(2).alert, "turned_away"));
    set("port4.mode=udp\nport4.enabled=1\nport4.access=10.0.0.9 write");
    for (int i = 0; i < 4; i++) dgram(udp(), "10.0.0.7", 9, "x");
    assert(!strcmp(info(3).alert, "turned_away"));
    dgram(udp(), "10.0.0.9", 9, "x");
    assert(!info(3).alert[0]);
    assert(bridge_json(j, sizeof j) && strstr(j, "\"denied_ip\":\"10.0.0.50\""));

    /* ---- carrying on while a save waits for flash ---- */
    cfg_defaults(&C);
    bridge_init();
    memset(Ln, 0, sizeof Ln);
    set("port1.enabled=1\nport1.access=10.0.0.5 write, 10.0.0.6 read");
    a = from(4001, "10.0.0.5"); b = from(4001, "10.0.0.6");
    a->out_n = b->out_n = 0;
    strcpy((char *)R[0], "bus"); Rn[0] = 3; bridge_pump();
    assert(a->out_n == 3 && b->out_n == 3);                             /* line to network */
    { struct pbuf *q = pbuf_alloc(0, 2, 0); memcpy(q->payload, "go", 2); a->unacked += 2; a->rx(a->arg, a, q, ERR_OK); }
    { struct pbuf *q = pbuf_alloc(0, 2, 0); memcpy(q->payload, "no", 2); b->unacked += 2; b->rx(b->arg, b, q, ERR_OK); }
    bridge_pump();
    assert(line(0, "go") && !a->unacked && !b->unacked);                /* network to line, the list still holding */
    a->rx(a->arg, a, NULL, ERR_OK); t_now += 5000;
    bridge_pump(); assert(!a->closed);                                  /* closing is left to bridge_poll */
    strcpy((char *)R[0], "x"); Rn[0] = 1; C.port[0].enabled = 0; bridge_configure(&C);
    bridge_pump(); assert(!strcmp(info(0).state, "listening"));         /* as are changed settings */
    bridge_poll(); assert(!strcmp(info(0).state, "off"));

    /* the widest state JSON still fits what /api/state leaves for it */
    cfg_defaults(&C); set("port1.enabled=1");
    for (int p = 0; p < PORTS; p++) { memset((void *)&g_uart[p], 0xFF, sizeof g_uart[p]); char t[64]; snprintf(t, sizeof t, "port%d.enabled=1", p + 1); set(t); for (int k = 0; k < 5; k++) from((u16_t)(4001 + p), "255.255.255.255"); }
    size_t n = bridge_json(j, sizeof j);
    printf("widest port state %zu bytes\n", n);
    assert(n && n < 4344 - 900);
    for (size_t k = 0; k < n; k++) assert(!bridge_json(j, k));
    puts("ok");
    return 0;
}
