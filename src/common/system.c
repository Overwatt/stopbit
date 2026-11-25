#include "system.h"
#include "fw_id.h"
#include "eth_mac.h"
#include "wdt.h"
#include "stm32h7xx_hal.h"
#include <string.h>

void jump_to_image(uint32_t addr)
{
    if (RCC->AHB1ENR & RCC_AHB1ENR_ETH1MACEN) eth_stop();
    __disable_irq();
    SysTick->CTRL = 0;
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    ARM_MPU_Disable();
    SCB_DisableICache();
    wdt_feed();
    SCB->VTOR = addr;
    __DSB();
    __ISB();
    uint32_t sp = ((volatile uint32_t *)addr)[0], pc = ((volatile uint32_t *)addr)[1];
    __asm volatile("msr msp, %0\n dsb\n isb\n cpsie i\n bx %1\n" :: "r"(sp), "r"(pc));
    __builtin_unreachable();
}

/* ---- Self-update --------------------------------------------------------- */
/* Runs from ITCM: the H750's internal flash is one 128 KB sector, so the code
 * doing the erase cannot live in it. Must not call anything in flash. */
__attribute__((section(".itcm"), noinline, noreturn,
               optimize("no-tree-loop-distribute-patterns")))
static void flash_worker(const uint32_t *src, uint32_t words)
{
    volatile uint32_t *dst = (volatile uint32_t *)0x08000000u;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (FLASH->CR1 & FLASH_CR_LOCK) {
            FLASH->KEYR1 = 0x45670123u;
            FLASH->KEYR1 = 0xCDEF89ABu;
        }
        while (FLASH->SR1 & (FLASH_SR_BSY | FLASH_SR_QW)) wdt_feed();
        FLASH->CCR1 = 0xFFFFFFFFu;
        FLASH->CR1 = FLASH_CR_SER | FLASH_CR_PSIZE;
        FLASH->CR1 = FLASH_CR_SER | FLASH_CR_PSIZE | FLASH_CR_START;
        __DSB();
        while (FLASH->SR1 & (FLASH_SR_BSY | FLASH_SR_QW)) wdt_feed();
        FLASH->CR1 = FLASH_CR_PSIZE;

        for (uint32_t i = 0; i < words; i += 8) {
            FLASH->CR1 = FLASH_CR_PG | FLASH_CR_PSIZE;
            __ISB();
            __DSB();
            for (uint32_t j = 0; j < 8; j++) dst[i + j] = src[i + j];
            __ISB();
            __DSB();
            while (FLASH->SR1 & (FLASH_SR_BSY | FLASH_SR_QW | FLASH_SR_WBNE)) { }
            wdt_feed();
            FLASH->CR1 = FLASH_CR_PSIZE;
        }
        uint32_t bad = 0;
        for (uint32_t i = 0; i < words; i++) bad |= dst[i] ^ src[i];
        if (!bad) break;
    }
    FLASH->CR1 |= FLASH_CR_LOCK;
    __DSB();
    SCB->AIRCR = (0x5FAu << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
    __DSB();
    for (;;) { }
}

void self_flash(const uint8_t *src, uint32_t size)
{
    extern uint32_t _sitcm[], _eitcm[], _siitcm[];
    eth_stop();
    uint32_t padded = (size + 31u) & ~31u;
    memset((uint8_t *)src + size, 0xFF, padded - size);
    for (uint32_t *d = _sitcm, *s = _siitcm; d < _eitcm;) *d++ = *s++;
    __disable_irq();
    SysTick->CTRL = 0;
    SCB_DisableICache();
    __DSB();
    __ISB();
    flash_worker((const uint32_t *)src, padded / 4);
}
