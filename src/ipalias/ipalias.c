/* ipalias.c — more IPv4 addresses for one lwIP Ethernet interface. See ipalias.h. */
#include "ipalias.h"
#include "lwip/acd.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include <stdbool.h>
#include <string.h>

typedef struct {
    struct netif    nif;
    struct acd      acd;
    ip4_addr_t      addr;
    ipalias_state_t st;
    struct eth_addr other;      /* the last device heard probing for or claiming it */
    bool            used;       /* nif is on lwIP's list */
} alias_t;

static alias_t s_a[IPALIAS_MAX];
static struct netif *s_main;
static ipalias_cb s_cb;

static void tell(int i)
{
    if (s_cb) s_cb(i, s_a[i].st, s_a[i].st == IPALIAS_CONFLICT ? &s_a[i].other : NULL);
}

static int place_of(const struct netif *n)
{
    for (int i = 0; i < IPALIAS_MAX; i++)
        if (s_a[i].used && &s_a[i].nif == n) return i;
    return -1;
}

static void unset(alias_t *a)
{
    if (!ip4_addr_isany_val(*netif_ip4_addr(&a->nif))) netif_set_ipaddr(&a->nif, IP4_ADDR_ANY4);
}

static void acd_said(struct netif *n, acd_callback_enum_t what)
{
    int i = place_of(n);
    if (i < 0) return;
    alias_t *a = &s_a[i];
    if (what == ACD_IP_OK) {
        netif_set_addr(&a->nif, &a->addr, netif_ip4_netmask(s_main), netif_ip4_gw(s_main));
        a->st = IPALIAS_LIVE;
        tell(i);
    } else if (what == ACD_DECLINE) {
        unset(a);
        a->st = IPALIAS_CONFLICT;
        tell(i);
    } else {                            /* ACD_RESTART_CLIENT: tried again only when asked */
        acd_stop(&a->acd);
    }
}

/* From the start: no address until the probes pass; probing only with a link. */
static void check(int i)
{
    alias_t *a = &s_a[i];
    acd_stop(&a->acd);
    unset(a);
    a->st = IPALIAS_CHECKING;
    if (netif_is_link_up(s_main)) acd_start(&a->nif, &a->acd, a->addr);
    tell(i);
}

static err_t if_init(struct netif *n)
{
    n->name[0] = 'a'; n->name[1] = '0' + (char)place_of(n);
    n->output = etharp_output;
    n->linkoutput = s_main->linkoutput;
    n->state = s_main->state;
    n->mtu = s_main->mtu;
    n->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(n->hwaddr, s_main->hwaddr, ETH_HWADDR_LEN);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static void add(int i, const ip4_addr_t *addr)
{
    alias_t *a = &s_a[i];
    memset(a, 0, sizeof *a);
    a->addr = *addr;
    a->used = true;                     /* before netif_add: if_init looks itself up */
    if (!netif_add(&a->nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, if_init, ethernet_input)) {
        memset(a, 0, sizeof *a);
        return;
    }
    acd_add(&a->nif, &a->acd, acd_said);
    netif_set_up(&a->nif);
    check(i);
}

static void drop(int i)
{
    alias_t *a = &s_a[i];
    acd_stop(&a->acd);
    acd_remove(&a->nif, &a->acd);
    netif_remove(&a->nif);
    memset(a, 0, sizeof *a);
}

void ipalias_init(struct netif *main, ipalias_cb cb)
{
    memset(s_a, 0, sizeof s_a);
    s_main = main;
    s_cb = cb;
}

void ipalias_set(const ip4_addr_t *addr, int n)
{
    if (!s_main) return;
    if (n > IPALIAS_MAX) n = IPALIAS_MAX;
    for (int i = 0; i < IPALIAS_MAX; i++) {
        if (!s_a[i].used) continue;
        bool kept = false;
        for (int k = 0; k < n; k++) kept = kept || ip4_addr_eq(&s_a[i].addr, &addr[k]);
        if (!kept) drop(i);
        else if (s_a[i].st == IPALIAS_CONFLICT) check(i);
    }
    for (int k = 0; k < n; k++) {
        if (ip4_addr_isany_val(addr[k])) continue;
        bool have = false;
        for (int i = 0; i < IPALIAS_MAX; i++) have = have || (s_a[i].used && ip4_addr_eq(&s_a[i].addr, &addr[k]));
        for (int i = 0; !have && i < IPALIAS_MAX; i++)
            if (!s_a[i].used) { add(i, &addr[k]); have = true; }
    }
}

void ipalias_link(int up)
{
    for (int i = 0; i < IPALIAS_MAX; i++) {
        alias_t *a = &s_a[i];
        if (!a->used) continue;
        if (a->st == IPALIAS_LIVE) { if (up) etharp_gratuitous(&a->nif); }
        else if (up) check(i);
        else acd_stop(&a->acd);
    }
}

struct netif *ipalias_input_netif(const struct pbuf *p)
{
    uint8_t f[42];                      /* Ethernet header, then ARP: sender at 22 and 28, target at 38 */
    bool any = false;
    for (int i = 0; i < IPALIAS_MAX; i++) any |= s_a[i].used;
    if (!any || !s_main || p->tot_len < sizeof f || pbuf_copy_partial(p, f, sizeof f, 0) != sizeof f) return NULL;
    if (f[12] != 0x08 || f[13] != 0x06) return NULL;
    ip4_addr_t s, t;
    memcpy(&s.addr, f + 28, 4);
    memcpy(&t.addr, f + 38, 4);
    for (int i = 0; i < IPALIAS_MAX; i++) {
        alias_t *a = &s_a[i];
        if (!a->used || a->st == IPALIAS_CONFLICT || (!ip4_addr_eq(&s, &a->addr) && !ip4_addr_eq(&t, &a->addr))) continue;
        bool about = ip4_addr_eq(&s, &a->addr) || ip4_addr_isany_val(s);     /* a claim, or a probe */
        if (about && memcmp(f + 22, s_main->hwaddr, ETH_HWADDR_LEN)) memcpy(a->other.addr, f + 22, ETH_HWADDR_LEN);
        return &a->nif;
    }
    return NULL;
}

ipalias_state_t ipalias_state(int i, ip4_addr_t *addr)
{
    if (i < 0 || i >= IPALIAS_MAX || !s_a[i].used) return IPALIAS_OFF;
    if (addr) *addr = s_a[i].addr;
    return s_a[i].st;
}
