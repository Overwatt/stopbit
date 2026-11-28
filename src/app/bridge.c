/*
 * Network -> serial: received TCP data is copied into a ring the size of the TCP
 * window and acknowledged to the peer only as it drains, so a slow line pushes back
 * on the sender and no lwIP buffers are held. The rings are shared: a connection
 * holds one only while it has bytes on their way to the line. Data that will not
 * reach the line (a reader, a listen-only port) takes no ring: it is acknowledged
 * and dropped as it comes. If every ring is in use, lwIP keeps the data and offers
 * it again (each 250 ms): that sender waits, nothing is lost.
 * Serial -> network: bytes are collected until the line goes quiet (gap) or the
 * packet is full, then sent to every connection. Each packet is a TCP segment of its
 * own, sent at once, and lwIP holds 32 of those for a connection. So once a connection
 * has BEHIND packets unacknowledged, later ones are added to one segment that goes when
 * it catches up: the backlog is then the send buffer (TCP_SND_BUF, 17.5 KB), not a few
 * dozen packets. A peer further behind than that loses the packet; the others are unaffected.
 * The access list covers whoever is at the other end, in every mode. Data from one
 * that may only read is taken, so it is not held up, and dropped.
 */
#include "bridge.h"
#include "device.h"
#include "uart485.h"
#include "traffic.h"
#include "history.h"
#include "quiet.h"
#include "knock.h"
#include "modbus.h"
#include "addrs.h"
#include "stm32h7xx_hal.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include "events.h"
#include <stdio.h>
#include <string.h>

#ifndef D2
#define D2 __attribute__((section(".d2_bss")))
#endif
#define RING TCP_WND
#define BEHIND 16               /* of lwIP's 32: a fast line and a peer slow to acknowledge stay below this */
#ifndef RINGS
#define RINGS 6                 /* of 16 places; a ring is 5.7 KB */
#endif

typedef struct {
    struct tcp_pcb *pcb;
    uint8_t *ring;              /* while bytes are held for the line; else NULL */
    uint32_t head, count;       /* ring read position, bytes held */
    bool     fin;               /* peer sent FIN; flush then close */
    uint32_t drained_at;
    bool     up;                /* connected (client mode: SYN completed) */
    uint8_t  port;
    bool     may_write;
    bool     kill;              /* not Modbus TCP: closed on the next poll */
    uint32_t lost;              /* bytes heard that it was too far behind to be sent */
} client_t;

enum { LS_OFF, LS_LISTENING, LS_CONNECTING, LS_CONNECTED, LS_WAITING, LS_BOUND, LS_ERROR };
static const char *const LS_NAME[] = { "off", "listening", "connecting", "connected",
                                       "retrying", "ready", "error" };

typedef struct {
    port_cfg_t cfg;
    uint32_t ip;                /* the address it listens on (portN.ip); 0 every one */
    uint8_t  state;
    struct tcp_pcb *listener;
    struct udp_pcb *udp;
    ip_addr_t udp_peer;
    u16_t    udp_peer_port;
    bool     udp_have_peer;
    client_t cl[MAX_CLIENTS];
    uint32_t retry_at, backoff;
    uint32_t gen;               /* invalidates in-flight DNS answers */
    uint32_t net_rx, net_tx, net_lost, refused, denied, ignored;
    knocks_t knocks;             /* whom the access list turned away */
    uint32_t heard_at, sent_at;  /* ms; valid once bytes have moved */
    bool heard, sent;
    uint32_t quiet_from, client_from;   /* last bytes / last client, or when it opened */
    quiet_t alert;
    fwd_t    fwd;                /* how long packets heard waited to go out */
} port_t;

static port_t s_p[PORTS];
static D2 uint8_t s_ring[RINGS][RING];
static bool s_ring_used[RINGS];
static uint32_t s_rings, s_rings_peak, s_rings_waits;
static port_cfg_t s_want[PORTS];
static uint32_t s_want_ip[PORTS];
static uint16_t s_quiet[PORTS];
static acl_t s_acl[PORTS];
static const char *const ALERT[] = { "", "silent", "no_client", "turned_away" };
static bool s_dirty;
/* A Modbus gateway's state, for ports in that mode (modbus.c). */
static D2 modbus_t s_mb[PORTS];
static uint16_t s_mb_timeout[PORTS];
static uint8_t s_mb_broadcast;              /* bit per port */
static uint8_t s_mb_answer, s_mb_rtu;       /* bit per port: unit 0 answers; clients send RTU frames */
static uint8_t s_mb_no_tcp;                 /* bit per port: no listener, the devices only */
_Static_assert(MAX_CLIENTS == MB_CLIENTS, "a Modbus client per connection");
static bool is_mb(int i);
static void mb_gone(client_t *c);

static size_t to_line(int i, const uint8_t *d, size_t n)
{
    if (s_p[i].cfg.listen_only) return n;   /* taken from the network, never sent */
    n = uart485_write(i, d, n);
    if (n) { s_p[i].sent = true; s_p[i].sent_at = HAL_GetTick(); }
    traffic_add(i, true, d, n, HAL_GetTick());
    return n;
}

