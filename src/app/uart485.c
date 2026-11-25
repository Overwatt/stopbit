#include "uart485.h"
#include "board.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#define RX_SZ 8192u             /* powers of two */
#define TX_SZ 4096u
#define D2 __attribute__((section(".d2_bss")))

typedef struct {
    const char *name;
    USART_TypeDef *u;
    IRQn_Type irq;
    GPIO_TypeDef *txp, *rxp, *dep;
    uint16_t txn, rxn, den;
    uint8_t af;
} hw_t;

#define HW(n, un, irqn) { un, RS485_##n##_UART, irqn, RS485_##n##_TX_PORT, RS485_##n##_RX_PORT, \
                      RS485_##n##_DE_PORT, RS485_##n##_TX_PIN, RS485_##n##_RX_PIN, \
                      RS485_##n##_DE_PIN, RS485_##n##_AF }
static const hw_t H[PORTS] = {
    HW(1, "USART1", USART1_IRQn), HW(2, "USART2", USART2_IRQn),
    HW(3, "USART3", USART3_IRQn), HW(4, "UART4", UART4_IRQn),
};

static D2 uint8_t s_rx[PORTS][RX_SZ];
static D2 uint8_t s_tx[PORTS][TX_SZ];
static struct {
    volatile uint32_t rh, rt, th, tt;   /* free-running head/tail */
    volatile uint32_t last_rx;          /* CYCCNT */
    volatile bool tx_active;
    bool open, echo_cancel;
    uint8_t mask;
} S[PORTS];
volatile uart_stats_t g_uart[PORTS];

static void isr(int p)
{
    USART_TypeDef *u = H[p].u;
    uint32_t f = u->ISR;
    volatile uart_stats_t *st = &g_uart[p];

    if (f & (USART_ISR_PE | USART_ISR_FE | USART_ISR_NE | USART_ISR_ORE)) {
        if (f & USART_ISR_PE) st->parity++;
        if (f & USART_ISR_FE) st->framing++;
        if (f & USART_ISR_NE) st->noise++;
        if (f & USART_ISR_ORE) st->overrun++;
        u->ICR = USART_ICR_PECF | USART_ICR_FECF | USART_ICR_NECF | USART_ICR_ORECF;
    }
    while (u->ISR & USART_ISR_RXNE_RXFNE) {
        uint8_t b = (uint8_t)u->RDR & S[p].mask;
        S[p].last_rx = DWT->CYCCNT;
        if (S[p].echo_cancel && S[p].tx_active) { st->echo_dropped++; continue; }
        st->rx_bytes++;
        if (S[p].rh - S[p].rt >= RX_SZ) { st->rx_lost++; continue; }
        s_rx[p][S[p].rh & (RX_SZ - 1)] = b;
        __DMB();
        S[p].rh++;
    }
    if (u->CR1 & USART_CR1_TXEIE_TXFNFIE) {
        while ((u->ISR & USART_ISR_TXE_TXFNF) && S[p].th != S[p].tt) {
            u->TDR = s_tx[p][S[p].tt & (TX_SZ - 1)];
            S[p].tt++;
            st->tx_bytes++;
        }
        if (S[p].th == S[p].tt) {
            u->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
            u->CR1 |= USART_CR1_TCIE;
        }
    } else if ((u->CR1 & USART_CR1_TCIE) && (u->ISR & USART_ISR_TC)) {
        u->CR1 &= ~USART_CR1_TCIE;
        S[p].tx_active = false;
    }
}

void USART1_IRQHandler(void) { isr(0); }
void USART2_IRQHandler(void) { isr(1); }
void USART3_IRQHandler(void) { isr(2); }
void UART4_IRQHandler(void)  { isr(3); }

void uart485_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_D2SRAM1EN | RCC_AHB2ENR_D2SRAM2EN | RCC_AHB2ENR_D2SRAM3EN;
    (void)RCC->AHB2ENR;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    RS485_1_UART_CLK_EN();
    RS485_2_UART_CLK_EN();
    RS485_3_UART_CLK_EN();
    RS485_4_UART_CLK_EN();
    memset((void *)g_uart, 0, sizeof g_uart);
}

/* Driver off (receive): DE as a plain output, low. */
static void de_idle(int p)
{
    GPIO_InitTypeDef g = { .Pin = H[p].den, .Mode = GPIO_MODE_OUTPUT_PP,
                           .Speed = GPIO_SPEED_FREQ_LOW };
    H[p].dep->BSRR = (uint32_t)H[p].den << 16;
    HAL_GPIO_Init(H[p].dep, &g);
}

void uart485_close(int p)
{
    GPIO_InitTypeDef g = { .Mode = GPIO_MODE_ANALOG };
    NVIC_DisableIRQ(H[p].irq);
    de_idle(p);             /* before TX floats */
    H[p].u->CR1 = 0;
    S[p].open = false;
    S[p].tx_active = false;
    S[p].rh = S[p].rt = S[p].th = S[p].tt = 0;
    g.Pin = H[p].txn; HAL_GPIO_Init(H[p].txp, &g);
    g.Pin = H[p].rxn; HAL_GPIO_Init(H[p].rxp, &g);
}

