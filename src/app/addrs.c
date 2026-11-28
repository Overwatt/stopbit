/* addrs.c — the gateway's extra addresses. See addrs.h. */
#include "addrs.h"
#include "events.h"
#include "ipalias.h"
#include "net.h"
#include "lwip/ip4_addr.h"
#include <stdio.h>
#include <string.h>

_Static_assert(EXTRA_IPS <= IPALIAS_MAX, "cfg.c allows no more extra addresses than ipalias keeps");

static uint32_t s_want[PORTS];          /* each port's; 0 blank */
static uint32_t s_ip, s_mask;           /* the main address the list was last made for */
static bool s_dirty = true;
static uint32_t s_out[PORTS];           /* outside the main subnet, and told so */
static uint8_t s_last[IPALIAS_MAX];     /* each place's state last told: taken, or lost */

static void said(int i, ipalias_state_t st, const struct eth_addr *other)
{
    ip4_addr_t a;
    ipalias_state(i, &a);
    if (st == IPALIAS_LIVE) events_add(EV_ADDRESS, 0, ip4_addr_get_u32(&a), "live");
    if (st == IPALIAS_CONFLICT) {
        const uint8_t *m = other->addr;
        char s[24];
        snprintf(s, sizeof s, "%s %02x:%02x:%02x:%02x:%02x:%02x", s_last[i] == IPALIAS_LIVE ? "lost" : "taken",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        events_add(EV_ADDRESS, 0, ip4_addr_get_u32(&a), s);
    }
    s_last[i] = (uint8_t)st;
}

void addrs_init(void)
{
    ipalias_init(net_netif(), said);
}

void addrs_want(const uint32_t ip[PORTS])
{
    memcpy(s_want, ip, sizeof s_want);
    s_dirty = true;
}

static bool in(const uint32_t *l, int n, uint32_t a)
{
    for (int k = 0; k < n; k++)
        if (l[k] == a) return true;
    return false;
}

void addrs_poll(void)
{
    const struct netif *m = net_netif();
    uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(m)), mask = ip4_addr_get_u32(netif_ip4_netmask(m));
    if (!s_dirty && ip == s_ip && mask == s_mask) return;
    s_dirty = false;
    s_ip = ip;
    s_mask = mask;
    uint32_t live[IPALIAS_MAX], out[PORTS] = { 0 };
    int n = 0, no = 0;
    for (int i = 0; i < PORTS && ip; i++) {             /* no main address yet (DHCP): none */
        uint32_t a = s_want[i];
        if (!a || a == ip || in(live, n, a) || in(out, no, a)) continue;
        uint32_t host = a & ~mask;
        if ((a & mask) != (ip & mask) || !host || host == ~mask) {      /* outside, or the network or broadcast address */
            if (!in(s_out, PORTS, a)) events_add(EV_ADDRESS, 0, a, "outside");
            out[no++] = a;
        } else if (n < IPALIAS_MAX) {
            live[n++] = a;
        }
    }
    memcpy(s_out, out, sizeof s_out);
    ip4_addr_t l[IPALIAS_MAX];
    for (int k = 0; k < n; k++) ip4_addr_set_u32(&l[k], live[k]);
    ipalias_set(l, n);
}

bool addrs_held(uint32_t ip)
{
    if (!ip) return false;
    if (ip == ip4_addr_get_u32(netif_ip4_addr(net_netif()))) return true;
    for (int i = 0; i < IPALIAS_MAX; i++) {
        ip4_addr_t a;
        if (ipalias_state(i, &a) == IPALIAS_LIVE && ip4_addr_get_u32(&a) == ip) return true;
    }
    return false;
}

#define ADD(...) do { int w = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= max - n) { return 0; } n += (size_t)w; } while (0)

size_t addrs_json(char *out, size_t max)
{
    static const char *const NAME[] = { "off", "checking", "live", "conflict" };
    size_t n = 0;
    char t[16];
    const char *sep = "";
    if (!max) return 0;
    ADD("\"addresses\":[");
    for (int i = 0; i < IPALIAS_MAX; i++) {
        ip4_addr_t a;
        ipalias_state_t st = ipalias_state(i, &a);
        if (st == IPALIAS_OFF) continue;
        ADD("%s{\"ip\":\"%s\",\"state\":\"%s\"}", sep, ip4addr_ntoa_r(&a, t, sizeof t), NAME[st]);
        sep = ",";
    }
    for (int k = 0; k < PORTS; k++) {
        if (!s_out[k]) continue;
        ip4_addr_t a;
        ip4_addr_set_u32(&a, s_out[k]);
        ADD("%s{\"ip\":\"%s\",\"state\":\"outside\"}", sep, ip4addr_ntoa_r(&a, t, sizeof t));
        sep = ",";
    }
    ADD("]");
    return n;
}