bool bridge_send(int i, const uint8_t *d, size_t n)
{
    return s_p[i].state != LS_OFF && !s_p[i].cfg.listen_only && uart485_tx_room(i) >= n && to_line(i, d, n) == n;
}

/* ---- access list --------------------------------------------------------- */
static acl_role_t role(const port_t *p, const ip_addr_t *ip)
{
    return acl_role(&s_acl[p - s_p], ip4_addr_get_u32(ip_2_ip4(ip)));
}

static void deny(port_t *p, const ip_addr_t *ip)
{
    uint32_t a = ip4_addr_get_u32(ip_2_ip4(ip)), now = HAL_GetTick();
    bool known = false;
    for (int k = 0; k < KNOCK_N; k++)
        known |= p->knocks.k[k].count && p->knocks.k[k].ip == a && now - p->knocks.k[k].at < 600000u;
    p->denied++;
    knock_add(&p->knocks, a, now);
    if (!known) events_add(EV_TURNED_AWAY, (int)(p - s_p) + 1, a, NULL);
}

/* ---- shared rings -------------------------------------------------------- */
static bool ring_take(client_t *c)
{
    for (int i = 0; i < RINGS; i++) {
        if (s_ring_used[i]) continue;
        s_ring_used[i] = true;
        c->ring = s_ring[i];
        c->head = c->count = 0;
        if (++s_rings > s_rings_peak) s_rings_peak = s_rings;
        return true;
    }
    s_rings_waits++;
    return false;
}

static void ring_give(client_t *c)
{
    if (!c->ring) return;
    s_ring_used[(c->ring - s_ring[0]) / RING] = false;
    c->ring = NULL;
    c->head = c->count = 0;
    s_rings--;
}

void bridge_rings(uint32_t *used, uint32_t *peak, uint32_t *size, uint32_t *waits)
{
    *used = s_rings; *peak = s_rings_peak; *size = RINGS; *waits = s_rings_waits;
}

/* ---- TCP connections ----------------------------------------------------- */
static void client_detach(client_t *c)
{
    mb_gone(c);
    tcp_arg(c->pcb, NULL);
    tcp_recv(c->pcb, NULL);
    tcp_err(c->pcb, NULL);
    c->pcb = NULL;
    c->up = false;
    ring_give(c);
}

/* Main-loop context only (never from this pcb's own callback). */
static void client_close(client_t *c)
{
    struct tcp_pcb *pcb = c->pcb;
    if (!pcb) return;
    client_detach(c);
    if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
}

static void schedule_retry(port_t *p)
{
    p->backoff = p->backoff ? p->backoff * 2 : 1000;
    if (p->backoff > 30000) p->backoff = 30000;
    p->retry_at = HAL_GetTick() + p->backoff;
    p->state = LS_WAITING;
}

static err_t cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *q, err_t err)
{
    client_t *c = arg;
    (void)err;
    if (!c) {
        if (q) { tcp_recved(pcb, q->tot_len); pbuf_free(q); }
        return ERR_OK;
    }
    if (!q) { c->fin = true; c->drained_at = HAL_GetTick(); return ERR_OK; }
    port_t *p = &s_p[c->port];
    if (is_mb(c->port)) {                                        /* requests: small, queued in modbus.c */
        p->net_rx += q->tot_len;
        for (struct pbuf *b = q; b && !c->kill; b = b->next)
            if (!modbus_from_client(&s_mb[c->port], (int)(c - p->cl), b->payload, b->len, HAL_GetTick())) c->kill = true;
        tcp_recved(pcb, q->tot_len);
        pbuf_free(q);
        return ERR_OK;
    }
    if (!c->ring && (!c->may_write || p->cfg.listen_only)) {     /* bound nowhere: taken and dropped */
        p->net_rx += q->tot_len;
        if (!c->may_write) p->ignored += q->tot_len;
        tcp_recved(pcb, q->tot_len);
        pbuf_free(q);
        return ERR_OK;
    }
    if (!c->ring && !ring_take(c)) return ERR_MEM;               /* lwIP offers it again */
    /* The ring equals the advertised window, so this always fits. */
    if (q->tot_len > RING - c->count) return ERR_MEM;
    for (struct pbuf *b = q; b; b = b->next) {
        const uint8_t *d = b->payload;
        uint32_t w = (c->head + c->count) % RING, n = b->len, k = RING - w;
        if (k > n) k = n;
        memcpy(&c->ring[w], d, k);
        memcpy(&c->ring[0], d + k, n - k);
        c->count += n;
    }
    p->net_rx += q->tot_len;
    pbuf_free(q);
    return ERR_OK;
}

static void cb_err(void *arg, err_t err)
{
    client_t *c = arg;
    (void)err;
    if (!c) return;
    mb_gone(c);
    c->pcb = NULL;              /* lwIP already freed it */
    c->up = false;
    ring_give(c);
    port_t *p = &s_p[c->port];
    if (p->cfg.mode == MODE_CLIENT && p->state != LS_OFF) schedule_retry(p);
}

