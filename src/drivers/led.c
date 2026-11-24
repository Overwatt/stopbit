/* led.c — Status LED driver. All hardware specifics live in board.h. */
#include "stm32h7xx_hal.h"
#include "board.h"
#include "led.h"

void led_init(void)
{
    BOARD_LED_CLK_ENABLE();

    GPIO_InitTypeDef g = {0};
    g.Pin   = BOARD_LED_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BOARD_LED_PORT, &g);

    led_off();
}

void led_on(void)
{
    HAL_GPIO_WritePin(BOARD_LED_PORT, BOARD_LED_PIN,
                      BOARD_LED_ACTIVE_HIGH ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void led_off(void)
{
    HAL_GPIO_WritePin(BOARD_LED_PORT, BOARD_LED_PIN,
                      BOARD_LED_ACTIVE_HIGH ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void led_toggle(void)
{
    HAL_GPIO_TogglePin(BOARD_LED_PORT, BOARD_LED_PIN);
}
