/*
 * Network -> serial: received TCP data is copied into a per-connection ring the
 * size of the TCP window and acknowledged to the peer only as it drains, so a
 * slow line pushes back on the sender and no lwIP buffers are held.
 * Serial -> network: bytes are collected until the line goes quiet (gap) or the
 * packet is full, then sent to every connection. A peer that cannot keep up
 * loses that packet; the others are unaffected.
 */
#include "bridge.h"
#include "uart485.h"
#include "traffic.h"
#include "stm32h7xx_hal.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include <stdio.h>
#include <string.h>

#define D2 __attribute__((section(".d2_bss")))
#define RING TCP_WND

typedef struct {
    struct tcp_pcb *pcb;
    uint32_t head, count;       /* ring read position, bytes held */
    bool     fin;               /* peer sent FIN; flush then close */
    uint32_t drained_at;
    bool     up;                /* connected (client mode: SYN completed) */
    uint8_t  port;
} client_t;

enum { LS_OFF, LS_LISTENING, LS_CONNECTING, LS_CONNECTED, LS_WAITING, LS_BOUND, LS_ERROR };
static const char *const LS_NAME[] = { "off", "listening", "connecting", "connected",
                                       "retrying", "ready", "error" };

typedef struct {
    port_cfg_t cfg;
    uint8_t  state;
    struct tcp_pcb *listener;
    struct udp_pcb *udp;
    ip_addr_t udp_peer;
    u16_t    udp_peer_port;
    bool     udp_have_peer;
    client_t cl[MAX_CLIENTS];
    uint32_t retry_at, backoff;
    uint32_t gen;               /* invalidates in-flight DNS answers */
    uint32_t net_rx, net_tx, net_lost, refused;
} port_t;

static port_t s_p[PORTS];
static D2 uint8_t s_ring[PORTS][MAX_CLIENTS][RING];
static port_cfg_t s_want[PORTS];
static bool s_dirty;

static size_t to_line(int i, const uint8_t *d, size_t n)
{
    if (s_p[i].cfg.listen_only) return n;   /* taken from the network, never sent */
    n = uart485_write(i, d, n);
    traffic_add(i, true, d, n, HAL_GetTick());
    return n;
}

bool bridge_send(int i, const uint8_t *d, size_t n)
{
    return s_p[i].state != LS_OFF && !s_p[i].cfg.listen_only && uart485_tx_room(i) >= n && to_line(i, d, n) == n;
}

/* ---- TCP connections ----------------------------------------------------- */
static void client_detach(client_t *c)
{
    tcp_arg(c->pcb, NULL);
    tcp_recv(c->pcb, NULL);
    tcp_err(c->pcb, NULL);
    c->pcb = NULL;
    c->up = false;
    c->count = 0;
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
    /* The ring equals the advertised window, so this always fits. */
    if (q->tot_len > RING - c->count) return ERR_MEM;
    int idx = (int)(c - s_p[c->port].cl);
    for (struct pbuf *b = q; b; b = b->next) {
        const uint8_t *d = b->payload;
        uint32_t w = (c->head + c->count) % RING, n = b->len, k = RING - w;
        if (k > n) k = n;
        memcpy(&s_ring[c->port][idx][w], d, k);
        memcpy(&s_ring[c->port][idx][0], d + k, n - k);
        c->count += n;
    }
    s_p[c->port].net_rx += q->tot_len;
    pbuf_free(q);
    return ERR_OK;
}

static void cb_err(void *arg, err_t err)
{
    client_t *c = arg;
    (void)err;
    if (!c) return;
    c->pcb = NULL;              /* lwIP already freed it */
    c->up = false;
    c->count = 0;
    port_t *p = &s_p[c->port];
    if (p->cfg.mode == MODE_CLIENT && p->state != LS_OFF) schedule_retry(p);
}