static void client_attach(port_t *p, client_t *c, struct tcp_pcb *pcb)
{
    ring_give(c);               /* none, as it was detached; to be sure */
    c->pcb = pcb;
    c->head = c->count = 0;
    c->fin = c->kill = false;
    c->lost = 0;
    c->port = (uint8_t)(p - s_p);
    tcp_arg(pcb, c);
    tcp_recv(pcb, cb_recv);
    tcp_err(pcb, cb_err);
    tcp_nagle_disable(pcb);
    ip_set_option(pcb, SOF_KEEPALIVE);
    pcb->keep_idle = 30000;
    pcb->keep_intvl = 10000;
    pcb->keep_cnt = 4;
}

static err_t cb_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    port_t *p = arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    acl_role_t r = role(p, &pcb->remote_ip);
    if (r == ACL_DENY) {
        deny(p, &pcb->remote_ip);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (p->cl[i].pcb) continue;
        client_attach(p, &p->cl[i], pcb);
        p->cl[i].may_write = r == ACL_WRITE;
        p->cl[i].up = true;
        if (is_mb((int)(p - s_p))) modbus_open(&s_mb[p - s_p], i, r == ACL_WRITE);
        return ERR_OK;
    }
    p->refused++;
    tcp_abort(pcb);
    return ERR_ABRT;
}

static void start_listen(port_t *p)
{
    struct tcp_pcb *l = tcp_new();
    if (l) {
        ip_set_option(l, SOF_REUSEADDR);
        ip_addr_t a = IPADDR4_INIT(p->ip);
        if (tcp_bind(l, p->ip ? &a : IP_ADDR_ANY, p->cfg.local_port) == ERR_OK) {
            struct tcp_pcb *ll = tcp_listen_with_backlog(l, MAX_CLIENTS);
            if (ll) {
                p->listener = ll;
                tcp_arg(ll, p);
                tcp_accept(ll, cb_accept);
                p->state = LS_LISTENING;
                return;
            }
        }
        tcp_close(l);
    }
    p->state = LS_ERROR;
    p->retry_at = HAL_GetTick() + 1000;
}

static err_t cb_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    client_t *c = arg;
    (void)pcb; (void)err;
    if (!c) return ERR_OK;
    c->up = true;
    s_p[c->port].state = LS_CONNECTED;
    s_p[c->port].backoff = 0;
    return ERR_OK;
}

static void connect_to(port_t *p, const ip_addr_t *ip)
{
    acl_role_t r = role(p, ip);
    if (r == ACL_DENY) { deny(p, ip); schedule_retry(p); return; }
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { schedule_retry(p); return; }
    client_attach(p, &p->cl[0], pcb);
    p->cl[0].may_write = r == ACL_WRITE;
    if (tcp_connect(pcb, ip, p->cfg.remote_port, cb_connected) != ERR_OK) {
        client_close(&p->cl[0]);
        schedule_retry(p);
    }
}

/* ---- DNS / UDP ----------------------------------------------------------- */
static void resolved(port_t *p, const ip_addr_t *ip)
{
    if (!ip) { schedule_retry(p); return; }
    if (p->cfg.mode == MODE_CLIENT) { connect_to(p, ip); return; }
    if (role(p, ip) == ACL_DENY) { deny(p, ip); schedule_retry(p); return; }
    p->udp_peer = *ip;
    p->udp_peer_port = p->cfg.remote_port;
    p->udp_have_peer = true;
    p->backoff = 0;
    p->state = LS_BOUND;
}

static void cb_dns(const char *name, const ip_addr_t *ip, void *arg)
{
    uint32_t tag = (uint32_t)arg;           /* gen << 2 | port */
    port_t *p = &s_p[tag & 3];
    (void)name;
    if (p->state == LS_CONNECTING && (p->gen & 0x3FFFFFFFu) == tag >> 2) resolved(p, ip);
}

static void start_resolve(port_t *p)
{
    ip_addr_t ip;
    uint32_t i = (uint32_t)(p - s_p);
    p->state = LS_CONNECTING;
    p->gen++;
    err_t e = dns_gethostbyname(p->cfg.remote_host, &ip, cb_dns, (void *)(p->gen << 2 | i));
    if (e == ERR_OK) resolved(p, &ip);
    else if (e != ERR_INPROGRESS) schedule_retry(p);
}

static void cb_udp(void *arg, struct udp_pcb *u, struct pbuf *q, const ip_addr_t *from, u16_t port)
{
    port_t *p = arg;
    int i = (int)(p - s_p);
    (void)u;
    acl_role_t r = role(p, from);
    if (r == ACL_DENY) {
        deny(p, from);
        pbuf_free(q);
        return;
    }
    if (!p->cfg.remote_host[0]) {           /* reply to whoever spoke last */
        p->udp_peer = *from;
        p->udp_peer_port = port;
        p->udp_have_peer = true;
    } else if (!p->udp_have_peer || !ip_addr_eq(from, &p->udp_peer)) {
        pbuf_free(q);                       /* not the configured peer */
        return;
    }
    if (r != ACL_WRITE) {
        p->ignored += q->tot_len;
    } else if (uart485_tx_room(i) >= q->tot_len) {
        p->net_rx += q->tot_len;
        for (struct pbuf *b = q; b; b = b->next) to_line(i, b->payload, b->len);
    } else {
        p->net_lost += q->tot_len;          /* never send half a datagram */
    }
    pbuf_free(q);
}

