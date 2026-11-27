/* test_devroom.c — the room device.c packs profiles into: several of different sizes at
 * once, one's place taken by another once nobody uses it, and each still whole after the
 * others have come and gone. Own profile source; the room small, so that it runs out.
 *   cc -DD2_BSS= -DPF_ROOM=4096 -Itests/stub -Isrc/app tests/test_devroom.c src/app/device.c \
 *      src/app/modbus.c src/app/profile.c src/app/pfdecode.c src/app/cfg.c src/app/acl.c -lm */
#include "device.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
modbus_t *bridge_modbus_port(int p) { (void)p; return NULL; }

/* Profile "pN": N sensors s0..s(N-1), named "Sensor <id> k". */
static char T[16384];
static int count_of(const char *id) { int n = 0; return id[0] == 'p' && sscanf(id + 1, "%d", &n) == 1 && n > 0 && n <= 125 ? n : 0; }
const char *profiles_id(int i) { (void)i; return NULL; }
const char *profiles_text(const char *id, size_t *len)
{
    int n = count_of(id), w;
    if (!n) return NULL;
    w = snprintf(T, sizeof T, "profile %s 1\nname \"Model %s\"\nblock b hold 0 %d 10s\n", id, id, n);
    for (int k = 0; k < n; k++) w += snprintf(T + w, sizeof T - (size_t)w, "sensor s%d \"Sensor %s %d\" b %d u16\n", k, id, k, k);
    *len = (size_t)w;
    return T;
}

static cfg_t c;
static void dev(int i, const char *profile)
{
    memset(&c.dev[i], 0, sizeof c.dev[i]);
    if (profile) { snprintf(c.dev[i].profile, sizeof c.dev[i].profile, "%s", profile); snprintf(c.dev[i].name, sizeof c.dev[i].name, "d%d", i); c.dev[i].port = 2; c.dev[i].address = (uint8_t)(i + 1); }
    device_configure(&c);
}

/* Device i's profile is the one named, all of it. */
static void whole(int i, const char *id)
{
    const device_t *d = device_get(i);
    char want[64];
    int n = count_of(id);
    if (!d->p) puts(d->error);
    assert(d->p && !strcmp(pf_str(d->p, d->p->id), id) && d->p->nfield == n && d->p->nblock == 1 && d->p->block[0].count == n);
    snprintf(want, sizeof want, "Model %s", id);
    assert(!strcmp(pf_str(d->p, d->p->name), want));
    for (int k = 0; k < n; k++) {
        snprintf(want, sizeof want, "Sensor %s %d", id, k);
        assert(!strcmp(pf_str(d->p, d->p->field[k].name), want) && d->p->field[k].addr == k);
    }
}

int main(void)
{
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    device_init();

    /* Four different profiles at once, two devices sharing one; a fifth is told why not. */
    dev(0, "p3"); dev(1, "p10"); dev(2, "p5"); dev(3, "p8"); dev(4, "p10");
    whole(0, "p3"); whole(1, "p10"); whole(2, "p5"); whole(3, "p8"); whole(4, "p10");
    assert(device_get(1)->p == device_get(4)->p);
    dev(5, "p2");
    assert(!device_get(5)->p && strstr(device_get(5)->error, "More than 4 different profiles"));
    dev(5, "nope");
    assert(!device_get(5)->p && strstr(device_get(5)->error, "No profile 'nope'"));
    dev(5, NULL);

    /* One goes; its profile stays for whoever asks again, until its place is wanted. */
    const profile_t *was = device_get(0)->p;
    dev(0, NULL);
    dev(0, "p3");
    assert(device_get(0)->p == was);
    whole(0, "p3");
    dev(0, NULL);
    dev(0, "p4");                                       /* bigger than the gap p3 left: goes after the rest */
    whole(0, "p4"); whole(1, "p10"); whole(2, "p5"); whole(3, "p8"); whole(4, "p10");
    dev(2, NULL);
    dev(2, "p2");                                       /* into a gap */
    whole(0, "p4"); whole(1, "p10"); whole(2, "p2"); whole(3, "p8"); whole(4, "p10");

    /* A profile held (as devmqtt.c holds one while it clears a removed device from HA) keeps its place. */
    const profile_t *held = device_get(3)->p;
    device_hold(held, 1);
    dev(3, NULL);
    dev(3, "p6");
    assert(!device_get(3)->p && strstr(device_get(3)->error, "More than 4 different profiles"));
    assert(!strcmp(pf_str(held, held->id), "p8") && held->nfield == 8);
    device_hold(held, -1);
    dev(3, NULL);
    dev(3, "p6");
    whole(0, "p4"); whole(1, "p10"); whole(2, "p2"); whole(3, "p6"); whole(4, "p10");

    /* The room runs out before the places do: said so, and nothing else is disturbed. */
    for (int i = 0; i < 5; i++) dev(i, NULL);
    dev(0, "p20"); dev(1, "p20");
    whole(0, "p20");
    dev(2, "p30");
    assert(!device_get(2)->p && strstr(device_get(2)->error, "No room for another profile"));
    dev(3, "p40");
    assert(!device_get(3)->p && strstr(device_get(3)->error, "No room for another profile"));
    whole(0, "p20"); whole(1, "p20");
    dev(0, NULL); dev(1, NULL); dev(2, NULL); dev(3, NULL);
    dev(2, "p30");                                      /* now there is */
    whole(2, "p30");
    dev(3, "p120");                                     /* one that could never fit this room */
    assert(!device_get(3)->p && strstr(device_get(3)->error, "No room for another profile"));
    whole(2, "p30");

    /* Many comings and goings: whoever is there is whole. */
    for (int i = 0; i < 6; i++) dev(i, NULL);
    static const char *const ID[] = { "p1", "p7", "p12", "p3", "p9", "p15", "p2" };
    const char *at[4] = { 0 };
    uint32_t r = 1;
    for (int k = 0; k < 2000; k++) {
        r = r * 1664525u + 1013904223u;
        int i = (int)(r >> 28) % 4;
        const char *id = (r >> 20 & 3) == 0 ? NULL : ID[(r >> 12) % 7];
        dev(i, id);
        at[i] = id && device_get(i)->p ? id : NULL;
        for (int j = 0; j < 4; j++) if (at[j]) whole(j, at[j]);
    }
    puts("devroom: ok");
    return 0;
}
