/* cc -Isrc/app tests/test_button.c src/app/{button,cfg}.c -o /tmp/b && /tmp/b */
#include "button.h"
#include "cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t T;
static int fired;
static void run(button_t *b, bool raw, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++, T++) fired += button_step(b, raw, T);
}
static int press(button_t *b, uint32_t ms)
{
    fired = 0;
    run(b, true, ms);
    run(b, false, 200);
    return fired;
}

int main(void)
{
    static const uint32_t start[] = { 0, 1000, 0xFFFFFFFFu - 2000u, 0xFFFFFFFFu - 40u };   /* tick wrap too */
    for (int i = 0; i < 4; i++) {
        button_t b = { 0 };
        T = start[i];
        run(&b, false, 100);
        assert(!press(&b, 20) && b.presses == 0);          /* shorter than the debounce */
        assert(!press(&b, 100) && b.presses == 1 && b.held_ms == 100);
        assert(!press(&b, 2999));
        assert(press(&b, 3000) == 1);
        assert(press(&b, 6097) == 1 && b.held_ms == 6097);
        assert(press(&b, 14999) == 1);
        assert(!press(&b, 15000));
        assert(!press(&b, 60000));
        fired = 0;                                         /* contact bounce at both ends */
        for (int k = 0; k < 10; k++) { run(&b, k & 1, 5); }
        run(&b, true, 4000);
        for (int k = 0; k < 10; k++) { run(&b, k & 1, 5); }
        run(&b, false, 200);
        assert(fired == 1);
        fired = 0;                                         /* a short drop-out does not end it */
        run(&b, true, 2000); run(&b, false, 10); run(&b, true, 2000); run(&b, false, 200);
        assert(fired == 1);
        fired = 0;                                         /* two short presses do not add up */
        run(&b, true, 2000); run(&b, false, 100); run(&b, true, 2000); run(&b, false, 200);
        assert(fired == 0);
        run(&b, true, 2990); assert(!button_armed(&b, T)); /* the blink */
        run(&b, true, 100);  assert(button_armed(&b, T));
        run(&b, true, 12000); assert(!button_armed(&b, T));
        run(&b, false, 200); assert(!button_armed(&b, T));
    }
    button_t h = { 0 };                                    /* held since before power-on */
    T = 0; fired = 0;
    run(&h, true, 5000); run(&h, false, 200);
    assert(fired == 1);

    cfg_t c, d;                                            /* what it changes, and nothing else */
    cfg_defaults(&c);
    c.net.dhcp = 0; c.net.ip = 1; c.net.mask = 2; c.net.gw = 3; c.net.dns = 4;
    c.web_set = 1; memset(c.web_salt, 7, sizeof c.web_salt); memset(c.web_key, 9, sizeof c.web_key);
    strcpy(c.device_name, "x"); strcpy(c.mqtt.host, "broker"); strcpy(c.mqtt.password, "pw");
    c.mqtt.enabled = 1; c.port[2].baud = 115200; c.port[2].enabled = 1;
    d = c;
    cfg_reset_access(&c);
    assert(c.net.dhcp == 1 && !c.net.ip && !c.net.mask && !c.net.gw && !c.net.dns && !c.web_set);
    for (unsigned i = 0; i < sizeof c.web_key; i++) assert(!c.web_key[i]);
    for (unsigned i = 0; i < sizeof c.web_salt; i++) assert(!c.web_salt[i]);
    c.net = d.net; c.web_set = d.web_set;
    memcpy(c.web_key, d.web_key, sizeof c.web_key); memcpy(c.web_salt, d.web_salt, sizeof c.web_salt);
    assert(!memcmp(&c, &d, sizeof c));
    puts("ok");
    return 0;
}
