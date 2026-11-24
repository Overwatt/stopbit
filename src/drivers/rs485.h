/*
 * rs485.h — Four half-duplex RS485 ports.
 *
 * Each port is a UART plus a GPIO "driver enable" (DE) line. All pin/peripheral
 * assignments come from board.h; this driver contains no hard-coded pins.
 */
#ifndef RS485_H
#define RS485_H

#include <stdint.h>
#include <stddef.h>
#include "board.h"

#define RS485_PORT_COUNT  BOARD_RS485_PORT_COUNT

/* Initialise all RS485 ports with their board.h default serial parameters. */
void rs485_init_all(void);

/* Reconfigure one port's line settings at runtime. Returns 0 on success. */
int rs485_configure(unsigned port, uint32_t baud, uint32_t wordlen,
                    uint32_t stopbits, uint32_t parity);

/* Blocking transmit on one port (drives DE high, sends, waits for the last
 * bit to leave the shift register, then releases DE). Returns 0 on success. */
int rs485_send(unsigned port, const uint8_t *data, size_t len);

/* Non-blocking: if a byte is waiting on any port, read it. Returns 1 and sets
 * out/port if a byte was available, else 0. */
int rs485_poll_byte(unsigned port, uint8_t *out);

/* Placeholder milestone-1 app: echo received bytes back out the same port. */
void rs485_service_echo(void);

#endif /* RS485_H */
