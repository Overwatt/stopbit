/*
 * Application: copied from external flash into AXI SRAM by the loader.
 * Network stack, web UI + API, and the RS485 <-> TCP/UDP bridge.
 */
#include "stm32h7xx_hal.h"
#include "system.h"
#include "net.h"
#include "http.h"
#include "extflash.h"
#include "slots.h"
#include "fw_id.h"
#include "wdt.h"
#include "board.h"
#include "board_init.h"
#include "hwinfo.h"
#include "bridge.h"
#include "api.h"

#define HEALTHY_MS  30000u     /* uptime that clears the crash counter */
#define TRIAL_MS   180000u     /* unconfirmed images give up after this */

extern const uint32_t g_pfnVectors[];

__attribute__((section(".fw_id"), used))
const fw_id_t g_fw_id = { FW_ID_MAGIC_APP, (uint32_t)g_pfnVectors, FW_VERSION };

uint32_t g_reset_cause;
bool     g_clock_hse;

static void supervise(void)
{
    static bool healthy, trial_done;
    if (!healthy && HAL_GetTick() > HEALTHY_MS) {
        healthy = true;
        bkp_set(BKP_CRASHES, 0);
    }
    if (!trial_done && HAL_GetTick() > TRIAL_MS) {
        int s = (int)bkp_get(BKP_SLOT);
        slot_info_t i;
        trial_done = true;
        if (s < 0 || s >= SLOT_COUNT) return;      /* started by /run */
        slots_get(s, &i);
        if (!i.confirmed) NVIC_SystemReset();
    }
}

/* Work light: board_outputs_init() made the pin an output. */
static void led_poll(void)
{
    static uint32_t t;
    if (HAL_GetTick() - t < (hwinfo_button_armed() ? 80u : 500u)) return;
    t = HAL_GetTick();
    BOARD_LED_PORT->BSRR = (BOARD_LED_PORT->ODR & BOARD_LED_PIN) ? (uint32_t)BOARD_LED_PIN << 16 : BOARD_LED_PIN;
}

int main(void)
{
    SCB->VTOR = (uint32_t)g_pfnVectors;
    __DSB();
    SCB_EnableICache();
    wdt_init();
    wdt_feed();
    HAL_Init();
    g_clock_hse = SystemClock_Config();
    board_outputs_init();
    wdt_feed();
    bkp_init();
    g_reset_cause = bkp_get(BKP_RSTLOG);   /* the loader cleared RCC_RSR */
    extflash_init();
#if defined(TEST_HANG)                     /* rollback test: watchdog reset */
    for (;;) { }
#elif defined(TEST_NONET)                  /* rollback test: alive, unreachable */
    for (;;) { wdt_feed(); if (HAL_GetTick() > 15000u) NVIC_SystemReset(); }
#endif
    net_init();
    bridge_init();
    api_init();
    http_init();
    for (;;) {
        wdt_feed();
        net_poll();
        http_poll();
        bridge_poll();
        api_poll();
        supervise();
        led_poll();
    }
}