static void start_udp(port_t *p)
{
    p->udp = udp_new();
    ip_addr_t a = IPADDR4_INIT(p->ip);
    if (!p->udp || udp_bind(p->udp, p->ip ? &a : IP_ADDR_ANY, p->cfg.local_port) != ERR_OK) {
        if (p->udp) { udp_remove(p->udp); p->udp = NULL; }
        p->state = LS_ERROR;
        p->retry_at = HAL_GetTick() + 1000;
        return;
    }
    udp_recv(p->udp, cb_udp, p);
    if (p->cfg.remote_host[0]) start_resolve(p);
    else p->state = LS_BOUND;
}

/* ---- Modbus --------------------------------------------------------------- */
static bool is_mb(int i) { return s_p[i].cfg.mode == MODE_MODBUS; }
static uint8_t unit0(int i) { return s_mb_answer >> i & 1 ? MB_BC_ANSWER : s_mb_broadcast >> i & 1 ? MB_BC_ALL : MB_BC_OFF; }
static bool mb_no_tcp(int i) { return is_mb(i) && (s_mb_no_tcp >> i & 1); }

static void mb_local(void *ctx, uint32_t tag, int st, const uint8_t *pdu, size_t n)
{
    device_reply((int)((port_t *)ctx - s_p), tag, st, pdu, n);
}

/* The port's Modbus state, for device.c; NULL if it is off or not a gateway. */
modbus_t *bridge_modbus_port(int i)
{
    return i >= 0 && i < PORTS && is_mb(i) && s_p[i].state != LS_OFF ? &s_mb[i] : NULL;
}

static size_t mb_line(void *ctx, const uint8_t *d, size_t n)
{
    int i = (int)((port_t *)ctx - s_p);
    return uart485_tx_room(i) >= n ? to_line(i, d, n) : 0;
}

