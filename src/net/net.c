#include "net.h"
#include "eth_mac.h"
#include "stm32h7xx_hal.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/autoip.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/apps/mdns.h"
#include "netif/ethernet.h"
#include <stdio.h>
#include <string.h>
#ifdef APP_IMAGE
#include "ipalias.h"
#endif

uint8_t g_mac[6];
char    g_hostname[16];
static struct netif s_if;
static uint32_t s_rand;

u32_t sys_now(void) { return HAL_GetTick(); }

/* Readable over SWD after a failure. */
void *volatile g_assert_at;

void lwip_assert_fail(void)
{
    g_assert_at = __builtin_return_address(0);
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) __BKPT(0);
    NVIC_SystemReset();
}

/* ---- RNG ----------------------------------------------------------------- */
static bool rng_word(uint32_t *out)
{
    uint32_t t0 = HAL_GetTick();
    while (!(RNG->SR & RNG_SR_DRDY)) {
        if (RNG->SR & (RNG_SR_SEIS | RNG_SR_CEIS)) return false;
        if (HAL_GetTick() - t0 > 20) return false;
    }
    *out = RNG->DR;
    return *out != 0;
}

static void rng_init(void)
{
    RCC->CR |= RCC_CR_HSI48ON;
    uint32_t t0 = HAL_GetTick();
    while (!(RCC->CR & RCC_CR_HSI48RDY) && HAL_GetTick() - t0 < 10) { }
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;   /* kernel clock defaults to HSI48 */
    (void)RCC->AHB2ENR;
    RNG->CR = RNG_CR_RNGEN;
}

bool rng_fill(void *dst, uint32_t len)
{
    uint8_t *p = dst;
    while (len) {
        uint32_t w, n = len < 4 ? len : 4;
        if (!rng_word(&w)) return false;
        memcpy(p, &w, n);
        p += n;
        len -= n;
    }
    return true;
}

/* For lwIP's ports/IDs only; never used for authentication. */
uint32_t net_rand(void)
{
    uint32_t w;
    if (rng_word(&w)) return w;
    s_rand = s_rand * 1664525u + 1013904223u + HAL_GetTick();
    return s_rand;
}

/* ---- netif --------------------------------------------------------------- */
#ifdef APP_IMAGE
/* A received frame goes to lwIP where the DMA put it; the buffer goes back to the
 * driver when lwIP frees it. One pbuf for each buffer, as each is lent once at a time. */
static struct pbuf_custom s_rxp[ETH_RX_BUFS];

static void rx_free(struct pbuf *p)
{
    eth_rx_give((uint32_t)((struct pbuf_custom *)p - s_rxp));
}
#endif

#ifdef APP_IMAGE
/* The ring is full: eight frames are on their way out, and the oldest is gone within a
 * frame's time (0.12 ms at 100 Mbit/s, 1.2 at 10). Waiting that long costs less than the
 * frame: a dropped TCP segment is sent again only after a second or so. Not without the
 * cycle counter (uart485_init starts it) or a link. */
static void tx_wait(void)
{
    if (!(DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) || !g_eth.link_up) return;
    uint32_t t0 = DWT->CYCCNT, max = SystemCoreClock / 1000000u * (g_eth.speed100 ? 250u : 1500u);
    while (!eth_tx_room() && DWT->CYCCNT - t0 < max) { }
}
#endif

static err_t link_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len > ETH_FRAME_MAX) return ERR_IF;
#ifdef APP_IMAGE
    if (!eth_tx_room()) tx_wait();
#endif
    uint8_t *b = eth_tx_alloc();
    if (!b) return ERR_MEM;
    pbuf_copy_partial(p, b, p->tot_len, 0);
    eth_tx_send(p->tot_len);
    return ERR_OK;
}

static err_t if_init(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = '0';
    n->output = etharp_output;
    n->linkoutput = link_output;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, g_mac, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
               NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    return ERR_OK;
}

