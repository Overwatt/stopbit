/*
 * main.c — Entry point for the open USR-N540 firmware.
 *
 * Milestone 1 (current): boot, clock to 400 MHz, blink the heartbeat LED,
 * bring up the 4 RS485 ports. Ethernet / LwIP / gateway logic arrive at
 * later milestones (see docs/ROADMAP.md).
 */
#include "stm32h7xx_hal.h"
#include "board.h"
#include "led.h"
#include "rs485.h"

void SystemClock_Config(void);

static void cpu_cache_enable(void)
{
    /* The Cortex-M7 caches give a big speedup. NOTE(eth): once we add
     * Ethernet DMA we must add an MPU region to keep the DMA descriptor
     * memory non-cacheable (or use cache maintenance), or packets corrupt. */
    SCB_EnableICache();
    SCB_EnableDCache();
}

int main(void)
{
    cpu_cache_enable();

    HAL_Init();              /* resets peripherals, configures SysTick */
    SystemClock_Config();    /* 400 MHz from the 25 MHz crystal */

    led_init();
    rs485_init_all();

    /* Heartbeat. When we know the real LED pin (board.h), this blinks for
     * real; until then it exercises the clock + GPIO path. */
    for (;;) {
        led_toggle();
        HAL_Delay(250);

        /* Placeholder app: echo anything received on each RS485 port back
         * out the same port. Trivial, but proves the UART + DE direction
         * path end-to-end once the pins are correct. */
        rs485_service_echo();
    }
}

/* Called by HAL_Init via the weak HAL_MspInit hook if needed; left default. */
