/*
 * wdt.h — External watchdog (UM706SS) on PC10 (WDI). Found by experiment: without
 * an edge roughly every 1.6 s it pulls NRST, even while the core is halted under
 * SWD. The datasheet allows 1.0 to 2.25 s, so feed it well inside one second.
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