static void mb_reply(void *ctx, int k, const uint8_t *d, size_t n)
{
    port_t *p = ctx;
    struct tcp_pcb *pcb = p->cl[k].pcb;
    if (pcb && tcp_sndbuf(pcb) >= n && tcp_write(pcb, d, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
        tcp_output(pcb);
        p->net_tx += n;
    } else {
        p->net_lost += n;
    }
}

static void mb_gone(client_t *c)
{
    if (c->pcb && is_mb(c->port)) modbus_close(&s_mb[c->port], (int)(c - s_p[c->port].cl));
}

static uint32_t gap_us(const port_cfg_t *c);

static void mb_start(int i)
{
    const mb_io_t io = { mb_line, mb_reply, mb_local, &s_p[i] };
    modbus_init(&s_mb[i], &io, s_mb_timeout[i], gap_us(&s_p[i].cfg));
    s_mb[i].broadcast = unit0(i); modbus_set_rtu(&s_mb[i], s_mb_rtu >> i & 1);
}

/* Requests out, replies in. The line is read only once the request has gone out, so
 * nothing of the reply is lost. Our own echo is dropped as it comes (echo_cancel), except
 * in the loopback test, where it is the reply: function 6 is answered with a copy. */
static void mb_pump(int i)
{
    port_t *p = &s_p[i];
    modbus_t *m = &s_mb[i];
    uint32_t now = HAL_GetTick();
    modbus_poll(m, now, uart485_rx_idle_us(i), uart485_tx_idle(i));
    size_t a = m->state == MB_SENDING ? 0 : uart485_rx_avail(i);
    if (a) {
        uint8_t b[256];
        size_t n = uart485_read(i, b, a < sizeof b ? a : sizeof b);
        traffic_add(i, false, b, n, now);
        p->heard = true;
        p->heard_at = p->quiet_from = now;
        modbus_from_line(m, b, n);
    }
    modbus_poll(m, now, uart485_rx_idle_us(i), uart485_tx_idle(i));
}

/* ---- start / stop -------------------------------------------------------- */
static void port_start(int i)
{
    port_t *p = &s_p[i];
    if (!p->cfg.enabled) return;
    p->quiet_from = p->client_from = HAL_GetTick();
    memset(&p->fwd, 0, sizeof p->fwd);
    port_cfg_t u = p->cfg;
    if (u.mode == MODE_MODBUS) u.echo_cancel = 1;           /* its own request is not a reply */
    uart485_open(i, &u);
    switch (p->cfg.mode) {
    case MODE_SERVER: start_listen(p); break;
    case MODE_MODBUS:
        mb_start(i);
        if (mb_no_tcp(i)) p->state = LS_BOUND;               /* only its devices use the line */
        else start_listen(p);
        break;
    case MODE_CLIENT: start_resolve(p); break;
    default:          start_udp(p);
    }
}

static void port_stop(int i)
{
    port_t *p = &s_p[i];
    p->state = LS_OFF;
    p->alert = QUIET_OK;
    p->gen++;
    p->backoff = 0;
    p->udp_have_peer = false;
    for (int k = 0; k < MAX_CLIENTS; k++) client_close(&p->cl[k]);
    if (p->listener) { tcp_close(p->listener); p->listener = NULL; }
    if (p->udp) { udp_remove(p->udp); p->udp = NULL; }
    uart485_close(i);
}

void bridge_init(void)
{
    memset(s_mb, 0, sizeof s_mb);       /* D2 SRAM is not zeroed at startup */
    uart485_init();
    history_init(HAL_GetTick());
    traffic_init();
    memset(s_p, 0, sizeof s_p);
    memset(s_ring_used, 0, sizeof s_ring_used);
    s_rings = s_rings_peak = s_rings_waits = 0;
}

void bridge_configure(const cfg_t *c)
{
    for (int i = 0; i < PORTS; i++) s_quiet[i] = cfg_quiet_min(c, i);
    for (int i = 0; i < PORTS; i++) s_mb_timeout[i] = (uint16_t)cfg_modbus_timeout(c, i);
    s_mb_broadcast = c->more.modbus_broadcast;
    s_mb_answer = c->more.modbus_answer; s_mb_rtu = c->more.modbus_rtu;
    s_mb_no_tcp = c->more.modbus_no_tcp;
    memcpy(s_acl, c->access, sizeof s_acl);     /* for new arrivals at once, the rest in bridge_poll */
    uint32_t want[PORTS];
    for (int i = 0; i < PORTS; i++) {
        s_want_ip[i] = cfg_port_ip(c, i);
        want[i] = c->port[i].enabled && c->port[i].mode != MODE_CLIENT ? s_want_ip[i] : 0;
    }
    addrs_want(want);
    memcpy(s_want, c->port, sizeof s_want);
    s_dirty = true;
}

/* modbus_tcp changed on a running Modbus port: open or close the listener alone. The line
 * and the queue carry on; closing a client drops its requests (modbus_close), not the
 * devices'. */
static void apply_tcp(int i)
{
    port_t *p = &s_p[i];
    if (p->state == LS_OFF || !is_mb(i)) return;
    if (!mb_no_tcp(i)) {
        if (p->state == LS_BOUND) start_listen(p);
        return;
    }
    if (p->state == LS_BOUND) return;
    for (int k = 0; k < MAX_CLIENTS; k++) client_close(&p->cl[k]);
    if (p->listener) { tcp_close(p->listener); p->listener = NULL; }
    p->state = LS_BOUND;
}

/* A changed list: drop whoever is no longer on it and nobody else. */
static void apply_access(port_t *p)
{
    for (int k = 0; k < KNOCK_N; k++)           /* now welcome: no longer news */
        if (acl_role(&s_acl[p - s_p], p->knocks.k[k].ip) != ACL_DENY) knock_forget(&p->knocks, k);
    for (int k = 0; k < MAX_CLIENTS; k++) {
        client_t *c = &p->cl[k];
        if (!c->pcb) continue;
        const ip_addr_t ip = c->pcb->remote_ip;
        acl_role_t r = role(p, &ip);
        c->may_write = r == ACL_WRITE;
        if (is_mb((int)(p - s_p))) modbus_may_write(&s_mb[p - s_p], k, c->may_write);
        if (r != ACL_DENY) continue;
        deny(p, &ip);
        client_close(c);
        if (p->cfg.mode == MODE_CLIENT) schedule_retry(p);
    }
    if (p->udp_have_peer && role(p, &p->udp_peer) == ACL_DENY) {
        deny(p, &p->udp_peer);
        p->udp_have_peer = false;
        if (p->cfg.remote_host[0]) schedule_retry(p);
    }
}

static bool same_wiring(const port_cfg_t *a, const port_cfg_t *b)
{
    port_cfg_t x = *a, y = *b;              /* a rename must not drop connections */
    memset(x.name, 0, sizeof x.name);
    memset(y.name, 0, sizeof y.name);
    return !memcmp(&x, &y, sizeof x);
}

/* ---- data ---------------------------------------------------------------- */
static uint32_t gap_us(const port_cfg_t *c)
{
    if (c->gap_ms) return c->gap_ms * 1000u;
    /* 3.5 characters of 11 bits, floored like Modbus RTU. */
    uint32_t us = 38500000u / c->baud;
    return us < 1750u ? 1750u : us;
}

static void forwarded(port_t *p, uint32_t us)
{
    static const uint32_t B[FWD_BUCKETS] = FWD_BOUNDS_US;
    int b = 0;
    while (b < FWD_BUCKETS && us > B[b]) b++;
    p->fwd.n[b]++;
    p->fwd.sum_us += us;
    p->fwd.last_us = us;
    if (us > p->fwd.max_us) p->fwd.max_us = us;
}

/* From the port's own address, through the main interface: udp_sendto refuses a source the
 * route's interface does not have (src/ipalias/ipalias.h). */
static err_t udp_out(port_t *p, struct pbuf *q)
{
    if (!p->ip) return udp_sendto(p->udp, q, &p->udp_peer, p->udp_peer_port);
    if (!addrs_held(p->ip)) return ERR_RTE;      /* being checked, taken by another device, or outside the subnet: not used */
    ip_addr_t src = IPADDR4_INIT(p->ip);
    struct netif *n = ip_route(&src, &p->udp_peer);
    return n ? udp_sendto_if_src(p->udp, q, &p->udp_peer, p->udp_peer_port, n, &src) : ERR_RTE;
}

static void serial_to_net(int i)
{
    static uint8_t buf[1400];
    port_t *p = &s_p[i];
    size_t a = uart485_rx_avail(i);
    uint32_t idle = uart485_rx_idle_us(i);              /* since the packet's last byte */
    if (!a || (a < p->cfg.packet_len && idle < gap_us(&p->cfg))) return;
    if (a > p->cfg.packet_len) a = p->cfg.packet_len;
    uint16_t n = (uint16_t)uart485_read(i, buf, a);
    traffic_add(i, false, buf, n, HAL_GetTick());
    p->heard = true;
    p->heard_at = p->quiet_from = HAL_GetTick();

    if (p->cfg.mode == MODE_UDP) {
        struct pbuf *q = p->udp_have_peer ? pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM) : NULL;
        if (q) {
            pbuf_take(q, buf, n);
            if (udp_out(p, q) == ERR_OK) { p->net_tx += n; forwarded(p, idle); }
            else p->net_lost += n;
            pbuf_free(q);
        } else if (p->udp_have_peer) {
            p->net_lost += n;
        }
        return;
    }
    bool sent = false;
    for (int k = 0; k < MAX_CLIENTS; k++) {
        struct tcp_pcb *pcb = p->cl[k].pcb;
        if (!pcb || !p->cl[k].up) continue;
        /* Behind: into the segment that is waiting, and it stays waiting while anything
         * sent is still unacknowledged (the acknowledgement, or lwIP sending again, sends
         * it). With nothing in flight nothing would, so then it goes now. */
        bool behind = tcp_sndqueuelen(pcb) >= BEHIND;
        if (tcp_sndbuf(pcb) >= n &&
            tcp_write(pcb, buf, n, behind ? TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE : TCP_WRITE_FLAG_COPY) == ERR_OK) {
            if (!behind || !pcb->unacked) tcp_output(pcb);
            sent = true;
        } else {
            p->net_lost += n;
            p->cl[k].lost += n;
        }
    }
    if (sent) { p->net_tx += n; forwarded(p, idle); }
}

