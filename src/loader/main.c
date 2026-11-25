/*
 * Loader: lives in internal flash, owns the network update path.
 * D-cache stays off (nothing here needs the speed); I-cache is on.
 */
#include "stm32h7xx_hal.h"
#include "board.h"
#include "system.h"
#include "net.h"
#include "http.h"
#include "extflash.h"
#include "fw_id.h"
#include "wdt.h"

extern const uint32_t g_pfnVectors[];

__attribute__((section(".fw_id"), used))
const fw_id_t g_fw_id = { FW_ID_MAGIC_LOADER, (uint32_t)g_pfnVectors, FW_VERSION };

uint32_t g_reset_cause;
bool     g_clock_hse;

/* Put every GPIO the stock firmware drove at its stock idle level. Which of
 * the high ones (if any) is the PHY reset is not known yet. */
static void stock_outputs(void)
{
    static const struct { GPIO_TypeDef *p; uint16_t hi, lo; } T[] = {
        { GPIOA, GPIO_PIN_4 | GPIO_PIN_5, GPIO_PIN_3 | GPIO_PIN_12 | GPIO_PIN_15 },
        { GPIOC, 0, GPIO_PIN_8 },            /* PC10 is the watchdog: wdt.h */
        { GPIOD, GPIO_PIN_13, GPIO_PIN_4 | GPIO_PIN_12 | GPIO_PIN_15 },
        { GPIOE, GPIO_PIN_4, GPIO_PIN_12 | GPIO_PIN_14 },
    };
    GPIO_InitTypeDef g = { .Mode = GPIO_MODE_OUTPUT_PP, .Speed = GPIO_SPEED_FREQ_LOW };
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        T[i].p->BSRR = T[i].hi | ((uint32_t)T[i].lo << 16);
        g.Pin = T[i].hi | T[i].lo;
        HAL_GPIO_Init(T[i].p, &g);
    }
}

int main(void)
{
    SCB->VTOR = (uint32_t)g_pfnVectors;   /* SystemInit assumes internal flash */
    __DSB();
    SCB_EnableICache();
    wdt_init();
    wdt_feed();
    HAL_Init();
    g_reset_cause = RCC->RSR;
    RCC->RSR |= RCC_RSR_RMVF;
    g_clock_hse = SystemClock_Config();

    stock_outputs();
    wdt_feed();
    extflash_init();
    HAL_Delay(50);                        /* let the PHY come out of reset */
    net_init();
    http_init();
    for (;;) {
        wdt_feed();
        net_poll();
        http_poll();
    }
}
