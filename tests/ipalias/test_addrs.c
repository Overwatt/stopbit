/* test_addrs.c — src/app/addrs.c with src/ipalias and real lwIP on the host: which ports'
 * addresses become extra ones, the events, the JSON. Run by tests/ipalias/run.sh. */
#include "addrs.h"
#include "events.h"
#include "ipalias.h"
#include "lwip/init.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern int ev_n; extern ev_kind_t ev_kind; extern uint32_t ev_a; extern char ev_s[64];   /* events_stub.c */

static u32_t s_now;
u32_t sys_now(void) { return s_now; }
static struct netif M;
struct netif *net_netif(void) { return &M; }

static const uint8_t MAC[6] = { 2, 0, 0, 0, 0, 0x10 }, X[6] = { 2, 0, 0, 0, 0, 0x99 };
static err_t link_out(struct netif *n, struct pbuf *p) { (void)n; (void)p; return ERR_OK; }
static err_t m_init(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = '0';
    n->output = etharp_output; n->linkoutput = link_out; n->mtu = 1500;
    n->hwaddr_len = 6; memcpy(n->hwaddr, MAC, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}
static void run(u32_t ms) { for (u32_t t = 0; t < ms; t += 10) { s_now += 10; sys_check_timeouts(); addrs_poll(); } }
static uint32_t ip(int a, int b, int c, int d) { ip4_addr_t x; IP4_ADDR(&x, a, b, c, d); return ip4_addr_get_u32(&x); }
static void claim(uint32_t spa)                        /* X says it has spa */
{
    uint8_t f[42] = { 0 };
    memcpy(f, MAC, 6); memcpy(f + 6, X, 6); f[12] = 0x08; f[13] = 0x06;
    f[15] = 1; f[16] = 0x08; f[18] = 6; f[19] = 4; f[21] = 2;
    memcpy(f + 22, X, 6); memcpy(f + 28, &spa, 4); memcpy(f + 32, MAC, 6);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, sizeof f, PBUF_POOL);
    pbuf_take(p, f, sizeof f);
    struct netif *to = ipalias_input_netif(p);
    if (!to) to = &M;
    if (to->input(p, to) != ERR_OK) pbuf_free(p);
}
static const char *json(void) { static char b[400]; assert(addrs_json(b, sizeof b)); return b; }

int main(void)
{
    srand(1);
    ip4_addr_t a, mask, gw;
    IP4_ADDR(&a, 10, 20, 0, 10); IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 10, 20, 0, 1);
    lwip_init();
    assert(netif_add(&M, &a, &mask, &gw, NULL, m_init, ethernet_input));
    netif_set_default(&M); netif_set_up(&M); netif_set_link_up(&M);
    addrs_init();
    run(100);
    assert(addrs_held(ip(10, 20, 0, 10)) && !addrs_held(0));
    assert(!strcmp(json(), "\"addresses\":[]") && ev_n == 0);

    /* Port 1 blank; 2 and 3 share an address; 4 is outside the subnet. */
    const uint32_t B = ip(10, 20, 0, 11), O = ip(10, 30, 0, 5);
    const uint32_t w1[PORTS] = { 0, B, B, O };
    addrs_want(w1);
    run(10);
    assert(!addrs_held(B) && addrs_held(ip(10, 20, 0, 10)));          /* being checked: not held */
    assert(ev_n == 1 && ev_kind == EV_ADDRESS && ev_a == O && !strcmp(ev_s, "outside"));
    assert(!strcmp(json(), "\"addresses\":[{\"ip\":\"10.20.0.11\",\"state\":\"checking\"},{\"ip\":\"10.30.0.5\",\"state\":\"outside\"}]"));
    run(15000);
    assert(ev_n == 2 && ev_a == B && !strcmp(ev_s, "live"));
    assert(addrs_held(B) && !addrs_held(O));
    assert(strstr(json(), "\"10.20.0.11\",\"state\":\"live\""));
    puts("ok   ports' addresses: one in use, one outside");

    /* Saved again, unchanged: nothing new to say, nothing checked again. */
    addrs_want(w1);
    run(15000);
    assert(ev_n == 2 && strstr(json(), "\"state\":\"live\""));
    /* The main address in a port: not an extra one. */
    const uint32_t w2[PORTS] = { ip(10, 20, 0, 10), B, 0, 0 };
    addrs_want(w2);
    run(10);
    assert(!strcmp(json(), "\"addresses\":[{\"ip\":\"10.20.0.11\",\"state\":\"live\"}]"));
    puts("ok   unchanged; the main address left out");

    /* Lost to another device, then taken by one before use. */
    claim(B); run(500); claim(B);
    assert(ev_n == 3 && ev_a == B && !strcmp(ev_s, "lost 02:00:00:00:00:99"));
    assert(strstr(json(), "\"state\":\"conflict\"") && !addrs_held(B));
    const uint32_t C = ip(10, 20, 0, 12), w3[PORTS] = { C, 0, 0, 0 };
    addrs_want(w3);
    run(500);
    claim(C);
    assert(ev_n == 4 && ev_a == C && !strcmp(ev_s, "taken 02:00:00:00:00:99"));
    puts("ok   lost, and taken");

    /* The main address moves to another subnet: the list made again for it. */
    const uint32_t w4[PORTS] = { B, O, 0, 0 };
    addrs_want(w4);
    run(10);
    ip4_addr_t a2, m2;
    IP4_ADDR(&a2, 10, 30, 0, 10); IP4_ADDR(&m2, 255, 255, 255, 0);
    netif_set_addr(&M, &a2, &m2, &gw);
    int e = ev_n;
    run(15000);
    assert(ev_n == e + 2);                                  /* B outside now, O in use */
    const char *j = json();
    assert(strstr(j, "{\"ip\":\"10.30.0.5\",\"state\":\"live\"}") && strstr(j, "{\"ip\":\"10.20.0.11\",\"state\":\"outside\"}"));
    /* No main address (DHCP not yet answered): none. */
    netif_set_addr(&M, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
    run(10);
    assert(!strcmp(json(), "\"addresses\":[]"));
    puts("ok   the main address changed, and gone");

    /* At its longest: 3 in the subnet, 1 outside. */
    netif_set_addr(&M, &a, &mask, &gw);
    const uint32_t w5[PORTS] = { ip(10, 20, 0, 111), ip(10, 20, 0, 112), ip(10, 20, 0, 113), ip(10, 30, 0, 114) };
    addrs_want(w5);
    run(15000);
    static char big[400];
    size_t k = addrs_json(big, sizeof big);
    printf("longest list %zu bytes\n", k);
    assert(k && k <= 220 && !addrs_json(big, k));          /* and refused when it does not fit */
    puts("addrs: ok");
    return 0;
}
