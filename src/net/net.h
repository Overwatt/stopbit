/* net.h — lwIP glue: one netif on eth_mac, DHCP with link-local fallback, mDNS. */
#ifndef NET_H
#define NET_H
#include <stdint.h>
#include <stdbool.h>

extern uint8_t g_mac[6];
extern char    g_hostname[16];   /* "n540-xxxxxx" */

void net_init(void);
void net_poll(void);
void net_ip_str(char out[16]);
void net_addrs(char ip[16], char mask[16], char gw[16]);
/* Switch addressing at run time. Addresses in network byte order. */
void net_set(bool dhcp, uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
/* Hardware RNG. Returns false (and writes nothing useful) if it has failed. */
bool rng_fill(void *dst, uint32_t len);
#endif
