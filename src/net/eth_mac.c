/*
 * eth_mac.c — Polled STM32H7 Ethernet (RM0433 ch. 58) + generic RMII PHY.
 *
 * Descriptors and buffers live in D2 SRAM (.dma_d2), which an MPU region marks
 * non-cacheable so this keeps working if the application enables the D-cache.
 *
 * Both tail pointers are parked one past the ring end. The DMA therefore stops
 * only when it meets a descriptor it does not own, and rewriting a tail pointer
 * acts as a plain "poll demand".
 */
#include "eth_mac.h"
#include "board.h"
#include <string.h>

#define RX_COUNT 12u
#define TX_COUNT 8u

typedef struct { volatile uint32_t d0, d1, d2, d3; } desc_t;

#define TDES3_OWN   (1u << 31)
#define TDES3_FD    (1u << 29)
#define TDES3_LD    (1u << 28)
#define RDES3_OWN   (1u << 31)
#define RDES3_CTXT  (1u << 30)
#define RDES3_FD    (1u << 29)
#define RDES3_LD    (1u << 28)
#define RDES3_BUF1V (1u << 24)
#define RDES3_ES    (1u << 15)
#define RDES3_PL    0x7FFFu

#define DMA_MEM __attribute__((section(".dma_d2"), aligned(32)))
static DMA_MEM desc_t  s_rx[RX_COUNT];
static DMA_MEM desc_t  s_tx[TX_COUNT];
static DMA_MEM uint8_t s_rxbuf[RX_COUNT][ETH_FRAME_MAX];
static DMA_MEM uint8_t s_txbuf[TX_COUNT][ETH_FRAME_MAX];

static uint32_t s_rx_i, s_tx_i;
eth_info_t g_eth;

/* ---- MDIO ---------------------------------------------------------------- */
static bool mdio_wait(void)
{
    uint32_t t0 = HAL_GetTick();
    while (ETH->MACMDIOAR & ETH_MACMDIOAR_MB)
        if (HAL_GetTick() - t0 > 5) return false;
    return true;
}

static bool mdio_op(uint32_t phy, uint32_t reg, uint32_t goc)
{
    /* CR=4: MDC = HCLK/102, valid for HCLK 150-250 MHz. */
    ETH->MACMDIOAR = (phy << ETH_MACMDIOAR_PA_Pos) | (reg << ETH_MACMDIOAR_RDA_Pos) |
                     (4u << ETH_MACMDIOAR_CR_Pos) | (goc << 2) | ETH_MACMDIOAR_MB;
    return mdio_wait();
}

bool mdio_read(uint32_t phy, uint32_t reg, uint16_t *val)
{
    if (!mdio_wait() || !mdio_op(phy, reg, 3u)) return false;
    *val = (uint16_t)ETH->MACMDIODR;
    return true;
}

bool mdio_write(uint32_t phy, uint32_t reg, uint16_t val)
{
    if (!mdio_wait()) return false;
    ETH->MACMDIODR = val;
    return mdio_op(phy, reg, 1u);
}

/* ---- Pins / clocks ------------------------------------------------------- */
static void eth_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF11_ETH;
    g.Pin = BOARD_ETH_PINS_A;  HAL_GPIO_Init(GPIOA, &g);
    g.Pin = BOARD_ETH_PINS_B;  HAL_GPIO_Init(GPIOB, &g);
    g.Pin = BOARD_ETH_PINS_C;  HAL_GPIO_Init(GPIOC, &g);
}

static void mpu_dma_region(void)
{
    /* 32 KB @ 0x30000000: normal, non-cacheable, shareable, no execute. */
    ARM_MPU_Disable();
    ARM_MPU_SetRegionEx(0, 0x30000000u,
        ARM_MPU_RASR(1, ARM_MPU_AP_FULL, 1, 1, 0, 0, 0, ARM_MPU_REGION_SIZE_32KB));
    ARM_MPU_Enable(MPU_CTRL_PRIVDEFENA_Msk);
}

/* ---- Rings --------------------------------------------------------------- */
static void rx_arm(uint32_t i)
{
    s_rx[i].d0 = (uint32_t)s_rxbuf[i];
    s_rx[i].d1 = 0;
    s_rx[i].d2 = 0;
    __DMB();
    s_rx[i].d3 = RDES3_OWN | RDES3_BUF1V;
}