static void net_to_serial(int i, bool may_close)
{
    port_t *p = &s_p[i];
    for (int k = 0; k < MAX_CLIENTS; k++) {
        client_t *c = &p->cl[k];
        if (!c->pcb) continue;
        while (c->count) {
            uint32_t n = RING - c->head;
            if (n > c->count) n = c->count;
            if (c->may_write) n = (uint32_t)to_line(i, &c->ring[c->head], n);
            else p->ignored += n;
            if (!n) break;
            c->head = (c->head + n) % RING;
            c->count -= n;
            c->drained_at = HAL_GetTick();
            tcp_recved(c->pcb, (u16_t)n);
        }
        if (!c->count) ring_give(c);    /* for whoever needs one next */
        /* After a half-close, leave time for the device to answer. */
        if (may_close && c->fin && !c->count && uart485_tx_idle(i) && HAL_GetTick() - c->drained_at > 1000) {
            client_close(c);
            if (p->cfg.mode == MODE_CLIENT) schedule_retry(p);
        }
    }
}

static void watch(port_t *p, uint32_t now, uint16_t limit)
{
    bool client = p->cfg.mode == MODE_UDP || mb_no_tcp((int)(p - s_p));   /* no connection to lose */
    for (int k = 0; k < MAX_CLIENTS; k++) client |= p->cl[k].pcb && p->cl[k].up;
    if (client) p->client_from = now;
    p->quiet_from = quiet_clamp(now, p->quiet_from);
    p->client_from = quiet_clamp(now, p->client_from);
    knock_clamp(&p->knocks, now);
    p->heard_at = quiet_clamp(now, p->heard_at);
    p->sent_at = quiet_clamp(now, p->sent_at);
    quiet_t was = p->alert;
    p->alert = quiet_eval(now, limit, p->quiet_from, p->client_from);
    /* Nobody connected, and someone unlisted keeps trying: most likely the usual
     * client at a new address. Known at once, whatever the limit. */
    bool alone = p->cfg.mode == MODE_UDP ? !p->udp_have_peer : !client;
    if (alone && p->alert != QUIET_SILENT && knock_persistent(&p->knocks, now)) p->alert = QUIET_TURNED_AWAY;
    if (p->alert != was) events_add(EV_ALERT, (int)(p - s_p) + 1, 0, ALERT[p->alert]);
}

void bridge_pump(void)
{
    for (int i = 0; i < PORTS; i++) {
        if (s_p[i].state == LS_OFF) continue;
        if (is_mb(i)) { mb_pump(i); continue; }
        serial_to_net(i);
        net_to_serial(i, false);
    }
}

