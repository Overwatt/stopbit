/*
 * ipalias.h — more IPv4 addresses for one lwIP Ethernet interface.
 *
 * lwIP gives an interface one IPv4 address. This gives the main interface up to
 * IPALIAS_MAX more, on the same link and MAC address, each checked for a conflict first
 * (lwIP's ACD, RFC 5227). It uses lwIP alone (netif, etharp, acd), under the raw API's
 * rules: NO_SYS, or with the core lock held.
 *
 * Each address has an interface of its own, with the main one's MAC address, MTU and link
 * output, up and never with its link up. lwIP's routing passes over an interface without
 * a link, so everything goes out through the main one, replies to an extra address too,
 * with that address as their source; lwIP's input already takes a packet for any
 * interface that is up. What the driver does:
 *   - each received frame goes to ipalias_input_netif(p), or the main interface if NULL;
 *   - after the main interface's link changes, ipalias_link(up).
 * Needs LWIP_ARP and LWIP_ACD. A UDP PCB bound to an extra address sends with
 * udp_sendto_if_src() and the main interface: udp_sendto() refuses a source address the
 * route's interface does not have (ERR_RTE).
 */
#ifndef IPALIAS_H
#define IPALIAS_H
#include "lwip/netif.h"
#include "lwip/prot/ethernet.h"

#define IPALIAS_MAX 3

typedef enum {
    IPALIAS_OFF,            /* no address in this place */
    IPALIAS_CHECKING,       /* probing for it, or waiting for the link to do so */
    IPALIAS_LIVE,           /* in use, and watched */
    IPALIAS_CONFLICT        /* another device has it: not used until asked again */
} ipalias_state_t;

/* A place's state changed. other: with IPALIAS_CONFLICT, the MAC address of the device
 * that has the address; else NULL. */
typedef void (*ipalias_cb)(int i, ipalias_state_t st, const struct eth_addr *other);

void ipalias_init(struct netif *main, ipalias_cb cb);
/* The addresses wanted, at most IPALIAS_MAX (more are ignored, as are 0 and repeats). One
 * no longer listed is given up; a new one is checked; one kept stays as it is, except one
 * in conflict, which is checked again. */
void ipalias_set(const ip4_addr_t *addr, int n);
/* The main interface's link has changed (call after netif_set_link_up/down). Up: an
 * address in use is announced again; one not in use is checked again. Down: checks stop. */
void ipalias_link(int up);
/* The interface a received frame belongs to: an extra address's for an ARP frame to or
 * from that address; NULL for the main interface (everything else). */
struct netif *ipalias_input_netif(const struct pbuf *p);
/* Place i (0 to IPALIAS_MAX - 1): its state, and its address if not IPALIAS_OFF. */
ipalias_state_t ipalias_state(int i, ip4_addr_t *addr);
#endif