static void rings_init(void)
{
    memset((void *)s_tx, 0, sizeof s_tx);
    for (uint32_t i = 0; i < RX_COUNT; i++) rx_arm(i);
    s_rx_i = s_tx_i = 0;
    __DSB();

    ETH->DMACTDLAR = (uint32_t)s_tx;
    ETH->DMACTDRLR = TX_COUNT - 1;
    ETH->DMACTDTPR = (uint32_t)&s_tx[TX_COUNT];
    ETH->DMACRDLAR = (uint32_t)s_rx;
    ETH->DMACRDRLR = RX_COUNT - 1;
    ETH->DMACRDTPR = (uint32_t)&s_rx[RX_COUNT];
}

/* ---- PHY ----------------------------------------------------------------- */
static void phy_find(void)
{
    g_eth.phy_addr = 0xFF;
    g_eth.phy_id = 0;
    for (uint32_t n = 0; n < 32; n++) {
        /* Try the strap address first, then scan. */
        uint32_t a = (n + BOARD_ETH_PHY_ADDRESS) & 31u;
        uint16_t id1, id2;
        if (!mdio_read(a, 2, &id1) || !mdio_read(a, 3, &id2)) return;
        if (id1 == 0xFFFF || (id1 == 0 && id2 == 0)) continue;
        g_eth.phy_addr = a;
        g_eth.phy_id = ((uint32_t)id1 << 16) | id2;
        return;
    }
}

bool eth_poll_link(void)
{
    uint16_t bmsr, anar, lpa;
    uint32_t a = g_eth.phy_addr;
    if (a > 31) return false;

    /* BMSR link bit latches low: read twice for the current state. */
    if (!mdio_read(a, 1, &bmsr) || !mdio_read(a, 1, &bmsr)) return false;
    g_eth.bmsr = bmsr;
    bool up = (bmsr & (1u << 2)) && (bmsr & (1u << 5));
    if (up && !g_eth.link_up) {
        if (!mdio_read(a, 4, &anar) || !mdio_read(a, 5, &lpa)) return false;
        g_eth.anlpar = lpa;
        uint16_t c = anar & lpa;
        bool s100 = c & 0x0180, fd;
        fd = s100 ? (c & 0x0100) : (c & 0x0040);
        uint32_t cr = ETH->MACCR & ~(ETH_MACCR_FES | ETH_MACCR_DM);
        ETH->MACCR = cr | (s100 ? ETH_MACCR_FES : 0) | (fd ? ETH_MACCR_DM : 0);
        g_eth.speed100 = s100;
        g_eth.full_duplex = fd;
    }
    g_eth.link_up = up;
    return up;
}

/* ---- Init ---------------------------------------------------------------- */
eth_status_t eth_init(const uint8_t mac[6])
{
    memset(&g_eth, 0, sizeof g_eth);
    g_eth.phy_addr = 0xFF;

    RCC->AHB2ENR |= RCC_AHB2ENR_D2SRAM1EN | RCC_AHB2ENR_D2SRAM2EN | RCC_AHB2ENR_D2SRAM3EN;
    (void)RCC->AHB2ENR;
    mpu_dma_region();
    eth_gpio_init();

    /* RMII must be selected while the MAC is in reset, before its clocks run. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    RCC->AHB1RSTR |= RCC_AHB1RSTR_ETH1MACRST;
    MODIFY_REG(SYSCFG->PMCR, SYSCFG_PMCR_EPIS_SEL, SYSCFG_PMCR_EPIS_SEL_2);
    (void)SYSCFG->PMCR;
    RCC->AHB1ENR |= RCC_AHB1ENR_ETH1MACEN | RCC_AHB1ENR_ETH1TXEN | RCC_AHB1ENR_ETH1RXEN;
    (void)RCC->AHB1ENR;
    HAL_Delay(1);
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETH1MACRST;
    HAL_Delay(1);

    /* MDIO only needs HCLK, so identify the PHY even when REF_CLK is missing:
     * that distinguishes "no PHY" from "PHY but no clock". */
    phy_find();

    ETH->DMAMR |= ETH_DMAMR_SWR;
    uint32_t t0 = HAL_GetTick();
    while (ETH->DMAMR & ETH_DMAMR_SWR) {
        if (HAL_GetTick() - t0 > 500)
            return g_eth.status = ETH_ERR_DMA_RESET, ETH_ERR_DMA_RESET;
    }

    /* MAC: strip CRC/padding; filter on our address + broadcast + multicast. */
    ETH->MACA0HR = ((uint32_t)mac[5] << 8) | mac[4];
    ETH->MACA0LR = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
                   ((uint32_t)mac[1] << 8) | mac[0];
    ETH->MACPFR = ETH_MACPFR_PM;
    ETH->MACCR = ETH_MACCR_CST | ETH_MACCR_ACS | ETH_MACCR_FES | ETH_MACCR_DM;
    ETH->MMCRIMR = 0xFFFFFFFFu;
    ETH->MMCTIMR = 0xFFFFFFFFu;

    /* MTL: store-and-forward both ways. */
    ETH->MTLTQOMR |= ETH_MTLTQOMR_TSF;
    ETH->MTLRQOMR |= ETH_MTLRQOMR_RSF;

    /* DMA: aligned bursts, contiguous 16-byte descriptors (DSL = 0). */
    ETH->DMASBMR = ETH_DMASBMR_AAL | ETH_DMASBMR_FB;
    ETH->DMACCR = 0;
    ETH->DMACTCR = 32u << 16;                              /* TPBL */
    ETH->DMACRCR = (32u << 16) | (ETH_FRAME_MAX << 1);     /* RPBL, RBSZ */
    rings_init();

    ETH->DMACTCR |= ETH_DMACTCR_ST;
    ETH->DMACRCR |= ETH_DMACRCR_SR;
    ETH->MACCR |= ETH_MACCR_TE | ETH_MACCR_RE;

    if (g_eth.phy_addr > 31)
        return g_eth.status = ETH_ERR_NO_PHY, ETH_ERR_NO_PHY;

    /* Advertise everything and restart autonegotiation. */
    mdio_write(g_eth.phy_addr, 4, 0x01E1);
    mdio_write(g_eth.phy_addr, 0, 0x1200);
    return ETH_OK;
}

