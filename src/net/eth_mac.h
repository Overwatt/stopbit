/*
 * eth_mac.h — Polled driver for the STM32H7 Ethernet MAC in RMII mode.
 * No interrupts, no RTOS, no HAL: the main loop drains the RX ring.
 */
#ifndef ETH_MAC_H
#define ETH_MAC_H

#include <stdint.h>
#include <stdbool.h>

#define ETH_FRAME_MAX 1536u

typedef enum {
    ETH_OK = 0,
    ETH_ERR_DMA_RESET = 1,   /* no 50 MHz REF_CLK from the PHY, usually */
    ETH_ERR_NO_PHY = 2,      /* nothing answered on MDIO */
} eth_status_t;

/* Kept in one struct so it can be read over SWD as well as via /status. */
typedef struct {
    uint32_t status;         /* eth_status_t */
    uint32_t phy_addr;       /* 0..31, or 0xFF if none */
    uint32_t phy_id;         /* (PHYID1 << 16) | PHYID2 */
    uint32_t bmsr, anlpar;
    uint32_t link_up, speed100, full_duplex;
    uint32_t rx_frames, tx_frames, rx_errors, tx_drops;
} eth_info_t;

extern eth_info_t g_eth;

eth_status_t eth_init(const uint8_t mac[6]);
void eth_stop(void);

/* Re-read PHY state; applies speed/duplex on change. Returns link state. */
bool eth_poll_link(void);

#ifdef APP_IMAGE
/* Next received frame, or NULL. Its buffer (*buf) is the caller's until given back;
 * meanwhile a spare takes its place for the DMA. With no spares left, frames wait in
 * the descriptors that still have a buffer, then the MAC drops them. */
#define ETH_RX_BUFS 18u
uint8_t *eth_rx_take(uint32_t *len, uint32_t *buf);
void eth_rx_give(uint32_t buf);
uint32_t eth_rx_spare(void);
/* Buffers lent now and at most; how often a descriptor was left without one. */
void eth_rx_stats(uint32_t *lent, uint32_t *peak, uint32_t *starved);
#else
/* Next received frame, or NULL. Valid until eth_rx_release(). */
const uint8_t *eth_rx_peek(uint32_t *len);
void eth_rx_release(void);
#endif

/* Free TX buffer (ETH_FRAME_MAX bytes), or NULL if the ring is full. */
uint8_t *eth_tx_alloc(void);
void eth_tx_send(uint32_t len);

bool mdio_read(uint32_t phy, uint32_t reg, uint16_t *val);
bool mdio_write(uint32_t phy, uint32_t reg, uint16_t val);

#endif
