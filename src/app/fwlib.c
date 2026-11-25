#include "fwlib.h"
#include "extflash.h"
#include "fw_id.h"
#include "sha256.h"
#include "system.h"
#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <string.h>

#define MAGIC      0x4C573546u          /* not slots.c's: a space is never bootable */
#define SECTOR     4096u
#define PAGE       256u
#define CHUNK      1024u
#define SPACE      0x64000u             /* 400 KB: header sector, then the image */
#define OFF_TRIED  0x100u
#define OFF_KEPT   0x104u
#define STUCK_MS   3000u

/* In the order they are first used. Chip 0 from the top, then the bottom of chip 1.
 * Settings (chip 1, 0x0F0000) and the boot slots (from 0x100000) are above all of it. */
static const struct { uint8_t dev; uint32_t addr; } AT[FWLIB_COUNT] = {
    { 0, 0x190000u }, { 0, 0x12C000u }, { 0, 0x0C8000u }, { 0, 0x064000u }, { 0, 0x000000u },
    { 1, 0x064000u }, { 1, 0x000000u },
};
_Static_assert(SECTOR + STAGE_SIZE <= SPACE, "library space too small");
_Static_assert(sizeof(slot_hdr_t) <= OFF_TRIED, "header overlaps the marks");

#ifndef AXI_BSS
#define AXI_BSS __attribute__((section(".axi_bss")))
#endif
static AXI_BSS uint8_t s_ring[FWLIB_RING];
static uint32_t s_head, s_count;

typedef enum { J_IDLE, J_STORE, J_CHECK, J_ARCHIVE, J_ARCHIVE_CHECK, J_COPY, J_COPY_CHECK } job_t;
static job_t       s_job;
static bool        s_failed;
static const char *s_err = "";
static slot_hdr_t  s_hdr;               /* the image being installed */
static int         s_space, s_slot, s_running;
static const char *s_need;
static bool        s_dup;               /* already in the library: take the upload, keep ours */
static bool        s_looked;            /* the first bytes are ours */
static int         s_arch;              /* space the old boot slot is being saved to */
static slot_hdr_t  s_arch_hdr;

/* ---- entries ------------------------------------------------------------- */
void fwlib_get(int i, fwlib_entry_t *o)
{
    uint32_t mark[2];
    memset(o, 0, sizeof *o);
    extflash_read(AT[i].dev, AT[i].addr, &o->hdr, sizeof o->hdr);
    o->valid = o->hdr.magic == MAGIC && o->hdr.size >= 0x400u && o->hdr.size <= STAGE_SIZE;
    o->hdr.version[sizeof o->hdr.version - 1] = 0;
    if (!o->valid) return;
    extflash_read(AT[i].dev, AT[i].addr + OFF_TRIED, mark, sizeof mark);
    o->tried = mark[0] == 0;
    o->kept = mark[1] == 0;
}

static bool same(const uint8_t a[32], const uint8_t b[32]) { return !memcmp(a, b, 32); }

static int find(const uint8_t sha[32])
{
    fwlib_entry_t e;
    for (int i = 0; i < FWLIB_COUNT; i++) {
        fwlib_get(i, &e);
        if (e.valid && same(e.hdr.sha, sha)) return i;
    }
    return -1;
}

static void mark(int i, uint32_t off)
{
    const uint32_t zero = 0;
    extflash_write(AT[i].dev, AT[i].addr + off, &zero, 4);
}

/* The space we can best afford to lose, never `keep` or the running image. */
static int pick(int keep)
{
    fwlib_entry_t e;
    slot_info_t run;
    int old = -1, failed = -1;
    uint32_t old_seq = 0, failed_seq = 0;
    memset(&run, 0, sizeof run);
    if (s_running >= 0 && s_running < SLOT_COUNT) slots_get(s_running, &run);
    for (int i = 0; i < FWLIB_COUNT; i++) {
        if (i == keep) continue;
        fwlib_get(i, &e);
        if (!e.valid) return i;
        if (run.valid && same(e.hdr.sha, run.hdr.sha)) continue;
        if (e.tried && !e.kept && (failed < 0 || e.hdr.seq < failed_seq)) { failed = i; failed_seq = e.hdr.seq; }
        if (old < 0 || e.hdr.seq < old_seq) { old = i; old_seq = e.hdr.seq; }
    }
    return failed >= 0 ? failed : old;
}

static bool commit(int i, slot_hdr_t *h)
{
    fwlib_entry_t e;
    h->magic = MAGIC;
    h->seq = 0;
    for (int k = 0; k < FWLIB_COUNT; k++) {
        fwlib_get(k, &e);
        if (e.valid && e.hdr.seq > h->seq) h->seq = e.hdr.seq;
    }
    h->seq++;
    if (!extflash_write(AT[i].dev, AT[i].addr, h, sizeof *h)) return false;
    fwlib_get(i, &e);
    return e.valid && !memcmp(&e.hdr, h, sizeof *h);
}

