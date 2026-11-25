/* cc -w -Itests/stub -Isrc/app -Isrc/common -DWDT_H '-Dwdt_feed()=(void)0' -DAXI_BSS= \
 *    tests/test_fwlib.c src/app/fwlib.c src/common/{slots,image,sha256}.c -o /tmp/fl && /tmp/fl */
#include "fwlib.h"
#include "fw_id.h"
#include "sha256.h"
#include "system.h"
#include "stm32h7xx_hal.h"
#include "flash_sim.h"
#include <assert.h>

void NVIC_SystemReset(void) { abort(); }

#define MAXIMG STAGE_SIZE
typedef struct { uint8_t d[MAXIMG]; uint32_t n; uint8_t sha[32]; char v[24]; } img_t;

static void make(img_t *im, const char *v, uint32_t n, const char *text)
{
    memset(im, 0, sizeof *im);
    im->n = n;
    for (uint32_t i = 0; i < n; i++) im->d[i] = (uint8_t)rand();
    uint32_t sp = 0x20020000u, pc = 0x24000401u;
    memcpy(im->d, &sp, 4);
    memcpy(im->d + 4, &pc, 4);
    fw_id_t id = { FW_ID_MAGIC_APP, STAGE_ADDR, "" };
    snprintf(id.version, sizeof id.version, "%s", v);
    memcpy(im->d + FW_ID_OFFSET, &id, sizeof id);
    if (text) memcpy(im->d + n - 900, text, strlen(text));
    sha256_t c;
    sha256_init(&c); sha256_update(&c, im->d, n); sha256_final(&c, im->sha);
    strcpy(im->v, id.version);
}

/* Put an image in a boot slot, as the loader's installer would have. */
static void boot_slot(int s, img_t *im, bool confirmed)
{
    slot_hdr_t h = { .size = im->n };
    memcpy(h.sha, im->sha, 32);
    strcpy(h.version, im->v);
    extflash_erase(SLOT_DEV, slots_addr(s), SLOT_HDR + 4096 * ((im->n + 4095) / 4096));
    extflash_write(SLOT_DEV, slots_addr(s) + SLOT_HDR, im->d, im->n);
    assert(slots_commit(s, &h));
    if (confirmed) assert(slots_confirm(s));
}
static bool slot_is(int s, const img_t *im)
{
    slot_info_t i;
    slots_get(s, &i);
    return i.valid && i.hdr.size == im->n && !memcmp(i.hdr.sha, im->sha, 32) &&
           !memcmp(F[SLOT_DEV] + slots_addr(s) + SLOT_HDR, im->d, im->n);
}
static int lib_find(const img_t *im)
{
    fwlib_entry_t e;
    for (int i = 0; i < FWLIB_COUNT; i++) {
        fwlib_get(i, &e);
        if (e.valid && !memcmp(e.hdr.sha, im->sha, 32)) return i;
    }
    return -1;
}
static int lib_count(void)
{
    fwlib_entry_t e; int n = 0;
    for (int i = 0; i < FWLIB_COUNT; i++) { fwlib_get(i, &e); n += e.valid; }
    return n;
}
static void protect(int running)
{
    f_nno = 0;
    f_no[f_nno++] = (range_t){ 1, 0x0C8000, 0x100000 };                     /* settings and above the library */
    f_no[f_nno++] = (range_t){ 0, 0x1F4000, 0x200000 };
    f_no[f_nno++] = (range_t){ 1, slots_addr(running), slots_addr(running) + 0x80000 };
}

static const char *run(void)
{
    uint32_t d;
    for (long i = 0; i < 2000000; i++) {
        fw_result_t r = fwlib_poll(&d);
        if (r == FW_DONE) return NULL;
        if (r == FW_FAILED) return fwlib_error();
        if (r == FW_IDLE) return "idle";
    }
    return "never finished";
}
/* As http.c does it: never more than a window's worth undrained. `cut` bytes sent. */
static const char *upload(const img_t *im, const uint8_t sha[32], int running, const char *need, uint32_t cut)
{
    uint32_t sent = 0, held = 0, d;
    const char *why = fwlib_receive(im->n, sha, running, need);
    if (why) return why;
    while (sent < cut) {
        uint32_t n = 1 + (uint32_t)rand() % 1460;
        if (n > cut - sent) n = cut - sent;
        if (held + n <= 5840) { fwlib_feed(im->d + sent, n); sent += n; held += n; }
        fw_result_t r = fwlib_poll(&d);
        assert(d <= held);
        held -= d;
        if (r == FW_FAILED) return fwlib_error();
        assert(r == FW_BUSY);
    }
    if (cut < im->n) { fwlib_cancel(); return "cut"; }
    return run();
}

