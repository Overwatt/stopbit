#include "hwinfo.h"
#include "tls.h"
#include "board.h"
#include "bridge.h"
#include "button.h"
#include "cfg.h"
#include "eth_mac.h"
#include "extflash.h"
#include "fw_id.h"
#include "gcm_hw.h"
#include "slots.h"
#include "uart485.h"
#include "wdt.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <stdlib.h>

extern bool g_clock_hse;
extern uint8_t _ebss, _end, _estack, _ed2bss, _eaxi, _ed3bss;

/* Factory calibration, in system memory (DS12556). */
#define CAL_VREF   (*(const uint16_t *)0x1FF1E860u)     /* raw at 3.3 V */
#define CAL_TEMP1  (*(const uint16_t *)0x1FF1E820u)     /* raw at 30 C */
#define CAL_TEMP2  (*(const uint16_t *)0x1FF1E840u)     /* raw at 110 C (rev Y) or 130 C */

/* ---- ADC3: the chip's own temperature and reference voltage -------------- */
static bool    s_adc, s_valid;
static int32_t s_decic, s_mv;
static uint32_t s_at;

static bool wait_set(volatile uint32_t *r, uint32_t bit, bool want)
{
    for (uint32_t t0 = HAL_GetTick(); !(*r & bit) == want;) {
        wdt_feed();
        if (HAL_GetTick() - t0 > 5) return false;
    }
    return true;
}

/* ---- reload button ------------------------------------------------------- */
static button_t s_button;

static void button_init(void)
{
    GPIO_InitTypeDef g = { .Pin = BOARD_BUTTON_PIN, .Mode = GPIO_MODE_INPUT, .Pull = GPIO_PULLUP };
    __HAL_RCC_GPIOA_CLK_ENABLE();
    HAL_GPIO_Init(BOARD_BUTTON_PORT, &g);
}

bool hwinfo_button(void)
{
    return button_step(&s_button, !(BOARD_BUTTON_PORT->IDR & BOARD_BUTTON_PIN), HAL_GetTick());
}

bool hwinfo_button_armed(void) { return button_armed(&s_button, HAL_GetTick()); }

void hwinfo_init(void)
{
    button_init();
    if (!(RCC->CR & RCC_CR_HSIRDY)) return;                 /* per_ck, below */
    RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
    (void)RCC->AHB4ENR;
    MODIFY_REG(RCC->D3CCIPR, RCC_D3CCIPR_ADCSEL, RCC_D3CCIPR_ADCSEL_1);   /* per_ck = HSI, 64 MHz */
    /* /12 (5.3 MHz, or half that on rev V), reference and sensor on */
    ADC3_COMMON->CCR = (6u << ADC_CCR_PRESC_Pos) | ADC_CCR_VREFEN | ADC_CCR_TSEN;
    ADC3->CR &= ~ADC_CR_DEEPPWD;
    ADC3->CR |= ADC_CR_ADVREGEN;
    HAL_Delay(2);                                           /* regulator and sensor start */
    ADC3->CR |= ADC_CR_ADCAL;
    if (!wait_set(&ADC3->CR, ADC_CR_ADCAL, false)) return;
    ADC3->ISR = ADC_ISR_ADRDY;
    ADC3->CR |= ADC_CR_ADEN;
    if (!wait_set(&ADC3->ISR, ADC_ISR_ADRDY, true)) return;
    ADC3->CFGR = ADC_CFGR_JQDIS | ADC_CFGR_OVRMOD;          /* 16 bits, software start */
    ADC3->PCSEL |= (1u << 18) | (1u << 19);
    ADC3->SMPR2 = (7u << 24) | (7u << 27);                  /* 810.5 cycles */
    s_adc = true;
}

static bool sample(uint32_t ch, uint32_t *raw)
{
    ADC3->SQR1 = ch << ADC_SQR1_SQ1_Pos;
    ADC3->ISR = ADC_ISR_EOC | ADC_ISR_EOS | ADC_ISR_OVR;
    ADC3->CR |= ADC_CR_ADSTART;
    if (!wait_set(&ADC3->ISR, ADC_ISR_EOC, true)) return false;
    *raw = ADC3->DR;
    return true;
}

