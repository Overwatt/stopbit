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
#include "tls.h"
#include "acme.h"
#include "clock.h"
#include "events.h"
#include "fw_id.h"
#include "bridge.h"
#include "api.h"

#define HEALTHY_MS  30000u     /* uptime that clears the crash counter */
#define TRIAL_MS   180000u     /* unconfirmed images give up after this */

extern const uint32_t g_pfnVectors[];

__attribute__((section(".fw_id"), used))
const fw_id_t g_fw_id = { FW_ID_MAGIC_APP, (uint32_t)g_pfnVectors, FW_VERSION };

uint32_t g_reset_cause, g_last_crumb;
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

/* While a save waits for flash: the ports carry on, the rest can wait. */
static void meanwhile(void)
{
    hwinfo_loop();
    bridge_pump();
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
    g_last_crumb = bkp_get(BKP_CRUMB);     /* where the last run was when it stopped */
#define CRUMB(n) bkp_set(BKP_CRUMB, (n))
#define STEP(n, name) do { CRUMB(n); hwinfo_step(name); } while (0)
    extflash_init();
    events_init();
    {
        uint32_t f = bkp_get(BKP_FAULT), pc = bkp_get(BKP_FAULT + 1);
        static const char *const NAME[] = { "", "", "", "hard fault", "memory fault", "bus fault", "usage fault" };
        if ((f & 0xFFFF0000u) == FAULT_MAGIC && (f & 0xFFFFu) < 7 && bkp_get(BKP_FAULT_SEEN) != (f ^ pc)) {
            events_add(EV_FAULT, 0, pc, NAME[f & 0xFFFFu]);
            bkp_set(BKP_FAULT_SEEN, f ^ pc);
        }
        events_add(EV_START, 0, g_reset_cause, g_fw_id.version);
    }
#if defined(TEST_HANG)                     /* rollback test: watchdog reset */
    for (;;) { }
#elif defined(TEST_NONET)                  /* rollback test: alive, unreachable */
    for (;;) { wdt_feed(); if (HAL_GetTick() > 15000u) NVIC_SystemReset(); }
#endif
    CRUMB(1); net_init();        /* marks the Ethernet memory uncacheable */
    CRUMB(2); SCB_EnableDCache();    /* nothing else here uses DMA */
    CRUMB(3); bridge_init();
    CRUMB(4); api_init();
    extflash_meanwhile(meanwhile);
    CRUMB(5); http_init();
    CRUMB(6); tls_init();
    CRUMB(7); acme_init();
    CRUMB(8); clock_init();
    for (;;) {
        hwinfo_loop();
        wdt_feed();
        STEP(11, "network"); net_poll();
        STEP(12, "web"); http_poll();
        STEP(13, "https"); tls_poll();
        STEP(14, "renewal"); acme_poll();
        STEP(15, "bridge"); bridge_poll();
        STEP(16, "api"); api_poll();
        STEP(17, "supervise"); supervise(); clock_poll();
        STEP(18, "events"); events_poll();
        hwinfo_step("led"); led_poll();
    }
}
