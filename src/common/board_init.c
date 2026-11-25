#include "stm32h7xx_hal.h"
#include "board.h"
#include "board_init.h"

/* Put every GPIO the stock firmware drove at its stock idle level. Which of
 * the high ones (if any) is the PHY reset is not known yet. */
void board_outputs_init(void)
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