static bool measure(void)
{
    uint32_t ref, t;
    if (!s_adc) return false;
    if (s_valid && HAL_GetTick() - s_at < 1000) return true;
    s_valid = false;
    if (!sample(19, &ref) || !sample(18, &t) || !ref || CAL_TEMP2 <= CAL_TEMP1) return false;
    int32_t span = ((DBGMCU->IDCODE >> 16) <= 0x1003u ? 110 : 130) - 30;
    s_mv = (int32_t)((3300u * CAL_VREF + ref / 2) / ref);
    int32_t at33 = (int32_t)((int64_t)t * s_mv / 3300);     /* as if at 3.3 V */
    s_decic = 300 + (at33 - (int32_t)CAL_TEMP1) * span * 10 / ((int32_t)CAL_TEMP2 - (int32_t)CAL_TEMP1);
    s_at = HAL_GetTick();
    return s_valid = true;
}

/* ---- main loop ----------------------------------------------------------- */
/* Longest time between passes: since the start, and in the last 10 to 20 s. */
static uint32_t s_loop_at, s_loop_max, s_loop[2], s_loop_turn;
static bool s_loop_on;
/* And which step of the pass was longest: since the start, and in the same window.
 * A save that keeps the ports moving counts as a new pass, so splits a step. */
typedef struct { uint32_t us; const char *in; uint32_t gcm_us; } step_t;   /* gcm_us: of which AES-GCM */
static step_t s_step_max, s_steps[2];
static const char *s_step;
static uint32_t s_step_at, s_step_gcm;

static void step_end(uint32_t now)
{
    uint32_t mhz = SystemCoreClock / 1000000u, us = (now - s_step_at) / mhz;
    uint32_t g = gcm_hw_cycles(), gus = (g - s_step_gcm) / mhz;
    if (s_step && us > s_steps[0].us) s_steps[0] = (step_t){ us, s_step, gus };
    if (s_step && us > s_step_max.us) s_step_max = (step_t){ us, s_step, gus };
    s_step_at = now;
    s_step_gcm = g;
}

void hwinfo_step(const char *name)
{
    step_end(DWT->CYCCNT);
    s_step = name;
}

void hwinfo_loop(void)
{
    uint32_t now = DWT->CYCCNT, us = (now - s_loop_at) / (SystemCoreClock / 1000000u);
    s_loop_at = now;
    step_end(now);
    if (!s_loop_on) { s_loop_on = true; return; }
    if (HAL_GetTick() - s_loop_turn >= 10000u) {
        s_loop_turn = HAL_GetTick();
        s_loop[1] = s_loop[0];
        s_loop[0] = 0;
        s_steps[1] = s_steps[0];
        s_steps[0] = (step_t){ 0, NULL, 0 };
    }
    if (us > s_loop[0]) s_loop[0] = us;
    if (us > s_loop_max) s_loop_max = us;
}

/* ---- JSON ---------------------------------------------------------------- */
size_t hwinfo_json(char *out, size_t max)
{
    size_t n = 0;
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) return 0; \
        n += (size_t)w_; } while (0)
