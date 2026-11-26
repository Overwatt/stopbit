/* Just enough of the HAL and CMSIS to build eth_mac.c on the host, against the fake
 * MAC in tests/test_eth.c. MDIO and the DMA reset finish at once (their busy bits are 0). */
#ifndef STUB_ETH_HAL_H
#define STUB_ETH_HAL_H
#include <stdint.h>

static inline uint32_t HAL_GetTick(void) { return 0; }
static inline void HAL_Delay(uint32_t ms) { (void)ms; }
static inline void __DMB(void) { }
static inline void __DSB(void) { }

typedef struct {
    volatile uint32_t MACCR, MACPFR, MACA0HR, MACA0LR, MACMDIOAR, MACMDIODR, MMCRIMR, MMCTIMR;
    volatile uint32_t MTLTQOMR, MTLRQOMR, DMAMR, DMASBMR, DMACCR, DMACTCR, DMACRCR;
    volatile uint32_t DMACTDLAR, DMACTDRLR, DMACTDTPR, DMACRDLAR, DMACRDRLR, DMACRDTPR;
} t_eth_t;
extern t_eth_t t_eth;
#define ETH (&t_eth)
#define ETH_MACCR_FES 0x4000u
#define ETH_MACCR_DM  0x2000u
#define ETH_MACCR_CST 0x200000u
#define ETH_MACCR_ACS 0x100000u
#define ETH_MACCR_TE  0x2u
#define ETH_MACCR_RE  0x1u
#define ETH_MACPFR_PM 0x10u
#define ETH_MACMDIOAR_MB 0u
#define ETH_MACMDIOAR_PA_Pos 21
#define ETH_MACMDIOAR_RDA_Pos 16
#define ETH_MACMDIOAR_CR_Pos 8
#define ETH_MTLTQOMR_TSF 0x2u
#define ETH_MTLRQOMR_RSF 0x20u
#define ETH_DMAMR_SWR 0u
#define ETH_DMASBMR_AAL 0x1000u
#define ETH_DMASBMR_FB 0x1u
#define ETH_DMACTCR_ST 0x1u
#define ETH_DMACRCR_SR 0x1u

typedef struct { volatile uint32_t AHB1ENR, AHB1RSTR, AHB2ENR; } t_rcc_t;
extern t_rcc_t t_rcc;
#define RCC (&t_rcc)
#define RCC_AHB1ENR_ETH1MACEN 1u
#define RCC_AHB1ENR_ETH1TXEN 2u
#define RCC_AHB1ENR_ETH1RXEN 4u
#define RCC_AHB1RSTR_ETH1MACRST 1u
#define RCC_AHB2ENR_D2SRAM1EN 1u
#define RCC_AHB2ENR_D2SRAM2EN 2u
#define RCC_AHB2ENR_D2SRAM3EN 4u
typedef struct { volatile uint32_t PMCR; } t_syscfg_t;
extern t_syscfg_t t_syscfg;
#define SYSCFG (&t_syscfg)
#define SYSCFG_PMCR_EPIS_SEL 0x00E00000u
#define SYSCFG_PMCR_EPIS_SEL_2 0x00800000u
#define MODIFY_REG(r, clear, set) ((r) = ((r) & ~(clear)) | (set))
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
#define __HAL_RCC_SYSCFG_CLK_ENABLE() ((void)0)

typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
#define GPIOA ((void *)1)
#define GPIOB ((void *)2)
#define GPIOC ((void *)3)
#define GPIO_MODE_AF_PP 2u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u
#define GPIO_AF11_ETH 11u
static inline void HAL_GPIO_Init(void *port, GPIO_InitTypeDef *g) { (void)port; (void)g; }

#define ARM_MPU_AP_FULL 3u
#define ARM_MPU_REGION_SIZE_32KB 14u
#define ARM_MPU_REGION_SIZE_64KB 15u
#define ARM_MPU_RASR(x, ap, tex, s, c, b, srd, size) ((uint32_t)(srd) << 8 | (uint32_t)(size) << 1)
extern uint32_t t_mpu_base, t_mpu_rasr;
#define ARM_MPU_SetRegionEx(n, base, rasr) (t_mpu_base = (base), t_mpu_rasr = (rasr))
#define ARM_MPU_Disable() ((void)0)
#define ARM_MPU_Enable(x) ((void)0)
#define MPU_CTRL_PRIVDEFENA_Msk 4u
#endif