static void client_attach(port_t *p, client_t *c, struct tcp_pcb *pcb)
{
    c->pcb = pcb;
    c->head = c->count = 0;
    c->fin = false;
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
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (p->cl[i].pcb) continue;
        client_attach(p, &p->cl[i], pcb);
        p->cl[i].up = true;
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
        if (tcp_bind(l, IP_ADDR_ANY, p->cfg.local_port) == ERR_OK) {
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
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { schedule_retry(p); return; }
    client_attach(p, &p->cl[0], pcb);
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
    if (!p->cfg.remote_host[0]) {           /* reply to whoever spoke last */
        p->udp_peer = *from;
        p->udp_peer_port = port;
        p->udp_have_peer = true;
    } else if (!p->udp_have_peer || !ip_addr_eq(from, &p->udp_peer)) {
        pbuf_free(q);                       /* not the configured peer */
        return;
    }
    if (uart485_tx_room(i) >= q->tot_len) {
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
    if (!p->udp || udp_bind(p->udp, IP_ADDR_ANY, p->cfg.local_port) != ERR_OK) {
        if (p->udp) { udp_remove(p->udp); p->udp = NULL; }
        p->state = LS_ERROR;
        p->retry_at = HAL_GetTick() + 1000;
        return;
    }
    udp_recv(p->udp, cb_udp, p);
    if (p->cfg.remote_host[0]) start_resolve(p);
    else p->state = LS_BOUND;
}

/* ---- start / stop -------------------------------------------------------- */
static void port_start(int i)
{
    port_t *p = &s_p[i];
    if (!p->cfg.enabled) return;
    uart485_open(i, &p->cfg);
    switch (p->cfg.mode) {
    case MODE_SERVER: start_listen(p); break;
    case MODE_CLIENT: start_resolve(p); break;
    default:          start_udp(p);
    }
}

static void port_stop(int i)
{
    port_t *p = &s_p[i];
    p->state = LS_OFF;
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
    uart485_init();
    traffic_init();
    memset(s_p, 0, sizeof s_p);
}

void bridge_configure(const port_cfg_t cfg[PORTS])
{
    memcpy(s_want, cfg, sizeof s_want);
    s_dirty = true;
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

static void serial_to_net(int i)
{
    static uint8_t buf[1400];
    port_t *p = &s_p[i];
    size_t a = uart485_rx_avail(i);
    if (!a || (a < p->cfg.packet_len && uart485_rx_idle_us(i) < gap_us(&p->cfg))) return;
    if (a > p->cfg.packet_len) a = p->cfg.packet_len;
    uint16_t n = (uint16_t)uart485_read(i, buf, a);
    traffic_add(i, false, buf, n, HAL_GetTick());

    if (p->cfg.mode == MODE_UDP) {
        struct pbuf *q = p->udp_have_peer ? pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM) : NULL;
        if (q) {
            pbuf_take(q, buf, n);
            if (udp_sendto(p->udp, q, &p->udp_peer, p->udp_peer_port) == ERR_OK) p->net_tx += n;
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
        if (tcp_sndbuf(pcb) >= n && tcp_write(pcb, buf, n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            tcp_output(pcb);
            sent = true;
        } else {
            p->net_lost += n;
        }
    }
    if (sent) p->net_tx += n;
}

static void net_to_serial(int i)
{
    port_t *p = &s_p[i];
    for (int k = 0; k < MAX_CLIENTS; k++) {
        client_t *c = &p->cl[k];
        if (!c->pcb) continue;
        while (c->count) {
            uint32_t n = RING - c->head;
            if (n > c->count) n = c->count;
            n = (uint32_t)to_line(i, &s_ring[i][k][c->head], n);
            if (!n) break;
            c->head = (c->head + n) % RING;
            c->count -= n;
            c->drained_at = HAL_GetTick();
            tcp_recved(c->pcb, (u16_t)n);
        }
        /* After a half-close, leave time for the device to answer. */
        if (c->fin && !c->count && uart485_tx_idle(i) && HAL_GetTick() - c->drained_at > 1000) {
            client_close(c);
            if (p->cfg.mode == MODE_CLIENT) schedule_retry(p);
        }
    }
}

void bridge_poll(void)
{
    if (s_dirty) {
        s_dirty = false;
        for (int i = 0; i < PORTS; i++) {
            if (same_wiring(&s_p[i].cfg, &s_want[i])) { s_p[i].cfg = s_want[i]; continue; }
            port_stop(i);
            s_p[i].cfg = s_want[i];
            port_start(i);
        }
    }
    for (int i = 0; i < PORTS; i++) {
        port_t *p = &s_p[i];
        if (p->state == LS_OFF) continue;
        serial_to_net(i);
        net_to_serial(i);
        if ((p->state == LS_WAITING || p->state == LS_ERROR) &&
            (int32_t)(HAL_GetTick() - p->retry_at) >= 0) {
            if (p->cfg.mode == MODE_SERVER) start_listen(p);
            else if (p->cfg.mode == MODE_UDP && !p->udp) start_udp(p);
            else start_resolve(p);
        }
    }
}

void bridge_info(int i, bridge_info_t *o)
{
    port_t *p = &s_p[i];
    volatile uart_stats_t *u = &g_uart[i];
    o->state = LS_NAME[p->state];
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
    o->serial_rx = u->rx_bytes;
    o->serial_tx = u->tx_bytes;
    o->errors = u->parity + u->framing + u->noise + u->overrun + u->rx_lost + p->net_lost + p->refused;
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
        ADD("%s{\"state\":\"%s\",\"serial_rx\":%lu,\"serial_tx\":%lu,\"net_rx\":%lu,\"net_tx\":%lu,"
            "\"net_lost\":%lu,\"rx_lost\":%lu,\"parity\":%lu,\"framing\":%lu,\"noise\":%lu,"
            "\"overrun\":%lu,\"echo\":%lu,\"refused\":%lu,\"peers\":[",
            i ? "," : "", LS_NAME[p->state], (unsigned long)u->rx_bytes, (unsigned long)u->tx_bytes,
            (unsigned long)p->net_rx, (unsigned long)p->net_tx, (unsigned long)p->net_lost,
            (unsigned long)u->rx_lost, (unsigned long)u->parity, (unsigned long)u->framing,
            (unsigned long)u->noise, (unsigned long)u->overrun, (unsigned long)u->echo_dropped,
            (unsigned long)p->refused);
        bool first = true;
        char ip[16];
        for (int k = 0; k < MAX_CLIENTS; k++) {
            if (!p->cl[k].pcb || !p->cl[k].up) continue;
            ipaddr_ntoa_r(&p->cl[k].pcb->remote_ip, ip, sizeof ip);
            ADD("%s\"%s:%u\"", first ? "" : ",", ip, p->cl[k].pcb->remote_port);
            first = false;
        }
        if (p->udp && p->udp_have_peer) {
            ipaddr_ntoa_r(&p->udp_peer, ip, sizeof ip);
            ADD("\"%s:%u\"", ip, p->udp_peer_port);
        }
        ADD("]}");
    }
    ADD("]");
    return n;
#undef ADD
}