/* ---- copying: header sector erased first, so it is invalid until committed -- */
static struct {
    int      src_dev;                   /* -1: the ring */
    uint32_t src;
    int      dst_dev;
    uint32_t dst, size, pos, erased, at;
    uint32_t drained;
} s_cp;

static uint32_t ring_take(uint8_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (out) out[i] = s_ring[s_head];
        s_head = (s_head + 1) % FWLIB_RING;
    }
    s_count -= n;
    s_cp.drained += n;
    return n;
}

static void copy_begin(int src_dev, uint32_t src, int dst_dev, uint32_t dst, uint32_t size)
{
    uint32_t drained = s_cp.drained;
    memset(&s_cp, 0, sizeof s_cp);
    s_cp.drained = drained;
    s_cp.src_dev = src_dev;
    s_cp.src = src;
    s_cp.dst_dev = dst_dev;
    s_cp.dst = dst;
    s_cp.size = size;
    s_cp.at = HAL_GetTick();
}

/* 0: more to do. 1: written. -1: the chip is stuck. */
static int copy_step(void)
{
    uint8_t b[PAGE];
    if (extflash_busy(s_cp.dst_dev)) return HAL_GetTick() - s_cp.at > STUCK_MS ? -1 : 0;
    if (s_cp.pos == s_cp.size) return 1;
    if (s_cp.erased <= SECTOR + s_cp.pos) {
        extflash_erase_start(s_cp.dst_dev, s_cp.dst + s_cp.erased);
        s_cp.erased += SECTOR;
        s_cp.at = HAL_GetTick();
        return 0;
    }
    uint32_t n = s_cp.size - s_cp.pos;
    if (n > PAGE) n = PAGE;
    if (s_cp.src_dev < 0) {
        if (s_count < n) return 0;
        ring_take(b, n);
    } else {
        extflash_read(s_cp.src_dev, s_cp.src + s_cp.pos, b, n);
    }
    extflash_write_start(s_cp.dst_dev, s_cp.dst + SECTOR + s_cp.pos, b, n);
    s_cp.pos += n;
    s_cp.at = HAL_GetTick();
    return 0;
}

/* ---- checking: read it back, hash it, look for what it must support -------- */
static struct {
    int      dev;
    uint32_t addr, size, pos;
    const uint8_t *sha;
    sha256_t ctx;
    const char *need;
    bool     found;
    uint8_t  b[32 + CHUNK];             /* the tail of the last chunk, then this one */
    uint32_t tail;
} s_ck;

static void check_begin(int dev, uint32_t addr, uint32_t size, const uint8_t *sha, const char *need)
{
    memset(&s_ck, 0, sizeof s_ck);
    s_ck.dev = dev;
    s_ck.addr = addr + SECTOR;
    s_ck.size = size;
    s_ck.sha = sha;
    s_ck.need = need;
    s_ck.found = !need;
    sha256_init(&s_ck.ctx);
}

/* 0: more to do. 1: matches. -1: does not. */
static int check_step(void)
{
    uint32_t n = s_ck.size - s_ck.pos;
    if (n > CHUNK) n = CHUNK;
    if (n) {
        uint8_t *d = s_ck.b + s_ck.tail;
        extflash_read(s_ck.dev, s_ck.addr + s_ck.pos, d, n);
        sha256_update(&s_ck.ctx, d, n);
        s_ck.pos += n;
        if (!s_ck.found) {
            uint32_t have = s_ck.tail + n, k = (uint32_t)strlen(s_ck.need);
            for (uint32_t i = 0; i + k <= have && !s_ck.found; i++)
                s_ck.found = s_ck.b[i] == (uint8_t)s_ck.need[0] && !memcmp(s_ck.b + i, s_ck.need, k);
            uint32_t t = have < k - 1 ? have : k - 1;
            memmove(s_ck.b, s_ck.b + have - t, t);
            s_ck.tail = t;
        }
        return 0;
    }
    uint8_t d[32];
    sha256_final(&s_ck.ctx, d);
    return same(d, s_ck.sha) ? 1 : -1;
}

/* ---- job ----------------------------------------------------------------- */
static void stop(const char *err)
{
    s_job = J_IDLE;
    s_err = err;
    s_failed = true;
}