void bridge_poll(void)
{
    hist_count_t h[PORTS];
    for (int i = 0; i < PORTS; i++) {
        volatile uart_stats_t *u = &g_uart[i];
        h[i] = (hist_count_t){ u->rx_bytes, u->tx_bytes,
            u->parity + u->framing + u->overrun + u->rx_lost + s_p[i].net_lost };   /* noise is not an error */
    }
    history_poll(HAL_GetTick(), h, PORTS);
    if (s_dirty) {
        s_dirty = false;
        for (int i = 0; i < PORTS; i++) {
            if (same_wiring(&s_p[i].cfg, &s_want[i]) && s_p[i].ip == s_want_ip[i]) {
                s_p[i].cfg = s_want[i];
                s_mb[i].timeout_ms = s_mb_timeout[i];
                s_mb[i].broadcast = unit0(i); modbus_set_rtu(&s_mb[i], s_mb_rtu >> i & 1);
                apply_tcp(i);
                apply_access(&s_p[i]);
                continue;
            }
            port_stop(i);
            s_p[i].cfg = s_want[i];
            s_p[i].ip = s_want_ip[i];
            port_start(i);
        }
    }
    for (int i = 0; i < PORTS; i++) {
        port_t *p = &s_p[i];
        if (p->state == LS_OFF) continue;
        if (is_mb(i)) {
            mb_pump(i);
            for (int k = 0; k < MAX_CLIENTS; k++)          /* not Modbus TCP, or the client is done */
                if (p->cl[k].pcb && (p->cl[k].kill || p->cl[k].fin)) client_close(&p->cl[k]);
        } else {
            serial_to_net(i);
            net_to_serial(i, true);
        }
        watch(p, HAL_GetTick(), s_quiet[i]);
        if ((p->state == LS_WAITING || p->state == LS_ERROR) &&
            (int32_t)(HAL_GetTick() - p->retry_at) >= 0) {
            if (p->cfg.mode == MODE_SERVER || p->cfg.mode == MODE_MODBUS) start_listen(p);
            else if (p->cfg.mode == MODE_UDP && !p->udp) start_udp(p);
            else start_resolve(p);
        }
    }
}

static uint32_t errors_of(const port_t *p, volatile uart_stats_t *u)
{
    return u->parity + u->framing + u->noise + u->overrun + u->rx_lost + p->net_lost + p->refused;
}

void bridge_brief(int i, bridge_brief_t *o)
{
    port_t *p = &s_p[i];
    volatile uart_stats_t *u = &g_uart[i];
    o->state = LS_NAME[p->state];
    o->alert = ALERT[p->alert];
    o->clients = 0;
    for (int k = 0; k < MAX_CLIENTS; k++) o->clients += p->cl[k].pcb && p->cl[k].up;
    o->serial_rx = u->rx_bytes;
    o->serial_tx = u->tx_bytes;
    o->errors = errors_of(p, u);
    o->denied = p->denied;
}

void bridge_info(int i, bridge_info_t *o)
{
    port_t *p = &s_p[i];
    volatile uart_stats_t *u = &g_uart[i];
    o->state = LS_NAME[p->state];
    o->alert = ALERT[p->alert];
    o->clients = 0;
    o->peers[0] = 0;
    size_t w = 0;
    char ip[16];
    for (int k = 0; k < MAX_CLIENTS; k++) {
        if (!p->cl[k].pcb || !p->cl[k].up) continue;
        ipaddr_ntoa_r(&p->cl[k].pcb->remote_ip, ip, sizeof ip);
        int n = snprintf(o->peers + w, sizeof o->peers - w, "%s%s:%u", w ? ", " : "", ip, p->cl[k].pcb->remote_port);
        if (n > 0 && (size_t)n < sizeof o->peers - w) w += (size_t)n;
        o->clients++;
    }
    o->net_lost = p->net_lost;
    o->refused = p->refused;
    o->denied = p->denied;
    o->ignored = p->ignored;
    o->denied_ip[0] = 0;
    const knock_t *kn = knock_get(&p->knocks, 0);
    if (kn) ip4_fmt(kn->ip, o->denied_ip);
    o->serial_rx = u->rx_bytes;
    o->serial_tx = u->tx_bytes;
    o->errors = errors_of(p, u);
    o->fwd = p->fwd;
    o->net_rx = p->net_rx;
    o->net_tx = p->net_tx;
    o->heard_ms = p->heard ? (int32_t)((HAL_GetTick() - p->heard_at) & 0x7FFFFFFF) : -1;
    o->up = p->state == LS_LISTENING || p->state == LS_CONNECTED || p->state == LS_BOUND;
}

const mb_stats_t *bridge_modbus(int i, uint8_t *waiting)
{
    if (!is_mb(i) || s_p[i].state == LS_OFF) return NULL;
    *waiting = s_mb[i].qn;
    return &s_mb[i].s;
}

