/* cc -w -DD2= -DD2_BSS= -Itests/stub_net -Itests/stub -Isrc/app tests/test_bridge.c \
 *    src/app/{bridge,cfg,acl,knock,traffic,history}.c -o /tmp/br && /tmp/br
 * The access list in every mode: who connects, who is heard, what reaches the line. */
#include "bridge.h"
#include "uart485.h"
#include "knock.h"
#include "events.h"
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
static bool busy;                                               /* the line takes nothing */
size_t uart485_write(int p, const uint8_t *b, size_t n) { if (busy) return 0; memcpy(L[p] + Ln[p], b, n); Ln[p] += n; return n; }
bool uart485_tx_idle(int p) { (void)p; return true; }
#include "device.h"
static uint32_t dr_tag; static int dr_st = 9; static size_t dr_n;
void device_reply(int port, uint32_t tag, int st, const uint8_t *pdu, size_t n) { (void)port; (void)pdu; dr_tag = tag; dr_st = st; dr_n = n; }

/* ---- the event log: counted ---- */
static int ev_away, ev_alert;
static char ev_last_alert[16];
void events_add(ev_kind_t k, int port, uint32_t a, const char *s)
{
    (void)port; (void)a;
    if (k == EV_TURNED_AWAY) ev_away++;
    if (k == EV_ALERT) { ev_alert++; snprintf(ev_last_alert, sizeof ev_last_alert, "%s", s ? s : ""); }
}

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
u16_t tcp_sndbuf(struct tcp_pcb *p) { return p->full ? 0 : 8000; }
err_t tcp_write(struct tcp_pcb *p, const void *d, u16_t n, u8_t f) { assert(p->used && !p->closed && !p->aborted); memcpy(p->out + p->out_n, d, n); p->out_n += n; p->flags = f; return ERR_OK; }
err_t tcp_output(struct tcp_pcb *p) { p->outputs++; return ERR_OK; }
void tcp_recved(struct tcp_pcb *p, u16_t n) { p->owed -= n; assert(p->owed >= 0); }
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
    memcpy(q->payload, s, strlen(s)); p->owed += (int)strlen(s);
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
static bridge_info_t info(int p)                                /* and the short form agrees with it, each time */
{
    bridge_info_t b; bridge_brief_t q;
    bridge_info(p, &b); bridge_brief(p, &q);
    assert(q.state == b.state && q.alert == b.alert && q.clients == b.clients && q.serial_rx == b.serial_rx &&
           q.serial_tx == b.serial_tx && q.errors == b.errors && q.denied == b.denied);
    return b;
}
static err_t give(struct tcp_pcb *p, struct pbuf **q, const char *s)  /* *q: kept, as lwIP would, if refused */
{
    if (!*q) { *q = pbuf_alloc(0, (u16_t)strlen(s), 0); memcpy((*q)->payload, s, strlen(s)); p->owed += (int)strlen(s); }
    err_t e = p->rx(p->arg, p, *q, ERR_OK);
    if (e == ERR_OK) *q = NULL;
    return e;
}
/* Bytes as hex: "01 03 00 00". */
static size_t hex(const char *t, uint8_t *o) { size_t n = 0; unsigned v; int k; while (sscanf(t, " %2x%n", &v, &k) == 1) { o[n++] = (uint8_t)v; t += k; } return n; }
static void sayb(struct tcp_pcb *p, const char *h)
{
    uint8_t b[600]; size_t n = hex(h, b);
    struct pbuf *q = pbuf_alloc(0, (u16_t)n, 0);
    memcpy(q->payload, b, n); p->owed += (int)n;
    assert(p->rx(p->arg, p, q, ERR_OK) == ERR_OK);
    bridge_poll();
}
static void heardb(int p, const char *h) { Rn[p] = hex(h, R[p]); bridge_poll(); }
static bool same(const uint8_t *b, size_t n, const char *h)
{
    uint8_t w[600]; size_t wn = hex(h, w);
    if (n == wn && !memcmp(b, w, n)) return true;
    printf("got:  "); for (size_t i = 0; i < n; i++) printf("%02x ", b[i]); printf("\nwant: %s\n", h);
    return false;
}
static bool lineb(int p, const char *h) { bool r = same(L[p], Ln[p], h); Ln[p] = 0; return r; }
static bool outb(struct tcp_pcb *p, const char *h) { bool r = same(p->out, (size_t)p->out_n, h); p->out_n = 0; return r; }
static uint32_t rings(uint32_t *peak, uint32_t *waits) { uint32_t u, pk, n, w; bridge_rings(&u, &pk, &n, &w); if (peak) *peak = pk; if (waits) *waits = w; return u; }
static struct udp_pcb *udp(void) { for (int i = 0; i < 4; i++) if (U[i].used) return &U[i]; return NULL; }