#define U(x) ((unsigned long)(x))
    uint32_t id = DBGMCU->IDCODE;
    const uint32_t *uid = (const uint32_t *)UID_BASE;
    OUT("{\"cpu\":{\"id\":\"0x%03lx\",\"rev\":\"0x%04lx\",\"uid\":\"%08lx%08lx%08lx\",\"mhz\":%lu,"
        "\"crystal\":%s,\"flash_kb\":%u",
        U(id & 0xFFF), U(id >> 16), U(uid[2]), U(uid[1]), U(uid[0]), U(SystemCoreClock / 1000000u),
        g_clock_hse ? "true" : "false", (unsigned)*(const uint16_t *)FLASHSIZE_BASE);
    if (measure())
        OUT(",\"temp_c\":%s%ld.%ld,\"mv\":%ld", s_decic < 0 ? "-" : "",
            labs(s_decic) / 10, labs(s_decic) % 10, (long)s_mv);
    OUT("},\"button\":{\"pressed\":%s,\"presses\":%lu,\"held_ms\":%lu},\"flash\":[",
        s_button.down ? "true" : "false", U(s_button.presses), U(s_button.held_ms));
    for (int i = 0; i < EXTFLASH_COUNT; i++) OUT("%s\"0x%06lx\"", i ? "," : "", U(extflash_id(i)));
    OUT("],\"eth\":{\"phy_id\":\"0x%08lx\",\"phy_addr\":%lu,\"link\":%s,\"speed\":%d,\"full_duplex\":%s,"
        "\"rx\":%lu,\"tx\":%lu,\"rx_err\":%lu,\"tx_drop\":%lu}",
        U(g_eth.phy_id), U(g_eth.phy_addr), g_eth.link_up ? "true" : "false",
        g_eth.speed100 ? 100 : 10, g_eth.full_duplex ? "true" : "false",
        U(g_eth.rx_frames), U(g_eth.tx_frames), U(g_eth.rx_errors), U(g_eth.tx_drops));

    OUT(",\"ports\":[");
    for (int i = 0; i < PORTS; i++) {
        char tx[8], rx[8], de[8];
        const char *u = uart485_pins(i, tx, rx, de);
        OUT("%s{\"uart\":\"%s\",\"tx\":\"%s\",\"rx\":\"%s\",\"de\":\"%s\"}", i ? "," : "", u, tx, rx, de);
    }

    int run = (int)bkp_get(BKP_SLOT);
    slot_info_t sl[SLOT_COUNT];
    for (int i = 0; i < SLOT_COUNT; i++) slots_get(i, &sl[i]);
    size_t heap = 0, peak = 0;
    tls_heap(&heap, &peak);
    OUT("],\"mem\":[{\"name\":\"AXI\",\"used\":%lu,\"size\":%lu},{\"name\":\"DTCM\",\"used\":%lu,\"size\":%lu},"
        "{\"name\":\"D2\",\"used\":%lu,\"size\":%lu},{\"name\":\"D3\",\"used\":%lu,\"size\":%lu}]",
        U((uint32_t)&_eaxi - D1_AXISRAM_BASE + peak), U(512u << 10), U((uint32_t)&_end - D1_DTCMRAM_BASE), U((uint32_t)&_estack - D1_DTCMRAM_BASE),
        U((uint32_t)&_ed2bss - D2_AHBSRAM_BASE), U(288u << 10), U((uint32_t)&_ed3bss - D3_SRAM_BASE), U(64u << 10));
    uint32_t ru, rp, rn, rw, el, ep, es;        /* the shared buffers: now, at most, how many, how often short */
    bridge_rings(&ru, &rp, &rn, &rw);
    eth_rx_stats(&el, &ep, &es);
    OUT(",\"pools\":{\"line\":[%lu,%lu,%lu,%lu],\"frames\":[%lu,%lu,%lu,%lu]}",
        U(ru), U(rp), U(rn), U(rw), U(el), U(ep), U(ETH_RX_BUFS), U(es));

    const step_t *r = s_steps[0].us > s_steps[1].us ? &s_steps[0] : &s_steps[1];
    OUT(",\"loop\":{\"recent_us\":%lu,\"max_us\":%lu,\"step\":{\"recent_us\":%lu,\"recent\":\"%s\",\"recent_gcm_us\":%lu,"
        "\"max_us\":%lu,\"max\":\"%s\",\"max_gcm_us\":%lu}}",
        U(s_loop[0] > s_loop[1] ? s_loop[0] : s_loop[1]), U(s_loop_max),
        U(r->us), r->in ? r->in : "", U(r->gcm_us), U(s_step_max.us), s_step_max.in ? s_step_max.in : "", U(s_step_max.gcm_us));
    const fw_id_t *ld = (const fw_id_t *)(FLASH_BASE + FW_ID_OFFSET);
    OUT(",\"loader\":\"%.23s\",\"slot\":%d,\"slots\":[", ld->magic == FW_ID_MAGIC_LOADER ? ld->version : "", run);
    for (int i = 0; i < SLOT_COUNT; i++)
        OUT("%s{\"version\":\"%.23s\",\"size\":%lu,\"confirmed\":%s}", i ? "," : "",
            sl[i].valid ? sl[i].hdr.version : "", sl[i].valid ? U(sl[i].hdr.size) : 0ul,
            sl[i].valid && sl[i].confirmed ? "true" : "false");
    OUT("],\"boots\":%lu,\"crashes\":%lu,\"resets\":[", U(bkp_get(BKP_BOOTS)), U(bkp_get(BKP_CRASHES)));
    for (int i = 0, k = 0; i < RSTLOG_N; i++)
        if (bkp_get(BKP_RSTLOG + i)) OUT("%s\"%lx\"", k++ ? "," : "", U(bkp_get(BKP_RSTLOG + i)));
    OUT("]}\n");
    return n;
#undef OUT
#undef U
}
