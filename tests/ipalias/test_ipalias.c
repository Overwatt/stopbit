/* test_ipalias.c — src/ipalias against real lwIP on the host: frames in by hand, the frames
 * sent caught at the link output. Run by tests/ipalias/run.sh. */
#include "ipalias.h"
#include "lwip/init.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static u32_t s_now;
u32_t sys_now(void) { return s_now; }

static struct netif M;
static const uint8_t MAC[6] = { 2, 0, 0, 0, 0, 0x10 }, H[6] = { 2, 0, 0, 0, 0, 0x50 }, X[6] = { 2, 0, 0, 0, 0, 0x99 };
static ip4_addr_t A, B, C, D, HIP, XIP;      /* main, extra ones, a host, another device */

/* ---- what went out ---- */
static uint8_t out[256][128]; static int outn;
static err_t link_out(struct netif *n, struct pbuf *p)
{
    (void)n;
    assert(outn < 256);
    memset(out[outn], 0, sizeof out[outn]);
    pbuf_copy_partial(p, out[outn++], p->tot_len < 128 ? p->tot_len : 128, 0);
    return ERR_OK;
}
static uint32_t at(const uint8_t *f, int o) { uint32_t v; memcpy(&v, f + o, 4); return v; }
static int u16(const uint8_t *f, int o) { return f[o] << 8 | f[o + 1]; }
/* ARP frames sent since `from`, with this opcode, sender address and target address (0: any). */
static int arps(int from, int op, uint32_t spa, uint32_t tpa)
{
    int k = 0;
    for (int i = from; i < outn; i++)
        if (u16(out[i], 12) == 0x0806 && u16(out[i], 20) == op && at(out[i], 28) == spa && (!tpa || at(out[i], 38) == tpa)) k++;
    return k;
}
/* The last TCP segment sent since `from`; NULL if none. */
static const uint8_t *tcp_seg(int from)
{
    for (int i = outn - 1; i >= from; i--)
        if (u16(out[i], 12) == 0x0800 && out[i][23] == 6) return out[i];
    return NULL;
}

/* ---- in ---- */
static void rx(const uint8_t *f, int len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    assert(p);
    pbuf_take(p, f, (u16_t)len);
    struct netif *to = ipalias_input_netif(p);
    if (!to) to = &M;
    if (to->input(p, to) != ERR_OK) pbuf_free(p);
}
static void arp_in(int op, const uint8_t *sha, uint32_t spa, const uint8_t *tha, uint32_t tpa)
{
    static const uint8_t ALL[6] = { 255, 255, 255, 255, 255, 255 }, NONE[6];
    uint8_t f[42] = { 0 };
    memcpy(f, op == 1 ? ALL : tha, 6); memcpy(f + 6, sha, 6); f[12] = 0x08; f[13] = 0x06;
    f[15] = 1; f[16] = 0x08; f[18] = 6; f[19] = 4; f[21] = (uint8_t)op;
    memcpy(f + 22, sha, 6); memcpy(f + 28, &spa, 4); memcpy(f + 32, op == 1 ? NONE : tha, 6); memcpy(f + 38, &tpa, 4);
    rx(f, sizeof f);
}
static void syn_in(uint32_t dst, int dport, int sport)       /* from the host H */
{
    uint8_t f[54] = { 0 };
    memcpy(f, MAC, 6); memcpy(f + 6, H, 6); f[12] = 0x08;
    f[14] = 0x45; f[17] = 40; f[20] = 0x40; f[22] = 64; f[23] = 6;
    memcpy(f + 26, &HIP.addr, 4); memcpy(f + 30, &dst, 4);
    f[34] = (uint8_t)(sport >> 8); f[35] = (uint8_t)sport; f[36] = (uint8_t)(dport >> 8); f[37] = (uint8_t)dport;
    f[41] = 1; f[46] = 0x50; f[47] = 0x02; f[48] = 0xFF; f[49] = 0xFF;        /* seq 1, SYN, window */
    rx(f, sizeof f);
}

static void run(u32_t ms) { for (u32_t t = 0; t < ms; t += 10) { s_now += 10; sys_check_timeouts(); } }

