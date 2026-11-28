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
    f_no[f_nno++] = (range_t){ 1, 0, 0x0EE000 };
    f_no[f_nno++] = (range_t){ 1, 0x0F4000, 0x0FA000 };          /* HTTPS and renewal records */
    f_no[f_nno++] = (range_t){ 1, 0x0FC000, 0x200000 };
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

    /* later settings: their own record, nothing else rewritten for them */
    memset(F, 0xFF, sizeof F);
    cfg_load(&c); assert(!c.more.ntp[0]);
    for (int a = 0xFA000; a < 0xFC000; a++) assert(F[1][a] == 0xFF);
    set(&c, "net.ntp=time.example.com"); assert(cfg_save(&c));
    assert(cfg_load(&d) && !strcmp(d.more.ntp, "time.example.com") && !memcmp(&c, &d, sizeof c));
    { const uint32_t *h = (const uint32_t *)(F[1] + 0xFA000); assert(h[0] == 0x45524F4Du && h[1] == 1 && h[3] == 256); }
    set(&c, "net.ntp=10.20.0.1"); assert(cfg_save(&c)); assert(cfg_load(&d) && !strcmp(d.more.ntp, "10.20.0.1"));
    set(&c, "port1.baud=4800"); static uint8_t was[0x2000]; memcpy(was, F[1] + 0xFA000, sizeof was);
    assert(cfg_save(&c) && !memcmp(was, F[1] + 0xFA000, sizeof was));               /* unchanged: not rewritten */
    for (int a = 0xFA000; a < 0xFC000; a++) F[1][a] ^= 0x5A;                       /* damaged: defaults, not lockout */
    assert(cfg_load(&d) && !d.more.ntp[0] && d.port[0].baud == 4800);
    for (int p = 0; p < PORTS; p++) assert(acl_role(&d.access[p], 0x36014B0Au) != ACL_DENY);

    /* modbus_tcp: a bit in the later settings' spare byte; a blank one (every older record)
     * means listen. Power cut at every flash operation of the save that turns it off, from no
     * later-settings copy and from one: old or new, never neither. */
    for (int start = 0; start < 2; start++) {
        static uint8_t img[0x4000], imm[0x2000];         /* main and lists; later settings */
        memset(F, 0xFF, sizeof F);
        cfg_load(&c); assert(cfg_modbus_tcp(&c, 1));
        set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502"); assert(cfg_save(&c));
        if (start) { set(&c, "net.ntp=10.20.0.1"); assert(cfg_save(&c)); }
        cfg_t old = c;
        memcpy(img, F[1] + 0xF0000, sizeof img); memcpy(imm, F[1] + 0xFA000, sizeof imm);
        set(&c, "port2.modbus_tcp=0\nport2.baud=19200");
        f_ops = 0; assert(cfg_save(&c)); long total = f_ops;
        assert(cfg_load(&d) && !cfg_modbus_tcp(&d, 1) && cfg_modbus_tcp(&d, 0) && !memcmp(&c, &d, sizeof c));
        for (long k = 0; k < total; k++)
            for (int rep = 0; rep < 20; rep++) {
                memcpy(F[1] + 0xF0000, img, sizeof img); memcpy(F[1] + 0xFA000, imm, sizeof imm);
                cfg_load(&d);
                f_ops = 0; f_cut = k; srand((unsigned)(k * 100 + rep));
                if (!setjmp(f_power)) { cfg_save(&c); assert(!"cut"); }
                f_cut = -1;
                assert(cfg_load(&d));
                assert(!memcmp(&d.more, &old.more, sizeof old.more) || !memcmp(&d.more, &c.more, sizeof c.more));
                assert(d.port[1].mode == MODE_MODBUS && d.port[1].local_port == 502);
                assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c));   /* and it can be saved again */
            }
        printf("power cut at each of %ld operations of a save turning TCP off: old or new, never neither\n", total);
    }

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
    /* Devices: their own record, 0x0EE000 / 0x0EF000; a damaged one means none. */
    set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\ndev3.profile=eg4-bms\ndev3.name=eg4m\ndev3.port=2\ndev3.address=16");
    assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c) && !strcmp(d.dev[2].name, "eg4m"));
    {
        int at = ((const uint32_t *)(F[1] + 0xEE000))[0] == 0x56454444u ? 0xEE000 : 0xEF000;
        const uint32_t *h = (const uint32_t *)(F[1] + at);
        assert(h[0] == 0x56454444u && h[1] == 1 && h[3] == DEVS * sizeof(dev_cfg_t));
        F[1][at + 100] ^= 1;
    }
    assert(cfg_load(&d) && !d.dev[2].profile[0] && d.port[1].mode == MODE_MODBUS);   /* the rest as it was */

    /* power cut at every flash operation of a save that changes devices (and the main
     * record): each is old or new, never neither, and the settings save again. From one
     * device copy written, and from both. */
    for (int start = 0; start < 2; start++) {
        static uint8_t img[0x6000];                      /* 0x0EE000 to 0x0F4000: devices, main, lists */
        memset(F, 0xFF, sizeof F);
        cfg_load(&c);
        set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\ndev1.profile=eg4-bms\ndev1.name=old\ndev1.port=2\ndev1.address=16");
        assert(cfg_save(&c));
        if (start) { set(&c, "dev2.profile=eg4-bms\ndev2.name=two\ndev2.port=2\ndev2.address=17"); assert(cfg_save(&c)); }
        cfg_t old = c;
        memcpy(img, F[1] + 0xEE000, sizeof img);
        set(&c, "dev1.name=new\ndev1.address=20\ndev2.profile=\ndev3.profile=eg4-bms\ndev3.name=three\ndev3.port=2\ndev3.address=30\nport2.baud=19200");
        f_ops = 0; assert(cfg_save(&c)); long total = f_ops;
        for (long k = 0; k < total; k++)
            for (int rep = 0; rep < 20; rep++) {
                memcpy(F[1] + 0xEE000, img, sizeof img);
                cfg_load(&d);
                f_ops = 0; f_cut = k; srand((unsigned)(k * 100 + rep));
                if (!setjmp(f_power)) { cfg_save(&c); assert(!"cut"); }
                f_cut = -1;
                assert(cfg_load(&d));
                assert(!memcmp(d.dev, old.dev, sizeof old.dev) || !memcmp(d.dev, c.dev, sizeof c.dev));
                assert(d.port[1].mode == MODE_MODBUS && (d.port[1].baud == 9600 || d.port[1].baud == 19200));
                assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c));   /* and it can be saved again */
            }
        printf("power cut at each of %ld operations of a save with devices: old or new, never neither\n", total);
    }
    /* power cut at every flash operation of a save that clears a port's address, and of
     * one that sets it: the port is never left off */
    for (int rev = 0; rev < 2; rev++) {
        static uint8_t img[0xE000];   /* 0x0EE000 to 0x0FC000: devices, main, lists, more */
        memset(F, 0xFF, sizeof F);
        cfg_load(&c);
        set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\nport2.ip=10.20.0.12");
        uint32_t a12 = cfg_port_ip(&c, 1);
        if (rev) set(&c, "port2.ip=");
        assert(cfg_save(&c));
        memcpy(img, F[1] + 0xEE000, sizeof img);
        set(&c, rev ? "port2.ip=10.20.0.12" : "port2.ip=");
        cfg_load(&d);
        f_ops = 0; assert(cfg_save(&c)); long total = f_ops;
        for (long k = 0; k < total; k++)
            for (int rep = 0; rep < 20; rep++) {
                memcpy(F[1] + 0xEE000, img, sizeof img);
                cfg_load(&d);
                f_ops = 0; f_cut = k; srand((unsigned)(k * 100 + rep));
                if (!setjmp(f_power)) { cfg_save(&c); assert(!"cut"); }
                f_cut = -1;
                assert(cfg_load(&d));
                assert(d.port[1].enabled && d.port[1].local_port == 502 && (!cfg_port_ip(&d, 1) || cfg_port_ip(&d, 1) == a12));
                assert(cfg_save(&c) && cfg_load(&d) && !memcmp(&c, &d, sizeof c));
            }
        printf("power cut at each of %ld operations of a save %s a port's address: on, old or new address\n", total, rev ? "setting" : "clearing");
    }
    /* power cut at every flash operation of a save that clears two ports' addresses and
     * moves one of them off the shared local port: valid, and old, new, or both ports off */
    {
        static uint8_t img[0xE000];
        char e[96];
        memset(F, 0xFF, sizeof F);
        cfg_load(&c);
        set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\nport2.ip=10.20.0.12\n"
                "port3.enabled=1\nport3.mode=modbus\nport3.local_port=502\nport3.ip=10.20.0.11");
        assert(cfg_save(&c));
        cfg_t old = c;
        memcpy(img, F[1] + 0xEE000, sizeof img);
        set(&c, "port2.ip=\nport3.ip=\nport3.local_port=503");
        cfg_load(&d);
        f_ops = 0; assert(cfg_save(&c)); long total = f_ops;
        for (long k = 0; k < total; k++)
            for (int rep = 0; rep < 20; rep++) {
                memcpy(F[1] + 0xEE000, img, sizeof img);
                cfg_load(&d);
                f_ops = 0; f_cut = k; srand((unsigned)(k * 100 + rep));
                if (!setjmp(f_power)) { cfg_save(&c); assert(!"cut"); }
                f_cut = -1;
                assert(cfg_load(&d) && cfg_validate(&d, e, sizeof e));
                assert(!memcmp(&d, &old, sizeof d) || !memcmp(&d, &c, sizeof d) || (!d.port[1].enabled && !d.port[2].enabled));
            }
        printf("power cut at each of %ld operations of a save clearing two ports' addresses: valid, old, new or off\n", total);
    }
    /* Ports at addresses of their own, both on 502: the main record has them off, so older
     * firmware, which reads it alone, still accepts it; this firmware reads them back on. */
    {
        char e[96];
        cfg_defaults(&c);
        set(&c, "port2.enabled=1\nport2.mode=modbus\nport2.local_port=502\nport2.ip=10.20.0.12\n"
                "port3.enabled=1\nport3.mode=modbus\nport3.local_port=502\nport3.ip=10.20.0.11\n"
                "port4.enabled=0\nport4.ip=10.20.0.13\nport1.enabled=1\nport1.listen_only=1");
        assert(cfg_validate(&c, e, sizeof e));
        assert(cfg_save(&c));
        uint32_t s0, s1, m0, m1;
        memcpy(&m0, F[1] + 0xF0000, 4); memcpy(&s0, F[1] + 0xF0008, 4);
        memcpy(&m1, F[1] + 0xF1000, 4); memcpy(&s1, F[1] + 0xF1008, 4);
        bool second = m1 == 0x47464335u && (m0 != 0x47464335u || s1 > s0);
        static cfg_t old;
        cfg_defaults(&old);
        memcpy(&old, F[1] + (second ? 0xF1000 : 0xF0000) + 48, 656);  /* what older firmware reads */
        assert(old.port[0].enabled && old.port[0].listen_only && !old.port[1].enabled && !old.port[2].enabled && !old.port[3].enabled);
        assert(cfg_validate(&old, e, sizeof e));
        assert(cfg_load(&d) && !memcmp(&c, &d, sizeof c));
        assert(d.port[1].enabled && d.port[2].enabled && !d.port[3].enabled && cfg_port_ip(&d, 3) && !d.more.ip_enabled);
        assert(cfg_save(&d) && cfg_load(&c) && !memcmp(&c, &d, sizeof c));   /* and again */
    }
    (void)ops;
    puts("ok");
    return 0;
}