void eth_stop(void)
{
    ETH->MACCR &= ~(ETH_MACCR_TE | ETH_MACCR_RE);
    ETH->DMACTCR &= ~ETH_DMACTCR_ST;
    ETH->DMACRCR &= ~ETH_DMACRCR_SR;
    HAL_Delay(1);
    RCC->AHB1RSTR |= RCC_AHB1RSTR_ETH1MACRST;
    (void)RCC->AHB1RSTR;
    RCC->AHB1RSTR &= ~RCC_AHB1RSTR_ETH1MACRST;
    RCC->AHB1ENR &= ~(RCC_AHB1ENR_ETH1MACEN | RCC_AHB1ENR_ETH1TXEN | RCC_AHB1ENR_ETH1RXEN);
}

/* ---- Data path ----------------------------------------------------------- */
const uint8_t *eth_rx_peek(uint32_t *len)
{
    for (uint32_t n = 0; n < RX_COUNT; n++) {
        uint32_t d3 = s_rx[s_rx_i].d3;
        if (d3 & RDES3_OWN) return NULL;
        __DMB();
        if (!(d3 & (RDES3_CTXT | RDES3_ES)) &&
            (d3 & (RDES3_FD | RDES3_LD)) == (RDES3_FD | RDES3_LD)) {
            uint32_t l = d3 & RDES3_PL;
            if (l && l <= ETH_FRAME_MAX) {
                *len = l;
                g_eth.rx_frames++;
                return s_rxbuf[s_rx_i];
            }
        }
        g_eth.rx_errors++;
        eth_rx_release();
    }
    return NULL;
}

void eth_rx_release(void)
{
    rx_arm(s_rx_i);
    s_rx_i = (s_rx_i + 1) % RX_COUNT;
    __DSB();
    ETH->DMACRDTPR = (uint32_t)&s_rx[RX_COUNT];
}

uint8_t *eth_tx_alloc(void)
{
    if (s_tx[s_tx_i].d3 & TDES3_OWN) {
        g_eth.tx_drops++;
        return NULL;
    }
    return s_txbuf[s_tx_i];
}

void eth_tx_send(uint32_t len)
{
    desc_t *d = &s_tx[s_tx_i];
    d->d0 = (uint32_t)s_txbuf[s_tx_i];
    d->d1 = 0;
    d->d2 = len & 0x3FFFu;
    __DMB();
    d->d3 = TDES3_OWN | TDES3_FD | TDES3_LD | (len & 0x7FFFu);
    s_tx_i = (s_tx_i + 1) % TX_COUNT;
    __DSB();
    ETH->DMACTDTPR = (uint32_t)&s_tx[TX_COUNT];
    g_eth.tx_frames++;
}
