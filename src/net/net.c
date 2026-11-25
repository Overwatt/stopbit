#include "net.h"
#include "eth_mac.h"
#include "stm32h7xx_hal.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/apps/mdns.h"
#include "netif/ethernet.h"
#include <stdio.h>
#include <string.h>

uint8_t g_mac[6];
char    g_hostname[16];
static struct netif s_if;
static uint32_t s_rand;

u32_t sys_now(void) { return HAL_GetTick(); }

/* Readable over SWD after a failure. */
const char *volatile g_assert_msg;

void lwip_assert_fail(const char *msg)
{
    g_assert_msg = msg;
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
static err_t link_output(struct netif *n, struct pbuf *p)
{
    (void)n;
    if (p->tot_len > ETH_FRAME_MAX) return ERR_IF;
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
    const uint8_t *f;
    uint32_t len;

    for (int budget = 8; budget && (f = eth_rx_peek(&len)); budget--) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (p) pbuf_take(p, f, (u16_t)len);
        eth_rx_release();
        if (p && s_if.input(p, &s_if) != ERR_OK) pbuf_free(p);
    }

    if (HAL_GetTick() - last_link >= 200) {
        last_link = HAL_GetTick();
        bool up = eth_poll_link();
        if (up && !netif_is_link_up(&s_if)) netif_set_link_up(&s_if);
        else if (!up && netif_is_link_up(&s_if)) netif_set_link_down(&s_if);
    }
    sys_check_timeouts();
}

void net_ip_str(char out[16])
{
    ip4addr_ntoa_r(netif_ip4_addr(&s_if), out, 16);
}