static img_t A, B, C, D[10], J;

static void fresh(void)
{
    /* Vendor data everywhere the library will go. */
    f_nno = 0; f_cut = -1; f_ops = 0; f_stuck = -1; f_flip = 0;
    fwlib_cancel();
    for (int i = 0; i < EXTFLASH_SIZE; i++) { F[0][i] = (uint8_t)rand(); F[1][i] = (uint8_t)(i < 0x9a000 ? rand() : 0xFF); }
    boot_slot(0, &A, true);
    boot_slot(1, &B, true);                  /* B is newer and running */
    protect(1);
}

int main(void)
{
    srand(7);
    make(&A, "v-a", 30000, "x listen_only y");
    make(&B, "v-b", 40000, "x listen_only y");
    make(&C, "v-c", 50001, "x listen_only y");
    char json[4096];

    /* vendor data is not mistaken for images */
    fresh();
    assert(lib_count() == 0);
    assert(fwlib_json(1, json, sizeof json) && strstr(json, "\"images\":[]"));

    /* install: goes to the library and the spare slot; the kept image it replaces is saved */
    assert(!upload(&C, C.sha, 1, NULL, C.n));
    assert(fwlib_slot() == 0 && slot_is(0, &C) && slot_is(1, &B));
    slot_info_t in;
    slots_get(0, &in);
    assert(!in.confirmed && in.tries_left == SLOT_TRIES && !strcmp(in.hdr.version, "v-c"));
    slot_info_t in1; slots_get(1, &in1);
    assert(in.hdr.seq > in1.hdr.seq);
    fwlib_entry_t e;
    fwlib_get(lib_find(&C), &e);
    assert(e.tried && !e.kept && !strcmp(e.hdr.version, "v-c"));
    fwlib_get(lib_find(&A), &e);
    assert(e.kept && !strcmp(e.hdr.version, "v-a"));
    assert(lib_count() == 2);
    assert(slots_confirm(0));
    fwlib_mark_kept(0);
    fwlib_get(lib_find(&C), &e);
    assert(e.kept);

    /* now C runs from slot 0; roll back to A, which left the boot slots. B gets saved. */
    protect(0);
    assert(!fwlib_rollback(A.sha, 0, "listen_only"));
    assert(!run() && fwlib_slot() == 1 && slot_is(1, &A) && slot_is(0, &C) && lib_find(&B) >= 0);
    assert(slots_confirm(1));
    assert(fwlib_json(1, json, sizeof json));
    assert(strstr(json, "\"version\":\"v-a\"") && strstr(json, "\"running\":true"));
    protect(1);
    assert(!strcmp(fwlib_rollback(A.sha, 1, NULL), "that firmware is already running"));
    uint8_t nosha[32] = { 1 };
    assert(fwlib_rollback(nosha, 1, NULL));

    /* same upload again: not stored twice, and works at any split */
    int n0 = lib_count();
    assert(!upload(&C, C.sha, 1, NULL, C.n) && lib_count() == n0 && slot_is(0, &C));

    /* refusals leave both boot slots alone */
    fresh();
    img_t *bad = &J;
    make(bad, "v-j", 20000, NULL);
    assert(!strcmp(upload(bad, bad->sha, 1, "listen_only", bad->n), "that firmware would not respect a port's listen-only setting"));
    assert(slot_is(0, &A) && slot_is(1, &B) && lib_find(bad) >= 0);
    fwlib_rollback(bad->sha, 1, "listen_only");
    assert(strstr(run(), "listen-only") && slot_is(0, &A));
    assert(!upload(bad, bad->sha, 1, NULL, bad->n));         /* fine when no port needs it */
    fresh();
    uint8_t before[2][64]; memcpy(before[0], F[0] + 0x190000, 64);
    make(bad, "v-j", 20000, NULL); bad->d[FW_ID_OFFSET] ^= 1;
    assert(!strcmp(upload(bad, bad->sha, 1, NULL, bad->n), "not an app image"));
    assert(!memcmp(before[0], F[0] + 0x190000, 64) && f_ops == f_ops);       /* refused before erasing */
    make(bad, "v-j", 20000, NULL);
    assert(!strcmp(upload(bad, C.sha, 1, NULL, bad->n), "verify failed") && lib_count() == 0 && slot_is(0, &A));
    assert(!strcmp(fwlib_receive(100, C.sha, 1, NULL), "bad size"));
    assert(!strcmp(fwlib_receive(STAGE_SIZE + 1, C.sha, 1, NULL), "bad size"));
    assert(!fwlib_receive(C.n, C.sha, 1, NULL) && fwlib_active());
    assert(fwlib_receive(C.n, C.sha, 1, NULL) && fwlib_rollback(A.sha, 1, NULL));  /* one at a time */
    fwlib_cancel();

    /* cut uploads at many points: nothing becomes valid, and the next one works */
    for (uint32_t cut = 0; cut < C.n; cut += 997) {
        assert(!strcmp(upload(&C, C.sha, 1, NULL, cut), "cut"));
        assert(lib_count() == 0 && slot_is(0, &A) && slot_is(1, &B));
    }
    assert(!upload(&C, C.sha, 1, NULL, C.n) && slot_is(0, &C));

    /* the needle is found wherever it falls across a chunk boundary */
    for (int off = -14; off <= 2; off++) {
        fresh();
        make(bad, "v-n", 9000, NULL);
        memcpy(bad->d + 4096 + off, "listen_only", 11);
        sha256_t c; sha256_init(&c); sha256_update(&c, bad->d, bad->n); sha256_final(&c, bad->sha);
        assert(!upload(bad, bad->sha, 1, "listen_only", bad->n));
    }

    /* fill the library: failed ones go first, then the oldest, never the running one */
    fresh();
    for (int i = 0; i < 10; i++) { char v[8]; snprintf(v, sizeof v, "d%d", i); make(&D[i], v, 20000 + 100 * i, NULL); }
    int running = 1;
    for (int i = 0; i < 10; i++) {
        protect(running);
        assert(!upload(&D[i], D[i].sha, running, NULL, D[i].n));
        int s = fwlib_slot();
        assert(s != running && slot_is(s, &D[i]));
        if (i != 4) { assert(slots_confirm(s)); fwlib_mark_kept(s); running = s; }  /* D4 fails its trial */
        assert(lib_count() <= FWLIB_COUNT);
    }
    assert(lib_count() == FWLIB_COUNT && lib_find(&D[4]) < 0 && lib_find(&D[9]) >= 0 && lib_find(&D[8]) >= 0);
    assert(lib_find(&A) < 0 && lib_find(&D[0]) < 0);
    size_t jl = fwlib_json(running, json, sizeof json);
    printf("library JSON, full: %zu bytes\n", jl);
    assert(jl);
    assert(!fwlib_json(running, json, 200));                                 /* too small: said so */
    /* run the oldest one, then upload: it is the second oldest that goes */
    int o = -1, o2 = -1;
    for (int i = 0; i < 10; i++) if (lib_find(&D[i]) >= 0) { if (o < 0) o = i; else if (o2 < 0) o2 = i; }
    protect(running);
    assert(!fwlib_rollback(D[o].sha, running, NULL) && !run());
    running = fwlib_slot();
    assert(slots_confirm(running));
    protect(running);
    assert(!upload(&A, A.sha, running, NULL, A.n));
    assert(lib_find(&D[o]) >= 0 && lib_find(&D[o2]) < 0);
    /* keep rolling back and forth between the oldest and newest */
    for (int i = 0; i < 6; i++) {
        protect(running);
        int t = i & 1 ? 9 : 8;
        if (slot_is(running, &D[t])) continue;
        assert(!fwlib_rollback(D[t].sha, running, NULL) && !run() && slot_is(fwlib_slot(), &D[t]));
        running = fwlib_slot();
        assert(slots_confirm(running));
    }

    /* the largest image there can be, in every space: protected ranges stay untouched */
    fresh();
    running = 1;
    for (int i = 0; i < 9; i++) {
        char v[8]; snprintf(v, sizeof v, "m%d", i);
        make(&J, v, STAGE_SIZE, NULL);
        protect(running);
        assert(!upload(&J, J.sha, running, NULL, J.n) && slot_is(fwlib_slot(), &J));
        running = fwlib_slot();
        assert(slots_confirm(running));
        fwlib_mark_kept(running);
    }
    assert(lib_count() == FWLIB_COUNT);

    /* a kept copy that rots is refused, dropped, and can be uploaded again */
    fresh();
    assert(!upload(&C, C.sha, 1, NULL, C.n));
    for (int a = 0; a < 2; a++) {
        int i = lib_find(&C);
        assert(i >= 0);
        F[0][0x190000 + 4096 + 20000] ^= 4;
        boot_slot(0, &A, true);
        if (a) assert(strstr(upload(&C, C.sha, 1, NULL, C.n), "damaged"));
        else { assert(!fwlib_rollback(C.sha, 1, NULL)); assert(strstr(run(), "damaged")); }
        assert(slot_is(0, &A) && lib_find(&C) < 0);
        assert(!upload(&C, C.sha, 1, NULL, C.n) && slot_is(0, &C));
    }

    /* a stuck chip ends the job instead of hanging */
    fresh();
    f_stuck = 0;
    assert(!strcmp(upload(&C, C.sha, 1, NULL, C.n), "flash write timed out") && slot_is(0, &A));
    /* one byte that did not take is caught, wherever it is */
    for (uint32_t k = 3; k < 480; k += 7) {
        fresh();
        f_flip = k; f_ops = 0;
        const char *why = upload(&C, C.sha, 1, NULL, C.n);
        if (!why) assert(slot_is(0, &C));
        else assert(!slot_is(0, &C));
        assert(slot_is(1, &B));
        f_flip = 0;
        fwlib_cancel();
        assert(!upload(&C, C.sha, 1, NULL, C.n) && slot_is(0, &C));          /* and it can be repeated */
    }

    /* cut the power during every flash operation of an install that also saves an image */
    fresh();
    long base = f_ops;
    assert(!upload(&C, C.sha, 1, NULL, C.n));
    long total = f_ops - base;
    long repaired = 0;
    for (long k = 0; k < total; k++) {
        fresh();
        srand((unsigned)k + 1);
        f_ops = 0;
        f_cut = k;
        if (!setjmp(f_power)) { upload(&C, C.sha, 1, NULL, C.n); assert(!"the power should have gone"); }
        f_cut = -1;
        fwlib_cancel();                                  /* what a restart does */
        assert(slot_is(1, &B));
        slots_get(1, &in1);
        assert(in1.confirmed);
        /* whatever the loader would start is A, B or C, whole */
        slots_get(0, &in);
        if (in.valid) {
            uint8_t d[32]; sha256_t c;
            sha256_init(&c); sha256_update(&c, F[1] + slots_addr(0) + SLOT_HDR, in.hdr.size); sha256_final(&c, d);
            if (!memcmp(d, in.hdr.sha, 32)) assert(slot_is(0, &A) || slot_is(0, &C));
        }
        /* and anything the library claims to hold is whole, or is refused untouched */
        for (int i = 0; i < FWLIB_COUNT; i++) {
            fwlib_get(i, &e);
            if (!e.valid) continue;
            if (!fwlib_rollback(e.hdr.sha, 1, NULL)) {
                const char *why = run();
                if (why) repaired++;
                assert(slot_is(1, &B));
            }
        }
        if (upload(&C, C.sha, 1, NULL, C.n)) {           /* damaged copies go once found */
            assert(strstr(fwlib_error(), "damaged"));
            assert(!upload(&C, C.sha, 1, NULL, C.n));
        }
        assert(slot_is(0, &C));
    }
    printf("power cut at each of %ld flash operations: running image intact every time (%ld damaged copies refused)\n", total, repaired);
    puts("ok");
    return 0;
}
