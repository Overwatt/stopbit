/*
 * board.h — Hardware configuration for the USR-N540 (STM32H750VBT6).
 *
 * ============================= READ THIS ===================================
 * Every pin assignment below marked TODO(dump) is a PLACEHOLDER GUESS. The
 * board has not been reverse-engineered yet. These values are here so the
 * drivers compile and the architecture is complete; they are almost
 * certainly WRONG for the real hardware.
 *
 * When we dump the firmware / buzz out the board, this is the ONE file to
 * correct. Nothing else in the codebase hard-codes a pin. Replace the
 * placeholders, rebuild, done.
 *
 * Known-good facts (from the chip markings and the pusr.com datasheet):
 *   - MCU:   STM32H750VBT6, LQFP100 package, Cortex-M7
 *   - HSE:   25 MHz crystal (visible on the board, marked "25.0")
 *   - Flash: 128 KB internal + external QSPI NOR (GigaDevice GD25Q16 = 2 MB,
 *            plus a second GD SOIC-8 chip)
 *   - 4x RS485 ports, 1x 10/100 Ethernet (external PHY, model TBD)
 * ===========================================================================
 */
#ifndef BOARD_H
#define BOARD_H

#include "stm32h7xx_hal.h"

/* ---- Clock ---------------------------------------------------------------
 * HSE is the 25 MHz crystal. Target SYSCLK is 400 MHz (the operating point
 * pusr.com lists for this product). PLL math lives in system_clock.c.
 */
#define BOARD_HSE_VALUE_HZ      25000000U
#define BOARD_SYSCLK_HZ         400000000U

/* ---- Status LED(s) -------------------------------------------------------
 * TODO(dump): real LED port/pin unknown. The board has several LEDs
 * (power, link, per-port activity). This is the "heartbeat" LED.
 */
#define BOARD_LED_PORT          GPIOE        /* TODO(dump) */
#define BOARD_LED_PIN           GPIO_PIN_2   /* TODO(dump) */
#define BOARD_LED_CLK_ENABLE()  __HAL_RCC_GPIOE_CLK_ENABLE()   /* TODO(dump) */
#define BOARD_LED_ACTIVE_HIGH   1            /* TODO(dump): is the LED active-high? */

/* ---- RS485 ports ---------------------------------------------------------
 * Four half-duplex RS485 ports. Each is a USART/UART plus a GPIO "driver
 * enable" (DE) line: drive DE high to transmit, low to receive.
 *
 * Per port we need: the UART peripheral, its TX/RX pins + alternate-function
 * number, and the DE GPIO. All TODO(dump) until reverse-engineered. The pins
 * chosen here deliberately avoid the standard RMII Ethernet pins (see below)
 * so the placeholder config is at least internally consistent.
 */
#define BOARD_RS485_PORT_COUNT  4

/* --- Port 1 --- TODO(dump) */
#define RS485_1_UART            USART1
#define RS485_1_UART_CLK_EN()   __HAL_RCC_USART1_CLK_ENABLE()
#define RS485_1_TX_PORT         GPIOA
#define RS485_1_TX_PIN          GPIO_PIN_9
#define RS485_1_RX_PORT         GPIOA
#define RS485_1_RX_PIN          GPIO_PIN_10
#define RS485_1_AF              GPIO_AF7_USART1
#define RS485_1_DE_PORT         GPIOA
#define RS485_1_DE_PIN          GPIO_PIN_8

/* --- Port 2 --- TODO(dump) */
#define RS485_2_UART            USART3
#define RS485_2_UART_CLK_EN()   __HAL_RCC_USART3_CLK_ENABLE()
#define RS485_2_TX_PORT         GPIOD
#define RS485_2_TX_PIN          GPIO_PIN_8
#define RS485_2_RX_PORT         GPIOD
#define RS485_2_RX_PIN          GPIO_PIN_9
#define RS485_2_AF              GPIO_AF7_USART3
#define RS485_2_DE_PORT         GPIOD
#define RS485_2_DE_PIN          GPIO_PIN_10

/* --- Port 3 --- TODO(dump) */
#define RS485_3_UART            UART4
#define RS485_3_UART_CLK_EN()   __HAL_RCC_UART4_CLK_ENABLE()
#define RS485_3_TX_PORT         GPIOC
#define RS485_3_TX_PIN          GPIO_PIN_10
#define RS485_3_RX_PORT         GPIOC
#define RS485_3_RX_PIN          GPIO_PIN_11
#define RS485_3_AF              GPIO_AF8_UART4
#define RS485_3_DE_PORT         GPIOA
#define RS485_3_DE_PIN          GPIO_PIN_15

/* --- Port 4 --- TODO(dump) */
#define RS485_4_UART            USART6
#define RS485_4_UART_CLK_EN()   __HAL_RCC_USART6_CLK_ENABLE()
#define RS485_4_TX_PORT         GPIOG
#define RS485_4_TX_PIN          GPIO_PIN_14
#define RS485_4_RX_PORT         GPIOG
#define RS485_4_RX_PIN          GPIO_PIN_9
#define RS485_4_AF              GPIO_AF7_USART6
#define RS485_4_DE_PORT         GPIOG
#define RS485_4_DE_PIN          GPIO_PIN_12

/* Default serial parameters for each RS485 port (changeable at runtime). */
#define RS485_DEFAULT_BAUD      9600U
#define RS485_DEFAULT_WORDLEN   UART_WORDLENGTH_8B
#define RS485_DEFAULT_STOPBITS  UART_STOPBITS_1
#define RS485_DEFAULT_PARITY    UART_PARITY_NONE

/* ---- Ethernet (RMII) -----------------------------------------------------
 * The H750 has a MAC but no PHY; there is an external 10/100 PHY on the
 * board (model TBD — commonly LAN8720A, RTL8201, or DP83848). The pins below
 * are the STANDARD RMII mapping on AF11; TODO(dump) to confirm against the
 * real routing, and set the PHY address + model once identified.
 */
#define BOARD_ETH_PHY_ADDRESS   0x00          /* TODO(dump): often 0 or 1 */
/* TODO(dump): PHY reset line, if any */
#define BOARD_ETH_PHY_RST_PORT  GPIOx_TBD
#define BOARD_ETH_PHY_RST_PIN   0

/* RMII signal pins (AF11). TODO(dump): confirm each. */
/* REF_CLK=PA1, MDIO=PA2, CRS_DV=PA7, MDC=PC1, RXD0=PC4, RXD1=PC5,
 * TX_EN=PG11, TXD0=PG13, TXD1=PG12 — standard, to be verified.          */

#endif /* BOARD_H */