void net_init(void)
{
    /* Locally administered MAC derived from the 96-bit chip UID. */
    const uint32_t *uid = (const uint32_t *)UID_BASE;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 12; i++)
        h = (h ^ ((const uint8_t *)uid)[i]) * 16777619u;
    g_mac[0] = 0x02; g_mac[1] = 0x54;
    g_mac[2] = (uint8_t)(h >> 24); g_mac[3] = (uint8_t)(h >> 16);
    g_mac[4] = (uint8_t)(h >> 8);  g_mac[5] = (uint8_t)h;
    snprintf(g_hostname, sizeof g_hostname, "n540-%02x%02x%02x", g_mac[3], g_mac[4], g_mac[5]);
    s_rand = h;

    rng_init();
    eth_init(g_mac);

    lwip_init();
    netif_add(&s_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, if_init, ethernet_input);
    netif_set_hostname(&s_if, g_hostname);
    netif_set_default(&s_if);
    netif_set_up(&s_if);

    mdns_resp_init();
    mdns_resp_add_netif(&s_if, g_hostname);
    mdns_resp_add_service(&s_if, g_hostname, "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
    dhcp_start(&s_if);
}

void net_poll(void)
{
    static uint32_t last_link;
    uint32_t len;

#ifdef APP_IMAGE
    uint8_t *b;
    uint32_t k;
    for (int budget = 8; budget && (b = eth_rx_take(&len, &k)); budget--) {
        s_rxp[k].custom_free_function = rx_free;
        struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, (u16_t)len, PBUF_REF, &s_rxp[k], b, ETH_FRAME_MAX);
        if (!p) { eth_rx_give(k); continue; }
        struct netif *to = ipalias_input_netif(p);           /* ARP to or from an extra address: its own */
        if (!to) to = &s_if;
        if (to->input(p, to) != ERR_OK) pbuf_free(p);
    }
    if (!eth_rx_spare()) pbuf_free_ooseq_pending = 1;  /* as lwIP does when its pool runs dry */
#else
    const uint8_t *f;
    for (int budget = 8; budget && (f = eth_rx_peek(&len)); budget--) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p) pbuf_take(p, f, (u16_t)len);
        eth_rx_release();
        if (p && s_if.input(p, &s_if) != ERR_OK) pbuf_free(p);
    }
#endif

    if (HAL_GetTick() - last_link >= 200) {
        last_link = HAL_GetTick();
        bool up = eth_poll_link();
        if (up && !netif_is_link_up(&s_if)) {
            netif_set_link_up(&s_if);
#ifdef APP_IMAGE
            ipalias_link(1);
#endif
        } else if (!up && netif_is_link_up(&s_if)) {
            netif_set_link_down(&s_if);
#ifdef APP_IMAGE
            ipalias_link(0);
#endif
        }
    }
    sys_check_timeouts();
}

void net_ip_str(char out[16])
{
    ip4addr_ntoa_r(netif_ip4_addr(&s_if), out, 16);
}

void net_addrs(char ip[16], char mask[16], char gw[16])
{
    ip4addr_ntoa_r(netif_ip4_addr(&s_if), ip, 16);
    ip4addr_ntoa_r(netif_ip4_netmask(&s_if), mask, 16);
    ip4addr_ntoa_r(netif_ip4_gw(&s_if), gw, 16);
}

void net_set(bool dhcp, uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns)
{
    ip4_addr_t a = { ip }, m = { mask }, g = { gw };
    dhcp_release_and_stop(&s_if);
    autoip_stop(&s_if);
    if (dhcp) {
        netif_set_addr(&s_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        dhcp_start(&s_if);
        return;
    }
    netif_set_addr(&s_if, &a, &m, &g);
#if LWIP_DNS
    ip_addr_t d;
    ip_addr_set_ip4_u32(&d, dns ? dns : gw);
    dns_setserver(0, &d);
#else
    (void)dns;
#endif
}

#ifdef APP_IMAGE
struct netif *net_netif(void) { return &s_if; }
#endif
