/*
 * wdt.h — External watchdog on PC10 (WDI). Found by experiment: without an edge
 * roughly every 1.6 s it pulls NRST, even while the core is halted under SWD.
 * It cannot be disabled in software, so anything slow must feed it.
 */
#ifndef WDT_H
#define WDT_H
#include "stm32h7xx.h"

static inline void wdt_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;
    MODIFY_REG(GPIOC->MODER, GPIO_MODER_MODE10, GPIO_MODER_MODE10_0);
}

/* Unconditional inline: also used by code running from ITCM during erase. */
__attribute__((always_inline)) static inline void wdt_feed(void)
{
    GPIOC->BSRR = (GPIOC->ODR & (1u << 10)) ? (1u << 26) : (1u << 10);
}
#endif