int main(void)
{
    char j[4344];
    size_t n;
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
    assert(line(0, "") && a->owed == 0 && info(0).ignored == 4);     /* taken, dropped, not held up */
    uint32_t f0 = info(0).fwd.n[FWD_BUCKETS];
    heard(0, "bus data");
    assert(a->out_n == 8);                                              /* still hears the bus */
    assert(info(0).fwd.n[FWD_BUCKETS] == f0 + 1 && info(0).fwd.last_us == 1000000);   /* sent 1 s after (the fake line's quiet) */
    for (int i = 0; i < 50; i++) say(a, "a long run of bytes that must never reach the line or stall it");
    assert(line(0, "") && a->owed == 0 && !a->closed);

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
    { struct pbuf *q = pbuf_alloc(0, 3, 0); memcpy(q->payload, "old", 3); a->owed += 3; a->rx(a->arg, a, q, ERR_OK); }
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
    ev_away = ev_alert = 0;
    for (int i = 0; i < 50; i++) { t_now += 1000; from(4001, "10.0.0.66"); }
    bridge_poll();
    assert(ev_away == 1);                                               /* fifty tries, logged once */
    assert(!info(0).alert[0]);                                          /* the usual client is there: no alarm */
    set("port1.access=10.0.0.6 read");                                  /* as if it had moved: it is dropped, */
    assert(a->closed);
    for (int i = 0; i < 2; i++) { t_now += 1000; from(4001, "10.0.0.5"); }  /* and keeps trying */
    bridge_poll();
    assert(!strcmp(info(0).alert, "turned_away"));
    assert(ev_away == 2 && ev_alert == 1 && !strcmp(ev_last_alert, "turned_away"));   /* a new address; the alert, once */
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
    { struct pbuf *q = pbuf_alloc(0, 2, 0); memcpy(q->payload, "go", 2); a->owed += 2; a->rx(a->arg, a, q, ERR_OK); }
    { struct pbuf *q = pbuf_alloc(0, 2, 0); memcpy(q->payload, "no", 2); b->owed += 2; b->rx(b->arg, b, q, ERR_OK); }
    bridge_pump();
    assert(line(0, "go") && !a->owed && !b->owed);                /* network to line, the list still holding */
    a->rx(a->arg, a, NULL, ERR_OK); t_now += 5000;
    bridge_pump(); assert(!a->closed);                                  /* closing is left to bridge_poll */
    strcpy((char *)R[0], "x"); Rn[0] = 1; C.port[0].enabled = 0; bridge_configure(&C);
    bridge_pump(); assert(!strcmp(info(0).state, "listening"));         /* as are changed settings */
    bridge_poll(); assert(!strcmp(info(0).state, "off"));

    /* ---- the shared rings ---- */
    cfg_defaults(&C);
    bridge_init();
    memset(Ln, 0, sizeof Ln);
    set("port1.enabled=1\nport2.enabled=1\nport3.enabled=1\nport3.access=10.0.0.5 read\nport4.enabled=1\nport4.listen_only=1");
    struct tcp_pcb *w[7];
    for (int k = 0; k < 7; k++) w[k] = from(k < 4 ? 4001 : 4002, k < 4 ? "10.0.0.1" : "10.0.0.2");
    struct tcp_pcb *rd = from(4003, "10.0.0.5"), *lo = from(4004, "10.0.0.6");
    busy = true;
    struct pbuf *held[7] = { 0 };
    char msg[7][8];
    for (int k = 0; k < 6; k++) { snprintf(msg[k], 8, "w%d.", k); assert(give(w[k], &held[k], msg[k]) == ERR_OK); }
    uint32_t peak, waits;
    assert(rings(&peak, &waits) == 6 && peak == 6 && waits == 0);
    snprintf(msg[6], 8, "w6.");
    assert(give(w[6], &held[6], "w6.") == ERR_MEM && held[6] && rings(NULL, &waits) == 6 && waits == 1);  /* waits, kept by lwIP */
    assert(give(w[0], &held[0], "more.") == ERR_OK);                    /* one that has a ring keeps using it */
    for (int i = 0; i < 100; i++) {                                     /* a reader or a listen-only port never needs one */
        struct pbuf *q = NULL;
        assert(give(rd, &q, "chatter from a reader") == ERR_OK && give(lo, &q, "chatter, listen-only") == ERR_OK);
    }
    assert(rd->owed == 0 && lo->owed == 0 && rings(NULL, NULL) == 6);
    bridge_poll();
    assert(Ln[0] == 0 && Ln[1] == 0 && rings(NULL, NULL) == 6 && w[0]->owed == 8);   /* the line is busy: all held, nothing acknowledged */
    busy = false;
    bridge_poll();
    assert(line(0, "w0.more.w1.w2.w3.") && line(1, "w4.w5.") && rings(NULL, NULL) == 0);    /* drained, given back */
    for (int k = 0; k < 6; k++) assert(w[k]->owed == 0);
    assert(give(w[6], &held[6], "w6.") == ERR_OK && !held[6]);         /* offered again: there is room now */
    bridge_poll();
    assert(line(1, "w6.") && w[6]->owed == 0 && rings(&peak, NULL) == 0 && peak == 6);
    assert(line(2, "") && line(3, ""));
    /* a connection that goes, or is closed, with bytes still held gives its ring back */
    busy = true;
    for (int k = 0; k < 6; k++) { struct pbuf *q = NULL; assert(give(w[k], &q, "x") == ERR_OK); }
    assert(rings(NULL, NULL) == 6);
    w[0]->err(w[0]->arg, ERR_ABRT);                                     /* lost: lwIP freed it */
    assert(rings(NULL, NULL) == 5);
    set("port2.enabled=0");                                             /* w4 and w5 closed */
    assert(rings(NULL, NULL) == 3 && w[4]->closed && w[5]->closed);
    set("port1.access=10.0.0.9 write");                                 /* w1 to w3 turned away */
    assert(rings(NULL, NULL) == 0 && w[1]->closed && w[3]->closed);
    busy = false;
    bridge_poll();
    assert(line(0, "") && line(1, ""));                                 /* what they had is not sent */
    /* demoted with bytes held: dropped as they drain, and the ring is given back */
    set("port1.access=");
    struct tcp_pcb *d = from(4001, "10.0.0.1");
    busy = true;
    { struct pbuf *q = NULL; assert(give(d, &q, "held") == ERR_OK); }
    set("port1.access=10.0.0.1 read");
    { struct pbuf *q = NULL; assert(give(d, &q, "after") == ERR_OK); }   /* behind what is held, in order */
    busy = false;
    bridge_poll();
    assert(line(0, "") && d->owed == 0 && rings(NULL, NULL) == 0 && !d->closed);
    set("port1.enabled=0\nport3.enabled=0\nport4.enabled=0\nport1.access=");

    /* A Modbus gateway on port 2: requests one at a time, each reply to whoever asked. */
    cfg_defaults(&C);
    set("port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\nport2.access=10.0.0.1 write, 10.0.0.2 read");
    assert(listener(502) && opened[1]);
    struct tcp_pcb *mw = from(502, "10.0.0.1"), *mr = from(502, "10.0.0.2"), *mx = from(502, "10.0.0.3");
    assert(!mw->aborted && !mr->aborted && mx->aborted);
    sayb(mw, "00 01 00 00 00 06 01 03 00 00 00 01");
    assert(lineb(1, "01 03 00 00 00 01 84 0a") && mw->owed == 0);
    heardb(1, "01 03 02 00 2a 39 9b");
    assert(outb(mw, "00 01 00 00 00 05 01 03 02 00 2a"));
    sayb(mw, "00 0a 00 00 00 06 00 06 00 01 00 03");                    /* unit 0: no broadcasts here */
    assert(outb(mw, "00 0a 00 00 00 03 00 86 0a") && lineb(1, ""));
    sayb(mr, "00 02 00 00 00 06 01 06 00 01 00 03");                    /* a reader may not write */
    assert(outb(mr, "00 02 00 00 00 03 01 86 01") && lineb(1, ""));
    sayb(mw, "00 03 00 00 00 06 01 03 00 00 00 01");                    /* both at once: in turn */
    sayb(mr, "00 04 00 00 00 06 01 03 00 00 00 01");
    assert(lineb(1, "01 03 00 00 00 01 84 0a"));
    heardb(1, "01 03 02 00 2a 39 9b");
    assert(outb(mw, "00 03 00 00 00 05 01 03 02 00 2a") && mr->out_n == 0);
    bridge_poll();
    assert(lineb(1, "01 03 00 00 00 01 84 0a"));
    heardb(1, "01 03 02 00 2b f8 5b");
    assert(outb(mr, "00 04 00 00 00 05 01 03 02 00 2b") && mw->out_n == 0);
    set("port2.access=10.0.0.1 read, 10.0.0.2 read");                   /* demoted: writes refused now */
    sayb(mw, "00 05 00 00 00 06 01 06 00 01 00 03");
    assert(outb(mw, "00 05 00 00 00 03 01 86 01") && lineb(1, ""));
    n = bridge_json(j, sizeof j);
    assert(n && strstr(j, "\"modbus\":{\"requests\":6,\"replies\":3,") && strstr(j, "\"refused\":3,"));
    set("port2.modbus_broadcast=1");                                    /* allowed: out, unanswered */
    sayb(mr, "00 0b 00 00 00 06 00 03 00 00 00 01");
    assert(lineb(1, "00 03 00 00 00 01 85 db") && mr->out_n == 0);
    t_now += 100;
    bridge_poll();
    set("port2.modbus_broadcast=0");
    sayb(mw, "00 06 00 01 00 06 01 03 00 00 00 01");                    /* not Modbus TCP: closed */
    assert(mw->closed && !mr->closed);
    mr->rx(mr->arg, mr, NULL, ERR_OK);                                   /* the client closes */
    bridge_poll();
    assert(mr->closed);
    set("port2.listen_only=0\nport2.enabled=0\nport2.access=");
    /* RTU frames over TCP with unit 0 answered: SolarAssistant through socat, to EG4 inverters.
     * Back to Modbus TCP without reopening the line or closing the client. */
    set("port2.enabled=1\nport2.mode=modbus\nport2.local_port=4002\nport2.modbus_framing=rtu\nport2.modbus_broadcast=answer");
    struct tcp_pcb *sa = from(4002, "10.0.0.7");
    assert(!sa->aborted);
    sayb(sa, "00 04 00 00 00 01 30 1b");
    assert(lineb(1, "00 04 00 00 00 01 30 1b"));
    heardb(1, "00 04 02 00 0c 84 f5");
    assert(outb(sa, "00 04 02 00 0c 84 f5"));
    set("port2.modbus_framing=tcp\nport2.modbus_broadcast=0");
    assert(!sa->closed);
    sayb(sa, "00 01 00 00 00 06 01 03 00 00 00 01");
    assert(lineb(1, "01 03 00 00 00 01 84 0a"));
    heardb(1, "01 03 02 00 2a 39 9b");
    assert(outb(sa, "00 01 00 00 00 05 01 03 02 00 2a"));
    set("port2.enabled=0\nport2.mode=server");

    /* A Modbus port that devices poll listens as usual: the engine's requests and its TCP
     * clients' share the queue, in turn; the engine's replies reach device_reply(). */
    cfg_defaults(&C);
    set("port3.enabled=1\nport3.mode=modbus\nport3.local_port=4003");
    assert(!strcmp(info(2).state, "listening") && listener(4003));
    assert(bridge_modbus_port(2) && !bridge_modbus_port(0) && !bridge_modbus_port(3));
    assert(modbus_local(bridge_modbus_port(2), 5, (const uint8_t[]){ 3, 0, 0, 0, 1 }, 5, 0, 42));
    struct tcp_pcb *mc = from(4003, "10.0.0.9");
    assert(!mc->aborted);
    sayb(mc, "00 07 00 00 00 06 01 03 00 00 00 01");                    /* behind the engine's */
    assert(lineb(2, "05 03 00 00 00 01 85 8e"));
    heardb(2, "05 03 02 00 07 08 46");
    assert(dr_tag == 42 && dr_st == 0 && dr_n == 4 && mc->out_n == 0);
    bridge_poll();
    assert(lineb(2, "01 03 00 00 00 01 84 0a"));
    heardb(2, "01 03 02 00 2a 39 9b");
    assert(outb(mc, "00 07 00 00 00 05 01 03 02 00 2a") && dr_tag == 42);
    set("port3.enabled=0\nport3.mode=server");
    assert(!bridge_modbus_port(2));

    /* modbus_tcp=0: no listener, only the devices use the line. A change opens or closes the
     * listener alone: the line stays open and the devices' requests stay queued. */
    cfg_defaults(&C);
    int was3 = opened[2];
    set("port3.enabled=1\nport3.mode=modbus\nport3.local_port=4003\nport3.modbus_tcp=0");
    assert(opened[2] == was3 + 1 && !listener(4003) && !strcmp(info(2).state, "ready") && info(2).up && bridge_modbus_port(2));
    assert(modbus_local(bridge_modbus_port(2), 5, (const uint8_t[]){ 3, 0, 0, 0, 1 }, 5, 0, 43));
    bridge_poll();
    assert(lineb(2, "05 03 00 00 00 01 85 8e"));
    heardb(2, "05 03 02 00 07 08 46");
    assert(dr_tag == 43 && dr_st == 0);
    set("port3.modbus_tcp=1");                                          /* on: listens; the line is not reopened */
    assert(opened[2] == was3 + 1 && listener(4003) && !strcmp(info(2).state, "listening"));
    struct tcp_pcb *mo = from(4003, "10.0.0.9");
    assert(!mo->aborted);
    assert(modbus_local(bridge_modbus_port(2), 5, (const uint8_t[]){ 3, 0, 0, 0, 1 }, 5, 0, 44));
    sayb(mo, "00 08 00 00 00 06 01 03 00 00 00 01");                    /* behind the engine's */
    assert(lineb(2, "05 03 00 00 00 01 85 8e"));
    set("port3.modbus_tcp=0");                                          /* off: the client goes, the engine's request stays */
    assert(mo->closed && !listener(4003) && !strcmp(info(2).state, "ready") && opened[2] == was3 + 1);
    heardb(2, "05 03 02 00 07 08 46");
    assert(dr_tag == 44 && dr_st == 0);
    bridge_poll();
    assert(lineb(2, "") && mo->out_n == 0);                             /* the client's request was dropped */
    set("port3.quiet_min=1");                                           /* no "no client" alert without TCP */
    for (int k = 0; k < 8; k++) { t_now += 10000; heardb(2, "00"); }
    assert(!info(2).alert[0]);
    set("port3.modbus_tcp=1");                                          /* with it, there is */
    for (int k = 0; k < 8; k++) { t_now += 10000; heardb(2, "00"); }
    assert(!strcmp(info(2).alert, "no_client"));
    set("port3.enabled=0\nport3.mode=server\nport3.modbus_tcp=1\nport3.quiet_min=0");
    assert(!bridge_modbus_port(2));

    /* the widest state JSON still fits what /api/state leaves for it */
    cfg_defaults(&C); set("port1.enabled=1");

    /* ---- a client that falls behind ---- */
    set("port1.access=");
    a = from(4001, "10.0.0.7");
    int o0 = a->outputs;
    heard(0, "one");
    assert(a->flags == TCP_WRITE_FLAG_COPY && a->outputs == o0 + 1);    /* keeping up: its own segment, sent at once */
    a->snd_queuelen = 16; a->unacked = a;                               /* 16 packets not yet acknowledged */
    heard(0, "two");
    assert(a->flags == (TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) && a->outputs == o0 + 1);   /* merged, and held */
    a->unacked = NULL;                                                  /* nothing in flight to bring it out */
    heard(0, "three");
    assert(a->flags == (TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) && a->outputs == o0 + 2);
    a->snd_queuelen = 0;
    heard(0, "four");
    assert(a->flags == TCP_WRITE_FLAG_COPY && a->outputs == o0 + 3);
    n = bridge_json(j, sizeof j);
    assert(n && !strstr(j, "peer_lost"));                               /* nobody has missed anything */
    b = from(4001, "10.0.0.8");
    uint32_t l0 = info(0).net_lost;
    b->full = true;                                                     /* its send buffer is full */
    heard(0, "12345");
    assert(info(0).net_lost == l0 + 5 && a->out_n >= 5);                /* b misses it, a does not */
    n = bridge_json(j, sizeof j);
    assert(n && strstr(j, "\"peer_lost\":[0,5]"));                      /* a, then b */
    b->full = false;

    for (int p = 0; p < PORTS; p++) { memset((void *)&g_uart[p], 0xFF, sizeof g_uart[p]); char t[64]; snprintf(t, sizeof t, "port%d.enabled=1", p + 1); set(t); for (int k = 0; k < 5; k++) from((u16_t)(4001 + p), "255.255.255.255"); }
    n = bridge_json(j, sizeof j);
    printf("widest port state %zu bytes\n", n);
    assert(n && n < 4344 - 900);
    for (int p = 0; p < PORTS; p++) { char t[96]; snprintf(t, sizeof t, "port%d.mode=modbus\nport%d.local_port=%d", p + 1, p + 1, 502 + p); set(t); for (int k = 0; k < 5; k++) from((u16_t)(502 + p), "255.255.255.255"); }
    n = bridge_json(j, sizeof j);
    printf("widest with Modbus %zu bytes, and its 11 figures as long as they get\n", n + PORTS * 11 * 9);
    assert(n && n + PORTS * 11 * 9 + PORTS * 40 + PORTS * 58 < 4344 - 900);   /* and "fwd_us" and "peer_lost" at their longest */
    for (size_t k = 0; k < n; k++) assert(!bridge_json(j, k));
    puts("ok");
    return 0;
}