static const char *begin(int running, const char *need)
{
    if (s_job != J_IDLE) return "an update is already in progress";
    if (need && (!need[0] || strlen(need) > 32)) need = NULL;
    s_failed = false;
    s_err = "";
    s_running = running;
    s_need = need;
    s_head = s_count = 0;
    s_cp.drained = 0;
    s_looked = false;
    memset(&s_hdr, 0, sizeof s_hdr);
    return NULL;
}

const char *fwlib_receive(uint32_t size, const uint8_t sha[32], int running, const char *need)
{
    const char *why = begin(running, need);
    if (why) return why;
    if (size < 0x400u || size > STAGE_SIZE) return "bad size";
    s_hdr.size = size;
    memcpy(s_hdr.sha, sha, 32);
    s_space = find(sha);
    s_dup = s_space >= 0;
    if (!s_dup) s_space = pick(-1);
    if (s_space < 0) return "no space";
    copy_begin(-1, 0, AT[s_space].dev, AT[s_space].addr, size);
    s_job = J_STORE;
    return NULL;
}

size_t fwlib_room(void) { return s_job == J_STORE ? FWLIB_RING - s_count : 0; }

void fwlib_feed(const uint8_t *d, size_t n)
{
    if (s_job != J_STORE) return;
    while (n-- && s_count < FWLIB_RING) {
        s_ring[(s_head + s_count) % FWLIB_RING] = *d++;
        s_count++;
    }
}

static void check_stored(void)
{
    s_job = J_CHECK;
    check_begin(AT[s_space].dev, AT[s_space].addr, s_hdr.size, s_hdr.sha, s_need);
}

const char *fwlib_rollback(const uint8_t sha[32], int running, const char *need)
{
    fwlib_entry_t e;
    slot_info_t run;
    const char *why = begin(running, need);
    if (why) return why;
    if ((s_space = find(sha)) < 0) return "that firmware is no longer kept";
    if (running >= 0 && running < SLOT_COUNT) {
        slots_get(running, &run);
        if (run.valid && same(run.hdr.sha, sha)) return "that firmware is already running";
    }
    fwlib_get(s_space, &e);
    s_hdr = e.hdr;
    s_dup = true;
    check_stored();
    return NULL;
}

/* The first bytes, before anything is erased. */
static const char *look(void)
{
    static uint8_t b[CHUNK];
    fw_id_t id;
    for (uint32_t i = 0; i < CHUNK; i++) b[i] = s_ring[(s_head + i) % FWLIB_RING];
    const char *why = image_check_app(b, s_hdr.size);
    if (why) return why;
    memcpy(&id, b + FW_ID_OFFSET, sizeof id);
    memcpy(s_hdr.version, id.version, sizeof s_hdr.version);
    s_hdr.version[sizeof s_hdr.version - 1] = 0;
    return NULL;
}

static void copy_to_slot(void)
{
    s_job = J_COPY;
    copy_begin(AT[s_space].dev, AT[s_space].addr + SECTOR, SLOT_DEV, slots_addr(s_slot), s_hdr.size);
}

/* The library image is good. If the boot slot it will replace holds one that was
 * kept and is not in the library, save that first. */
static void install(void)
{
    slot_info_t old;
    s_slot = slots_spare(s_running);
    slots_get(s_slot, &old);
    if (old.valid && old.confirmed && find(old.hdr.sha) < 0 && (s_arch = pick(s_space)) >= 0) {
        s_arch_hdr = old.hdr;
        s_job = J_ARCHIVE;
        copy_begin(SLOT_DEV, slots_addr(s_slot) + SECTOR, AT[s_arch].dev, AT[s_arch].addr, old.hdr.size);
    } else {
        copy_to_slot();
    }
}

