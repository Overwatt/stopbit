/*
 * uart485.h — Interrupt-driven UARTs for the four RS485 ports.
 * Direction control: the UART drives each transceiver's DE pin (high = transmit)
 * with its hardware driver-enable feature; see HARDWARE.md.
 */
#ifndef UART485_H
#define UART485_H
#include "cfg.h"

typedef struct {
    uint32_t rx_bytes, tx_bytes;
    uint32_t parity, framing, noise, overrun;   /* line errors */
    uint32_t rx_lost;                           /* RX ring full */
    uint32_t echo_dropped;
} uart_stats_t;

extern volatile uart_stats_t g_uart[PORTS];

void   uart485_init(void);                      /* once, after eth_init (D2 SRAM) */
void   uart485_open(int p, const port_cfg_t *c);
void   uart485_close(int p);
size_t uart485_rx_avail(int p);
size_t uart485_read(int p, uint8_t *buf, size_t n);
uint32_t uart485_rx_idle_us(int p);             /* since the last received byte */
size_t uart485_tx_room(int p);
size_t uart485_write(int p, const uint8_t *buf, size_t n);
bool   uart485_tx_idle(int p);                  /* ring and shifter empty */
#endif