/* ---- what the module said ---- */
static int told, told_i; static ipalias_state_t told_st; static struct eth_addr told_other;
static void said(int i, ipalias_state_t st, const struct eth_addr *other)
{
    told++; told_i = i; told_st = st;
    assert((st == IPALIAS_CONFLICT) == (other != NULL));
    if (other) told_other = *other;
}
static ipalias_state_t state_of(const ip4_addr_t *want)
{
    for (int i = 0; i < IPALIAS_MAX; i++) {
        ip4_addr_t a;
        ipalias_state_t st = ipalias_state(i, &a);
        if (st != IPALIAS_OFF && ip4_addr_eq(&a, want)) return st;
    }
    return IPALIAS_OFF;
}

static err_t m_init(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = '0';
    n->output = etharp_output; n->linkoutput = link_out; n->mtu = 1500;
    n->hwaddr_len = 6; memcpy(n->hwaddr, MAC, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}
static int accepted;
static err_t on_accept(void *arg, struct tcp_pcb *p, err_t e) { (void)arg; (void)e; accepted++; tcp_abort(p); return ERR_ABRT; }

int main(void)
{
    srand(1);
    IP4_ADDR(&A, 10, 20, 0, 10); IP4_ADDR(&B, 10, 20, 0, 11); IP4_ADDR(&C, 10, 20, 0, 12); IP4_ADDR(&D, 10, 20, 0, 13);
    IP4_ADDR(&HIP, 10, 20, 0, 50); IP4_ADDR(&XIP, 10, 20, 0, 99);
    ip4_addr_t mask, gw;
    IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 10, 20, 0, 1);
    lwip_init();
    assert(netif_add(&M, &A, &mask, &gw, NULL, m_init, ethernet_input));
    netif_set_default(&M); netif_set_up(&M); netif_set_link_up(&M);
    ipalias_init(&M, said);
    run(100);

    /* Before anything: frames go to the main interface. */
    uint8_t junk[20] = { 0 };
    struct pbuf *p = pbuf_alloc(PBUF_RAW, sizeof junk, PBUF_POOL); pbuf_take(p, junk, sizeof junk);
    assert(!ipalias_input_netif(p)); pbuf_free(p);

    /* An extra address: probed, not answered for, then in use. */
    int o = outn;
    ipalias_set(&B, 1);
    assert(state_of(&B) == IPALIAS_CHECKING && told_st == IPALIAS_CHECKING);
    arp_in(1, H, HIP.addr, NULL, B.addr);
    run(3000);
    assert(arps(o, 2, B.addr, 0) == 0);                       /* no reply yet */
    assert(arps(o, 1, 0, B.addr) >= 1);                       /* probes: sender 0.0.0.0 */
    run(12000);
    assert(state_of(&B) == IPALIAS_LIVE && told_st == IPALIAS_LIVE);
    assert(arps(o, 1, 0, B.addr) == 3 && arps(o, 1, B.addr, B.addr) == 2);   /* 3 probes, 2 announcements */
    puts("ok   probed, then in use");

    /* Asked for: answered with that address and the main MAC; the main address as before. */
    o = outn;
    arp_in(1, H, HIP.addr, NULL, B.addr);
    arp_in(1, H, HIP.addr, NULL, A.addr);
    assert(arps(o, 2, B.addr, HIP.addr) == 1 && arps(o, 2, A.addr, HIP.addr) == 1);
    for (int i = o; i < outn; i++) assert(!memcmp(out[i] + 6, MAC, 6) && !memcmp(out[i] + 22, MAC, 6));
    puts("ok   ARP for both addresses, one MAC");

    /* TCP: a listener on the extra address takes it, and answers from it; the main address
     * there is refused; a connection out has the main address. */
    struct tcp_pcb *l = tcp_new();
    assert(tcp_bind(l, &B, 502) == ERR_OK);
    l = tcp_listen(l); tcp_accept(l, on_accept);
    o = outn;
    syn_in(B.addr, 502, 40000);
    const uint8_t *s = tcp_seg(o);
    assert(s && at(s, 26) == B.addr && at(s, 30) == HIP.addr && u16(s, 34) == 502 && (s[47] & 0x12) == 0x12);   /* SYN-ACK */
    o = outn;
    syn_in(A.addr, 502, 40001);
    s = tcp_seg(o);
    assert(s && at(s, 26) == A.addr && (s[47] & 0x04));                                                   /* RST */
    struct tcp_pcb *c = tcp_new();
    o = outn;
    assert(tcp_connect(c, &HIP, 80, NULL) == ERR_OK);
    s = tcp_seg(o);
    assert(s && at(s, 26) == A.addr && (s[47] & 0x02));
    tcp_abort(c);
    puts("ok   TCP to the extra address, from it; out from the main one");

    /* UDP bound to the extra address: udp_sendto refuses; udp_sendto_if_src through the main
     * interface sends from it. */
    struct udp_pcb *u = udp_new();
    assert(udp_bind(u, &B, 4005) == ERR_OK);
    p = pbuf_alloc(PBUF_TRANSPORT, 4, PBUF_RAM); memcpy(p->payload, "ping", 4);
    assert(udp_sendto(u, p, &HIP, 9) == ERR_RTE);
    o = outn;
    assert(udp_sendto_if_src(u, p, &HIP, 9, &M, &B) == ERR_OK);
    assert(outn == o + 1 && at(out[o], 26) == B.addr && out[o][23] == 17);
    pbuf_free(p); udp_remove(u);
    puts("ok   UDP from the extra address, through the main interface");

    /* Another device claims it: defended once, given up at the second. */
    o = outn;
    arp_in(2, X, B.addr, MAC, A.addr);
    assert(state_of(&B) == IPALIAS_LIVE && arps(o, 1, B.addr, B.addr) == 1);   /* an announcement */
    run(1000);
    arp_in(2, X, B.addr, MAC, A.addr);
    assert(state_of(&B) == IPALIAS_CONFLICT && told_st == IPALIAS_CONFLICT && !memcmp(told_other.addr, X, 6));
    o = outn;
    arp_in(1, H, HIP.addr, NULL, B.addr);
    run(15000);
    assert(arps(o, 2, B.addr, 0) == 0 && arps(o, 1, 0, B.addr) == 0);         /* not answered, not tried again */
    assert(state_of(&B) == IPALIAS_CONFLICT);
    puts("ok   claimed by another: defended, then given up");

    /* Asked again: checked again. Another device answering the probe for a new one. */
    ip4_addr_t two[2] = { B, C };
    ipalias_set(two, 2);
    assert(state_of(&B) == IPALIAS_CHECKING && state_of(&C) == IPALIAS_CHECKING);
    run(500);
    arp_in(2, X, C.addr, MAC, 0);                            /* X has C: it answers the probe */
    assert(state_of(&C) == IPALIAS_CONFLICT && !memcmp(told_other.addr, X, 6));
    run(15000);
    assert(state_of(&B) == IPALIAS_LIVE && state_of(&C) == IPALIAS_CONFLICT);
    puts("ok   checked again when asked; one already taken found");

    /* The link: unplugged, nothing checked; back, the one in use announced, the rest checked. */
    ip4_addr_t two2[2] = { B, D };
    ipalias_set(two2, 2);                                    /* C given up, D new */
    assert(state_of(&C) == IPALIAS_OFF && state_of(&D) == IPALIAS_CHECKING);
    netif_set_link_down(&M); ipalias_link(0);
    o = outn;
    run(15000);
    assert(state_of(&D) == IPALIAS_CHECKING && arps(o, 1, 0, D.addr) == 0);
    netif_set_link_up(&M); ipalias_link(1);
    assert(arps(o, 1, B.addr, B.addr) >= 1 && state_of(&B) == IPALIAS_LIVE);
    run(15000);
    assert(state_of(&D) == IPALIAS_LIVE);
    puts("ok   unplugged and back");

    /* Unchanged ones stay as they are; one taken off is answered no more. */
    o = outn;
    ipalias_set(&D, 1);
    assert(state_of(&B) == IPALIAS_OFF && state_of(&D) == IPALIAS_LIVE);
    arp_in(1, H, HIP.addr, NULL, B.addr);
    arp_in(1, H, HIP.addr, NULL, D.addr);
    assert(arps(o, 2, B.addr, 0) == 0 && arps(o, 2, D.addr, HIP.addr) == 1 && arps(o, 1, 0, D.addr) == 0);
    ip4_addr_t many[4] = { D, D, C, B };                     /* a repeat, and one too many */
    ipalias_set(many, 4);
    assert(state_of(&D) == IPALIAS_LIVE && state_of(&C) == IPALIAS_CHECKING && state_of(&B) == IPALIAS_OFF);
    ipalias_set(NULL, 0);
    for (int i = 0; i < IPALIAS_MAX; i++) assert(ipalias_state(i, NULL) == IPALIAS_OFF);
    puts("ok   kept, removed, repeats");
    puts("ipalias: ok");
    return 0;
}
