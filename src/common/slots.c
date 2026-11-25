#include "slots.h"
#include "extflash.h"
#include "sha256.h"
#include "system.h"
#include "fw_id.h"
#include "wdt.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define DEV        1
#define MAGIC      0x544C5335u
#define HDR_SIZE   4096u
#define SLOT_BYTES (HDR_SIZE + STAGE_SIZE)
#define OFF_CONFIRM  0x100u
#define OFF_ATTEMPT  0x110u          /* SLOT_TRIES words */

static const uint32_t BASE[SLOT_COUNT] = { 0x100000u, 0x180000u };
_Static_assert(SLOT_BYTES <= 0x80000u, "slot too big");

void slots_get(int s, slot_info_t *o)
{
    uint32_t confirm, att[SLOT_TRIES];
    memset(o, 0, sizeof *o);
    extflash_read(DEV, BASE[s], &o->hdr, sizeof o->hdr);
    o->valid = o->hdr.magic == MAGIC && o->hdr.size && o->hdr.size <= STAGE_SIZE;
    o->hdr.version[sizeof o->hdr.version - 1] = 0;
    if (!o->valid) return;
    extflash_read(DEV, BASE[s] + OFF_CONFIRM, &confirm, 4);
    extflash_read(DEV, BASE[s] + OFF_ATTEMPT, att, sizeof att);
    o->confirmed = confirm == 0;
    for (int i = 0; i < SLOT_TRIES; i++) o->tries_left += att[i] == 0xFFFFFFFFu;
}

static bool payload_ok(const slot_hdr_t *h)
{
    sha256_t c;
    uint8_t d[32];
    sha256_init(&c);
    sha256_update(&c, (const void *)STAGE_ADDR, h->size);
    sha256_final(&c, d);
    return ct_equal(d, h->sha, 32) && !image_check_app(h->size);
}

int slots_load_best(void)
{
    slot_info_t in[SLOT_COUNT];
    for (int s = 0; s < SLOT_COUNT; s++) slots_get(s, &in[s]);
    int first = (in[1].valid && (!in[0].valid || in[1].hdr.seq > in[0].hdr.seq)) ? 1 : 0;

    for (int k = 0; k < SLOT_COUNT; k++) {
        int s = first ^ k;
        slot_info_t *i = &in[s];
        if (!i->valid || (!i->confirmed && i->tries_left == 0)) continue;
        if (!i->confirmed) {
            const uint32_t zero = 0;
            uint32_t n = (uint32_t)(SLOT_TRIES - i->tries_left);
            if (!extflash_write(DEV, BASE[s] + OFF_ATTEMPT + 4 * n, &zero, 4)) continue;
        }
        extflash_read(DEV, BASE[s] + HDR_SIZE, (void *)STAGE_ADDR, i->hdr.size);
        if (payload_ok(&i->hdr)) return s;
    }
    return SLOT_NONE;
}

const char *slots_install(uint32_t size, int *slot)
{
    static uint8_t chk[256];
    slot_info_t in[SLOT_COUNT];
    slot_hdr_t h = { .magic = MAGIC, .size = size };
    sha256_t c;
    uint32_t seq = 0;
    const uint8_t *img = (const uint8_t *)STAGE_ADDR;

    for (int s = 0; s < SLOT_COUNT; s++) {
        slots_get(s, &in[s]);
        if (in[s].valid && in[s].hdr.seq > seq) seq = in[s].hdr.seq;
    }
    /* Overwrite whichever slot we can best afford to lose: an invalid one, then
     * an unconfirmed one, then the older one. */
    int t;
    if (in[0].valid != in[1].valid) t = in[0].valid ? 1 : 0;
    else if (in[0].valid && in[0].confirmed != in[1].confirmed) t = in[0].confirmed ? 1 : 0;
    else t = in[1].hdr.seq < in[0].hdr.seq ? 1 : 0;

    h.seq = seq + 1;
    memcpy(h.version, ((const fw_id_t *)(img + FW_ID_OFFSET))->version, sizeof h.version);
    sha256_init(&c);
    sha256_update(&c, img, size);
    sha256_final(&c, h.sha);

    /* Header sector goes first on erase and last on write, so a power cut at
     * any point leaves this slot invalid and the other one untouched. */
    uint32_t len = (size + 4095u) & ~4095u;
    if (!extflash_erase(DEV, BASE[t], HDR_SIZE + len)) return "erase timeout";
    if (!extflash_write(DEV, BASE[t] + HDR_SIZE, img, size)) return "write timeout";
    for (uint32_t o = 0; o < size; o += sizeof chk) {
        uint32_t n = size - o < sizeof chk ? size - o : sizeof chk;
        extflash_read(DEV, BASE[t] + HDR_SIZE + o, chk, n);
        if (memcmp(chk, img + o, n)) return "verify failed";
        wdt_feed();
    }
    if (!extflash_write(DEV, BASE[t], &h, sizeof h)) return "header write timeout";
    slots_get(t, &in[t]);
    if (memcmp(&in[t].hdr, &h, sizeof h)) return "header verify failed";
    *slot = t;
    return NULL;
}

bool slots_confirm(int s)
{
    const uint32_t zero = 0;
    slot_info_t i;
    if (s < 0 || s >= SLOT_COUNT) return false;
    if (!extflash_write(DEV, BASE[s] + OFF_CONFIRM, &zero, 4)) return false;
    slots_get(s, &i);
    return i.valid && i.confirmed;
}

void bkp_init(void)
{
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    (void)RCC->APB4ENR;
    PWR->CR1 |= PWR_CR1_DBP;
    while (!(PWR->CR1 & PWR_CR1_DBP)) { }
}
uint32_t bkp_get(int i) { return (&RTC->BKP0R)[i]; }
void     bkp_set(int i, uint32_t v) { (&RTC->BKP0R)[i] = v; __DSB(); }

void bkp_log_reset(uint32_t rsr)
{
    for (int i = RSTLOG_N - 1; i > 0; i--) bkp_set(BKP_RSTLOG + i, bkp_get(BKP_RSTLOG + i - 1));
    bkp_set(BKP_RSTLOG, rsr);
    bkp_set(BKP_BOOTS, bkp_get(BKP_BOOTS) + 1);
}
