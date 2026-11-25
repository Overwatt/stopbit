/* cc -w -Itests/stub -Isrc/app -Isrc/common -DWDT_H '-Dwdt_feed()=(void)0' \
 *    tests/test_store.c src/app/{cfg_store,cfg,acl}.c src/common/sha256.c -o /tmp/s && /tmp/s */
#include "cfg_store.h"
#include "sha256.h"
#include "stm32h7xx_hal.h"
#include "flash_sim.h"
#include <assert.h>
void NVIC_SystemReset(void) { abort(); }

static void set(cfg_t *c, const char *t)
{ char b[512], e[96]; strcpy(b, t); if (!cfg_parse(c, b, e, sizeof e)) { puts(e); abort(); } }
static void is(const cfg_t *c, int p, const char *want)
{ char t[ACL_TEXT]; acl_text(&c->access[p], t); if (strcmp(t, want)) { printf("port %d: %s, wanted %s\n", p + 1, t, want); abort(); } }

int main(void)
{
    cfg_t c, d;
    f_no[f_nno++] = (range_t){ 1, 0, 0x0F0000 };
    f_no[f_nno++] = (range_t){ 1, 0x0F4000, 0x200000 };
    f_no[f_nno++] = (range_t){ 0, 0, 0x200000 };
    memset(F, 0xFF, sizeof F);
    assert(!cfg_load(&c));
    is(&c, 0, "");

    set(&c, "port1.baud=38400\nport1.listen_only=1\nport1.enabled=1");
    assert(cfg_save(&c));
    for (int a = 0xF2000; a < 0xF4000; a++) assert(F[1][a] == 0xFF);         /* no lists: nothing written */
    set(&c, "port1.access=10.20.1.54 read\nport3.access=10.0.0.0/8 write");
    assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c));
    is(&d, 0, "10.20.1.54 read"); is(&d, 2, "10.0.0.0/8 write");
    long ops = f_ops;
    set(&c, "port1.name=x");
    assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c));
    is(&d, 0, "10.20.1.54 read");

    /* what older firmware checks: schema 2, exactly 656 bytes, checksum over them */
    for (int i = 0; i < 2; i++) {
        uint32_t h[4]; uint8_t dg[32]; sha256_t s;
        memcpy(h, F[1] + 0xF0000 + 0x1000 * i, 16);
        assert(h[0] == 0x47464335u && h[1] == 2 && h[3] == 656);
        sha256_init(&s); sha256_update(&s, h + 1, 12); sha256_update(&s, F[1] + 0xF0000 + 0x1000 * i + 48, 656); sha256_final(&s, dg);
        assert(!memcmp(dg, F[1] + 0xF0000 + 0x1000 * i + 16, 32));
    }

    /* one copy damaged: the other is used. Both: nobody gets in, the rest still loads. */
    uint8_t keep[2][4096]; memcpy(keep[0], F[1] + 0xF2000, 4096); memcpy(keep[1], F[1] + 0xF3000, 4096);
    set(&c, "port1.access=10.20.1.99 read"); assert(cfg_save(&c));
    for (int i = 0; i < 2; i++) {
        uint8_t was = F[1][0xF2000 + 0x1000 * i + 60];
        F[1][0xF2000 + 0x1000 * i + 60] ^= 1;
        assert(cfg_load(&d));
        char t[ACL_TEXT]; acl_text(&d.access[0], t);
        assert(!strcmp(t, "10.20.1.54 read") || !strcmp(t, "10.20.1.99 read"));
        F[1][0xF2000 + 0x1000 * i + 60] = was;
    }
    F[1][0xF2000 + 60] ^= 1; F[1][0xF3000 + 60] ^= 1;
    assert(cfg_load(&d));
    for (int p = 0; p < PORTS; p++) assert(acl_role(&d.access[p], 0x36014B0Au) == ACL_DENY);
    assert(d.port[0].listen_only && d.port[0].baud == 38400);
    set(&d, "port1.access=10.20.1.54 read\nport2.access=\nport3.access=\nport4.access=");
    assert(cfg_save(&d) && cfg_load(&c)); is(&c, 0, "10.20.1.54 read"); is(&c, 1, "");

    /* junk that is not ours is not a list */
    memset(F, 0xFF, sizeof F);
    for (int a = 0xF2000; a < 0xF4000; a++) F[1][a] = (uint8_t)rand();
    cfg_load(&c); is(&c, 0, "");

    /* power cut at every flash operation, from both starting points */
    for (int start = 0; start < 2; start++) {
        static uint8_t img[0x4000];
        memset(F, 0xFF, sizeof F);
        cfg_load(&c);
        set(&c, "port1.listen_only=1\nport1.baud=19200"); assert(cfg_save(&c));
        if (start) { set(&c, "port1.access=10.20.1.54 read"); assert(cfg_save(&c)); set(&c, "port2.access=1.1.1.1 read"); assert(cfg_save(&c)); }
        cfg_t old = c;
        memcpy(img, F[1] + 0xF0000, sizeof img);
        set(&c, "port1.access=10.20.1.99 write\nport1.baud=57600");
        f_ops = 0; assert(cfg_save(&c)); long total = f_ops;
        for (long k = 0; k < total; k++)
            for (int rep = 0; rep < 20; rep++) {
                memcpy(F[1] + 0xF0000, img, sizeof img);
                cfg_load(&d);
                f_ops = 0; f_cut = k; srand((unsigned)(k * 100 + rep));
                if (!setjmp(f_power)) { cfg_save(&c); assert(!"cut"); }
                f_cut = -1;
                assert(cfg_load(&d));
                assert(d.port[0].listen_only && (d.port[0].baud == 19200 || d.port[0].baud == 57600));
                assert(!memcmp(d.access, old.access, sizeof old.access) || !memcmp(d.access, c.access, sizeof c.access));
                assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c));   /* and it can be saved again */
            }
        printf("power cut at each of %ld operations: old or new, never neither\n", total);
    }
    (void)ops;
    puts("ok");
    return 0;
}