void uart485_open(int p, const port_cfg_t *c)
{
    static const uint16_t DIV[] = { 1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256 };
    USART_TypeDef *u = H[p].u;
    GPIO_InitTypeDef g = { .Mode = GPIO_MODE_AF_PP, .Pull = GPIO_PULLUP,
                           .Speed = GPIO_SPEED_FREQ_MEDIUM, .Alternate = H[p].af };
    uart485_close(p);
    NVIC_ClearPendingIRQ(H[p].irq);

    uint32_t clk = p == 0 ? HAL_RCC_GetPCLK2Freq() : HAL_RCC_GetPCLK1Freq(), presc = 0, brr;
    while ((brr = (clk / DIV[presc] + c->baud / 2) / c->baud) > 0xFFFFu) presc++;

    /* Word length counts the parity bit. */
    int bits = c->data_bits + (c->parity != PARITY_NONE);
    /* The UART raises DE one bit time (16/16) before a burst and drops it 4/16
     * after the last stop bit, so replies can follow straight away. */
    uint32_t cr1 = USART_CR1_FIFOEN | (16u << USART_CR1_DEAT_Pos) | (4u << USART_CR1_DEDT_Pos);
    if (bits == 9) cr1 |= USART_CR1_M0;
    if (bits == 7) cr1 |= USART_CR1_M1;
    if (c->parity != PARITY_NONE) cr1 |= USART_CR1_PCE;
    if (c->parity == PARITY_ODD) cr1 |= USART_CR1_PS;

    u->CR1 = cr1;
    u->CR2 = c->stop_bits == 2 ? USART_CR2_STOP_1 : 0;
    u->CR3 = USART_CR3_DEM | (c->loopback ? USART_CR3_HDSEL : 0);   /* DE active high */
    u->PRESC = presc;
    u->BRR = brr;
    u->ICR = 0xFFFFFFFFu;
    S[p].mask = c->data_bits == 7 ? 0x7F : 0xFF;
    S[p].echo_cancel = c->echo_cancel && !c->loopback;
    S[p].last_rx = DWT->CYCCNT;

    u->CR1 |= USART_CR1_UE;
    u->CR1 |= USART_CR1_TE | USART_CR1_RE;
    /* TE drives TX idle-high before the pin is handed over. */
    g.Pin = H[p].txn; HAL_GPIO_Init(H[p].txp, &g);
    g.Pin = H[p].den; g.Pull = GPIO_NOPULL; HAL_GPIO_Init(H[p].dep, &g);
    g.Pull = GPIO_PULLUP;
    g.Pin = H[p].rxn; HAL_GPIO_Init(H[p].rxp, &g);
    u->RQR = USART_RQR_RXFRQ;
    u->ICR = 0xFFFFFFFFu;
    S[p].open = true;
    u->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
    u->CR3 |= USART_CR3_EIE;
    NVIC_SetPriority(H[p].irq, 5);
    NVIC_EnableIRQ(H[p].irq);
}

static void pin_name(char *b, GPIO_TypeDef *port, uint16_t pin)
{
    snprintf(b, 8, "P%c%d", (char)('A' + (((uint32_t)port - GPIOA_BASE) / 0x400u) % 26u),
             __builtin_ctz(pin) & 15);
}

const char *uart485_pins(int p, char *tx, char *rx, char *de)
{
    pin_name(tx, H[p].txp, H[p].txn);
    pin_name(rx, H[p].rxp, H[p].rxn);
    pin_name(de, H[p].dep, H[p].den);
    return H[p].name;
}

size_t uart485_rx_avail(int p) { return S[p].rh - S[p].rt; }

size_t uart485_read(int p, uint8_t *buf, size_t n)
{
    size_t a = uart485_rx_avail(p);
    if (n > a) n = a;
    for (size_t i = 0; i < n; i++) buf[i] = s_rx[p][(S[p].rt + i) & (RX_SZ - 1)];
    __DMB();
    S[p].rt += n;
    return n;
}

uint32_t uart485_rx_idle_us(int p)
{
    /* Order matters: if a byte lands between the reads, the timestamp must be the
     * older of the two, or the difference wraps and looks like a long silence. */
    uint32_t last = S[p].last_rx;
    __DMB();
    return (DWT->CYCCNT - last) / (SystemCoreClock / 1000000u);
}

size_t uart485_tx_room(int p) { return S[p].open ? TX_SZ - (S[p].th - S[p].tt) : 0; }

size_t uart485_write(int p, const uint8_t *buf, size_t n)
{
    size_t room = uart485_tx_room(p);
    if (n > room) n = room;
    if (!n) return 0;
    for (size_t i = 0; i < n; i++) s_tx[p][(S[p].th + i) & (TX_SZ - 1)] = buf[i];
    __DMB();
    __disable_irq();
    S[p].th += n;
    S[p].tx_active = true;
    H[p].u->CR1 = (H[p].u->CR1 & ~USART_CR1_TCIE) | USART_CR1_TXEIE_TXFNFIE;
    __enable_irq();
    return n;
}

bool uart485_tx_idle(int p) { return !S[p].tx_active; }
