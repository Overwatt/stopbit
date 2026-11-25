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
#include "slots.h"
#include "board_init.h"

extern const uint32_t g_pfnVectors[];

__attribute__((section(".fw_id"), used))
const fw_id_t g_fw_id = { FW_ID_MAGIC_LOADER, (uint32_t)g_pfnVectors, FW_VERSION };

uint32_t g_reset_cause;
bool     g_clock_hse;

/* Boots the best app slot; returns only if the loader should stay up. */
static void try_boot_app(void)
{
    if (bkp_get(BKP_STAY) == BKP_STAY_MAGIC) {
        bkp_set(BKP_STAY, 0);
        return;
    }
    if ((uint32_t)g_pfnVectors != FLASH_BANK1_BASE) return;   /* SWD RAM run */

    uint32_t n = bkp_get(BKP_CRASHES);
    if (n >= CRASH_LIMIT) return;
    int s = slots_load_best();
    if (s == SLOT_NONE) return;
    bkp_set(BKP_CRASHES, n + 1);      /* the app clears this after 30 s */
    bkp_set(BKP_SLOT, (uint32_t)s);
    jump_to_image(STAGE_ADDR);
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

    board_outputs_init();
    wdt_feed();
    bkp_init();
    bkp_log_reset(g_reset_cause);
    /* Pulling the plug is not a crash; only count resets while powered. */
    if (g_reset_cause & RCC_RSR_PORRSTF) bkp_set(BKP_CRASHES, 0);
    extflash_init();
    try_boot_app();

    HAL_Delay(50);                        /* let the PHY come out of reset */
    net_init();
    http_init();
    for (;;) {
        wdt_feed();
        net_poll();
        http_poll();
    }
}