static void step(void)
{
    int r;
    const char *why;
    switch (s_job) {
    case J_STORE:
        if (!s_looked) {
            if (s_count < CHUNK) return;
            if ((why = look())) stop(why);
            s_looked = true;
            return;
        }
        if (s_dup) {                                    /* nothing to write; take it in */
            s_cp.pos += ring_take(NULL, s_count);
            if (s_cp.pos >= s_hdr.size) check_stored();
            return;
        }
        if ((r = copy_step()) < 0) stop("flash write timed out");
        else if (r) check_stored();
        return;
    case J_CHECK:
        if ((r = check_step()) == 0) return;
        if (r < 0) {
            if (s_dup) mark(s_space, 0);                /* drop it, or it would shadow a new upload */
            stop(s_dup ? "the kept copy was damaged and has been dropped" : "verify failed");
            return;
        }
        if (!s_dup && !commit(s_space, &s_hdr)) { stop("header write failed"); return; }
        if (!s_ck.found) { stop("that firmware would not respect a port's listen-only setting"); return; }
        install();
        return;
    case J_ARCHIVE:                                     /* never a reason to refuse an update */
        if ((r = copy_step()) == 0) return;
        if (r < 0) { copy_to_slot(); return; }
        s_job = J_ARCHIVE_CHECK;
        check_begin(AT[s_arch].dev, AT[s_arch].addr, s_arch_hdr.size, s_arch_hdr.sha, NULL);
        return;
    case J_ARCHIVE_CHECK:
        if ((r = check_step()) == 0) return;
        if (r > 0 && commit(s_arch, &s_arch_hdr)) { mark(s_arch, OFF_TRIED); mark(s_arch, OFF_KEPT); }
        copy_to_slot();
        return;
    case J_COPY:
        if ((r = copy_step()) < 0) stop("flash write timed out");
        else if (r) {
            s_job = J_COPY_CHECK;
            check_begin(SLOT_DEV, slots_addr(s_slot), s_hdr.size, s_hdr.sha, NULL);
        }
        return;
    case J_COPY_CHECK:
        if ((r = check_step()) == 0) return;
        if (r < 0) { stop("verify failed"); return; }
        mark(s_space, OFF_TRIED);
        if (!slots_commit(s_slot, &s_hdr)) { stop("header write failed"); return; }
        s_job = J_IDLE;
        s_failed = false;
        return;
    default:
        return;
    }
}

fw_result_t fwlib_poll(uint32_t *drained)
{
    bool was = s_job != J_IDLE;
    if (was) step();
    *drained = s_cp.drained;
    s_cp.drained = 0;
    if (!was) return FW_IDLE;
    if (s_job != J_IDLE) return FW_BUSY;
    return s_failed ? FW_FAILED : FW_DONE;
}

const char *fwlib_error(void)  { return s_err; }
int  fwlib_slot(void)   { return s_slot; }
bool fwlib_active(void) { return s_job != J_IDLE; }
void fwlib_cancel(void) { s_job = J_IDLE; s_head = s_count = 0; }

void fwlib_mark_kept(int slot)
{
    slot_info_t in;
    if (slot < 0 || slot >= SLOT_COUNT) return;
    slots_get(slot, &in);
    int i = in.valid ? find(in.hdr.sha) : -1;
    if (i >= 0) { mark(i, OFF_TRIED); mark(i, OFF_KEPT); }
}

size_t fwlib_json(int running, char *out, size_t max)
{
    static const char *const STAGE[] = { "", "storing", "checking", "saving", "saving", "installing", "installing" };
    size_t n = 0;
#define OUT(...) do { int w_ = snprintf(out + n, max - n, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= max - n) return 0; \
        n += (size_t)w_; } while (0)
    slot_info_t in[SLOT_COUNT];
    for (int s = 0; s < SLOT_COUNT; s++) slots_get(s, &in[s]);
    uint32_t pos = s_job == J_CHECK || s_job == J_ARCHIVE_CHECK || s_job == J_COPY_CHECK ? s_ck.pos : s_cp.pos;
    uint32_t size = s_job == J_ARCHIVE || s_job == J_ARCHIVE_CHECK ? s_arch_hdr.size : s_hdr.size;
    OUT("{\"spaces\":%d,\"job\":\"%s\",\"percent\":%lu,\"images\":[", FWLIB_COUNT, STAGE[s_job],
        s_job != J_IDLE && size ? (unsigned long)(pos * 100ull / size) : 0ul);
    for (int i = 0, k = 0; i < FWLIB_COUNT; i++) {
        fwlib_entry_t e;
        char sha[65];
        int slot = SLOT_NONE;
        fwlib_get(i, &e);
        if (!e.valid) continue;
        for (int s = 0; s < SLOT_COUNT; s++)
            if (in[s].valid && same(in[s].hdr.sha, e.hdr.sha)) { slot = s; e.kept |= in[s].confirmed; }
        for (int b = 0; b < 32; b++) snprintf(sha + 2 * b, 3, "%02x", e.hdr.sha[b]);
        for (char *p = e.hdr.version; *p; p++) if (*p < 0x20 || *p > 0x7e || *p == '"' || *p == '\\') *p = '?';
        OUT("%s{\"version\":\"%s\",\"size\":%lu,\"seq\":%lu,\"kept\":%s,\"tried\":%s,\"running\":%s,\"slot\":%d,\"sha\":\"%s\"}",
            k++ ? "," : "", e.hdr.version, (unsigned long)e.hdr.size, (unsigned long)e.hdr.seq,
            e.kept ? "true" : "false", e.tried ? "true" : "false",
            slot != SLOT_NONE && slot == running ? "true" : "false", slot, sha);
    }
    OUT("]}\n");
    return n;
#undef OUT
}