size_t bridge_json(char *out, size_t max)
{
    size_t n = 0;
#define ADD(...) do { int w = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= max - n) { return 0; } n += (size_t)w; } while (0)
    ADD("[");
    for (int i = 0; i < PORTS; i++) {
        port_t *p = &s_p[i];
        volatile uart_stats_t *u = &g_uart[i];
        char ip[16] = "";
        const knock_t *kn = knock_get(&p->knocks, 0);
        if (kn) ip4_fmt(kn->ip, ip);
        ADD("%s{\"state\":\"%s\",\"serial_rx\":%lu,\"serial_tx\":%lu,\"net_rx\":%lu,\"net_tx\":%lu,"
            "\"net_lost\":%lu,\"rx_lost\":%lu,\"parity\":%lu,\"framing\":%lu,\"noise\":%lu,"
            "\"overrun\":%lu,\"echo\":%lu,\"refused\":%lu,\"err_10m\":%lu,\"heard_ms\":%ld,\"sent_ms\":%ld,\"alert\":\"%s\","
            "\"denied\":%lu,\"denied_ip\":\"%s\",\"denied_ms\":%ld,\"ignored\":%lu,\"peers\":[",
            i ? "," : "", LS_NAME[p->state], (unsigned long)u->rx_bytes, (unsigned long)u->tx_bytes,
            (unsigned long)p->net_rx, (unsigned long)p->net_tx, (unsigned long)p->net_lost,
            (unsigned long)u->rx_lost, (unsigned long)u->parity, (unsigned long)u->framing,
            (unsigned long)u->noise, (unsigned long)u->overrun, (unsigned long)u->echo_dropped,
            (unsigned long)p->refused, (unsigned long)history_errors(i),
            p->heard ? (long)((HAL_GetTick() - p->heard_at) & 0x7FFFFFFF) : -1L,
            p->sent ? (long)((HAL_GetTick() - p->sent_at) & 0x7FFFFFFF) : -1L, ALERT[p->alert],
            (unsigned long)p->denied, ip,
            kn ? (long)((HAL_GetTick() - kn->at) & 0x7FFFFFFF) : -1L, (unsigned long)p->ignored);
        bool first = true;
        for (int k = 0; k < MAX_CLIENTS; k++) {
            if (!p->cl[k].pcb || !p->cl[k].up) continue;
            ipaddr_ntoa_r(&p->cl[k].pcb->remote_ip, ip, sizeof ip);
            ADD("%s\"%s:%u\"", first ? "" : ",", ip, p->cl[k].pcb->remote_port);
            first = false;
        }
        uint8_t ro = 0;                         /* which of these may only read */
        for (int k = 0, b = 0; k < MAX_CLIENTS; k++)
            if (p->cl[k].pcb && p->cl[k].up) ro |= (uint8_t)(!p->cl[k].may_write << b++);
        if (p->udp && p->udp_have_peer) {
            ipaddr_ntoa_r(&p->udp_peer, ip, sizeof ip);
            ADD("\"%s:%u\"", ip, p->udp_peer_port);
            ro = role(p, &p->udp_peer) != ACL_WRITE;
        }
        ADD("],\"read_only\":%u", ro);
        uint32_t lost = 0;                      /* what each of those did not get, if any did not */
        for (int k = 0; k < MAX_CLIENTS; k++) if (p->cl[k].pcb && p->cl[k].up) lost |= p->cl[k].lost;
        if (lost) {
            first = true;
            for (int k = 0; k < MAX_CLIENTS; k++) {
                if (!p->cl[k].pcb || !p->cl[k].up) continue;
                ADD("%s%lu", first ? ",\"peer_lost\":[" : ",", (unsigned long)p->cl[k].lost);
                first = false;
            }
            ADD("]");
        }
        uint32_t fn = 0;
        for (int b = 0; b <= FWD_BUCKETS; b++) fn += p->fwd.n[b];
        if (fn) ADD(",\"fwd_us\":[%lu,%lu,%lu]", (unsigned long)p->fwd.last_us,     /* last, average, longest */
                    (unsigned long)(p->fwd.sum_us / fn), (unsigned long)p->fwd.max_us);
        if (is_mb(i)) {
            const mb_stats_t *m = &s_mb[i].s;
            ADD(",\"modbus\":{\"requests\":%lu,\"replies\":%lu,\"exceptions\":%lu,\"timeouts\":%lu,\"bad\":%lu,"
                "\"busy\":%lu,\"refused\":%lu,\"broadcasts\":%lu,\"waiting\":%u,\"last_ms\":%lu,\"avg_ms\":%lu,\"max_ms\":%lu}",
                (unsigned long)m->requests, (unsigned long)m->replies, (unsigned long)m->exceptions,
                (unsigned long)m->timeouts, (unsigned long)m->bad, (unsigned long)m->busy, (unsigned long)m->refused,
                (unsigned long)m->broadcasts, (unsigned)s_mb[i].qn, (unsigned long)m->last_ms,
                (unsigned long)(m->replies ? m->total_ms / m->replies : 0), (unsigned long)m->max_ms);
        }
        ADD("}");
    }
    ADD("]");
    return n;
#undef ADD
}

size_t bridge_access_json(int i, char *out, size_t max)
{
    port_t *p = &s_p[i];
    uint32_t now = HAL_GetTick();
    const knock_t *k;
    char ip[16];
    size_t n = 0;
#define ADD(...) do { int w = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= max - n) { return 0; } n += (size_t)w; } while (0)
    ADD("{\"denied\":%lu,\"turned_away\":[", (unsigned long)p->denied);
    for (int j = 0; (k = knock_get(&p->knocks, j)); j++) {
        ip4_fmt(k->ip, ip);
        ADD("%s{\"ip\":\"%s\",\"count\":%lu,\"ms\":%lu}", j ? "," : "", ip, (unsigned long)k->count,
            (unsigned long)(now - k->at));
    }
    ADD("]}\n");
    return n;
#undef ADD
}
