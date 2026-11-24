/*
 * rs485.c — Four half-duplex RS485 ports on the STM32H750.
 *
 * Direction control is done by manually toggling a GPIO DE line (rather than
 * the USART hardware DE function) because on this board the DE pin may not sit
 * on the UART's native RTS/DE pin. board.h is the single source of truth for
 * every pin — see the TODO(dump) markers there.
 *
 * Milestone 1 is polling-mode. Interrupt/DMA-driven RX with ring buffers comes
 * with the TCP bridge milestone, when throughput matters.
 */
#include "stm32h7xx_hal.h"
#include "board.h"
#include "rs485.h"

typedef struct {
    USART_TypeDef *uart;
    GPIO_TypeDef  *tx_port;  uint16_t tx_pin;
    GPIO_TypeDef  *rx_port;  uint16_t rx_pin;
    uint8_t        af;
    GPIO_TypeDef  *de_port;  uint16_t de_pin;
} rs485_pins_t;

/* Build the port table straight from board.h. */
static const rs485_pins_t s_pins[RS485_PORT_COUNT] = {
    { RS485_1_UART, RS485_1_TX_PORT, RS485_1_TX_PIN, RS485_1_RX_PORT, RS485_1_RX_PIN,
      RS485_1_AF, RS485_1_DE_PORT, RS485_1_DE_PIN },
    { RS485_2_UART, RS485_2_TX_PORT, RS485_2_TX_PIN, RS485_2_RX_PORT, RS485_2_RX_PIN,
      RS485_2_AF, RS485_2_DE_PORT, RS485_2_DE_PIN },
    { RS485_3_UART, RS485_3_TX_PORT, RS485_3_TX_PIN, RS485_3_RX_PORT, RS485_3_RX_PIN,
      RS485_3_AF, RS485_3_DE_PORT, RS485_3_DE_PIN },
    { RS485_4_UART, RS485_4_TX_PORT, RS485_4_TX_PIN, RS485_4_RX_PORT, RS485_4_RX_PIN,
      RS485_4_AF, RS485_4_DE_PORT, RS485_4_DE_PIN },
};

static UART_HandleTypeDef s_uart[RS485_PORT_COUNT];

static void enable_all_gpio_clocks(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
}

static void enable_uart_clock(unsigned port)
{
    switch (port) {
        case 0: RS485_1_UART_CLK_EN(); break;
        case 1: RS485_2_UART_CLK_EN(); break;
        case 2: RS485_3_UART_CLK_EN(); break;
        case 3: RS485_4_UART_CLK_EN(); break;
        default: break;
    }
}

static void de_receive(unsigned port)   /* DE low = listen */
{
    HAL_GPIO_WritePin(s_pins[port].de_port, s_pins[port].de_pin, GPIO_PIN_RESET);
}

static void de_transmit(unsigned port)  /* DE high = drive the bus */
{
    HAL_GPIO_WritePin(s_pins[port].de_port, s_pins[port].de_pin, GPIO_PIN_SET);
}

static void init_gpio(unsigned port)
{
    const rs485_pins_t *p = &s_pins[port];
    GPIO_InitTypeDef g = {0};

    /* TX + RX as UART alternate function. */
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = p->af;
    g.Pin = p->tx_pin; HAL_GPIO_Init(p->tx_port, &g);
    g.Pin = p->rx_pin; HAL_GPIO_Init(p->rx_port, &g);

    /* DE as push-pull output, default to receive. */
    GPIO_InitTypeDef d = {0};
    d.Mode  = GPIO_MODE_OUTPUT_PP;
    d.Pull  = GPIO_NOPULL;
    d.Speed = GPIO_SPEED_FREQ_HIGH;
    d.Pin   = p->de_pin;
    HAL_GPIO_Init(p->de_port, &d);
    de_receive(port);
}

static int init_uart(unsigned port, uint32_t baud, uint32_t wordlen,
                     uint32_t stopbits, uint32_t parity)
{
    UART_HandleTypeDef *h = &s_uart[port];
    h->Instance                    = s_pins[port].uart;
    h->Init.BaudRate               = baud;
    h->Init.WordLength             = wordlen;
    h->Init.StopBits               = stopbits;
    h->Init.Parity                 = parity;
    h->Init.Mode                   = UART_MODE_TX_RX;
    h->Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    h->Init.OverSampling           = UART_OVERSAMPLING_16;
    h->Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    h->Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    h->FifoMode                    = UART_FIFOMODE_DISABLE;
    h->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    return (HAL_UART_Init(h) == HAL_OK) ? 0 : -1;
}

void rs485_init_all(void)
{
    enable_all_gpio_clocks();
    for (unsigned port = 0; port < RS485_PORT_COUNT; ++port) {
        enable_uart_clock(port);
        init_gpio(port);
        init_uart(port, RS485_DEFAULT_BAUD, RS485_DEFAULT_WORDLEN,
                  RS485_DEFAULT_STOPBITS, RS485_DEFAULT_PARITY);
    }
}

int rs485_configure(unsigned port, uint32_t baud, uint32_t wordlen,
                    uint32_t stopbits, uint32_t parity)
{
    if (port >= RS485_PORT_COUNT) return -1;
    HAL_UART_DeInit(&s_uart[port]);
    int rc = init_uart(port, baud, wordlen, stopbits, parity);
    de_receive(port);
    return rc;
}

int rs485_send(unsigned port, const uint8_t *data, size_t len)
{
    if (port >= RS485_PORT_COUNT || (data == NULL && len)) return -1;
    UART_HandleTypeDef *h = &s_uart[port];

    de_transmit(port);
    for (size_t i = 0; i < len; ++i) {
        while (!__HAL_UART_GET_FLAG(h, UART_FLAG_TXE)) { }
        h->Instance->TDR = data[i];
    }
    /* Wait for the final byte to fully shift out before releasing the bus,
     * otherwise we truncate the last character on the wire. */
    while (!__HAL_UART_GET_FLAG(h, UART_FLAG_TC)) { }
    de_receive(port);
    return 0;
}

int rs485_poll_byte(unsigned port, uint8_t *out)
{
    if (port >= RS485_PORT_COUNT || out == NULL) return 0;
    UART_HandleTypeDef *h = &s_uart[port];
    if (__HAL_UART_GET_FLAG(h, UART_FLAG_RXNE)) {
        *out = (uint8_t)(h->Instance->RDR & 0xFF);
        return 1;
    }
    return 0;
}

void rs485_service_echo(void)
{
    for (unsigned port = 0; port < RS485_PORT_COUNT; ++port) {
        uint8_t b;
        if (rs485_poll_byte(port, &b)) {
            rs485_send(port, &b, 1);
        }
    }
}
