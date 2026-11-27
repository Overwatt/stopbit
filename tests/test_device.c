/* test_device.c — the engine (src/app/device.c) with the EG4 profile, a real Modbus queue
 * (modbus.c) and a fake line and clock.
 *   see tests/host.sh for the build line */
#include "device.h"
#include "../tests/fuzz/json_ok.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
static modbus_t M[PORTS];
static bool on[PORTS];
modbus_t *bridge_modbus_port(int p) { return on[p] ? &M[p] : NULL; }

static uint8_t L[4096]; static size_t Ln;
static size_t line(void *ctx, const uint8_t *d, size_t n) { (void)ctx; memcpy(L + Ln, d, n); Ln += n; return n; }
static void reply(void *ctx, int c, const uint8_t *d, size_t n) { (void)ctx; (void)c; (void)d; (void)n; }
static void local(void *ctx, uint32_t tag, int st, const uint8_t *pdu, size_t n) { device_reply((int)(intptr_t)ctx, tag, st, pdu, n); }
static int gone_n; static char gone_name[25];
static void gone(int i, const device_t *d) { gone_n++; assert(i >= 0 && (i == 1 || gone_n > 1)); snprintf(gone_name, sizeof gone_name, "%s", d->cfg.name); }

static void run(void) { device_poll(); modbus_poll(&M[1], t_now, 1000000, true); modbus_poll(&M[1], t_now, 1000000, true); }
static void answer(const uint8_t *f, size_t n)             /* a frame without its CRC */
{
    uint8_t b[300];
    memcpy(b, f, n);
    uint16_t c = modbus_crc(b, n);
    b[n] = (uint8_t)c; b[n + 1] = (uint8_t)(c >> 8);
    modbus_from_line(&M[1], b, n + 2);
    modbus_poll(&M[1], t_now, 0, true);
    modbus_poll(&M[1], t_now, 1000000, true);
}
/* What went out: unit, start, count. */
static bool sent(uint8_t unit, uint16_t start, uint16_t count)
{
    bool ok = Ln == 8 && L[0] == unit && L[1] == 3 && (L[2] << 8 | L[3]) == start && (L[4] << 8 | L[5]) == count;
    if (!ok) printf("sent %zu: %02x %02x %02x%02x %02x%02x, wanted %02x %04x %u\n", Ln, L[0], L[1], L[2], L[3], L[4], L[5], unit, start, count);
    Ln = 0;
    return ok;
}
static uint32_t tag(int i, int b) { return (uint32_t)device_get(i)->gen << 16 | (uint32_t)i << 8 | (uint32_t)b; }
static size_t live1(uint8_t *pdu, uint16_t volts)            /* a reply PDU: function, count, 18 registers */
{
    memset(pdu, 0, 38);
    pdu[0] = 3; pdu[1] = 36;
    pdu[2] = (uint8_t)(volts >> 8); pdu[3] = (uint8_t)volts;
    for (int k = 0; k < 16; k++) { pdu[6 + 2 * k] = 0x0C; pdu[7 + 2 * k] = 0xE5; }   /* 3.301 V */
    return 38;
}
static bool known(int dev, const char *id) { const device_t *d = device_get(dev); return pf_ok(&d->st, profile_field(d->p, id)); }

int main(void)
{
    static cfg_t c;
    static uint8_t pdu[300];
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    const dev_cfg_t D[4] = {
        { "eg4-bms", "eg4m", "", "", 2, 0x10, 0, 0 },
        { "eg4-bms", "p1", "", "", 2, 1, 0, 0 },
        { "nope", "x", "", "", 2, 3, 0, 0 },
        { "eg4-bms", "p3", "", "", 3, 1, 0, 0 },
    };
    memcpy(c.dev, D, sizeof D);
    device_init();
    device_on_gone(gone);
    device_configure(&c);
    assert(device_get(0)->p && device_get(1)->p == device_get(0)->p && !device_get(2)->p && device_get(4) == NULL);
    assert(strstr(device_get(2)->error, "No profile 'nope'"));

    /* Turns: each device's blocks in order, devices alternating; then nothing until due. */
    const mb_io_t io = { line, reply, local, (void *)(intptr_t)1 };
    modbus_init(&M[1], &io, 1000, 2000);
    on[1] = true;
    static const uint16_t BL[5][2] = { { 0x0000, 18 }, { 0x0012, 21 }, { 0x0069, 11 }, { 0x0075, 3 }, { 0x0078, 8 } };
    for (int b = 0; b < 5; b++)
        for (int dv = 0; dv < 2; dv++) {
            run();
            assert(sent(dv ? 1 : 0x10, BL[b][0], BL[b][1]));
            answer((const uint8_t[]){ dv ? 1 : 0x10, 0x83, 2 }, 3);        /* "illegal address": a miss */
        }
    assert(M[1].turn_us == 50000);
    assert(strstr(device_get(3)->error, "Its port is off"));                /* port 3 is not on */
    run();
    assert(Ln == 0);
    assert(device_get(0)->misses == 5 && strstr(device_get(0)->why, "Error 2 from the device"));
    t_now += 10000;
    run();
    assert(sent(0x10, 0x0000, 18));                                         /* due again */

    /* A reply: values, online. */
    uint32_t s0 = device_get(0)->seq;
    size_t n = live1(pdu, 5312);
    uint8_t fr[60] = { 0x10 };
    memcpy(fr + 1, pdu, n);
    answer(fr, n + 1);
    const device_t *d0 = device_get(0);
    assert(d0->online && d0->seq > s0 && known(0, "total_voltage") && known(0, "min_cell_voltage"));
    assert(d0->st.val[profile_field(d0->p, "total_voltage")].v > 53.11f);
    { int up[PORTS], all[PORTS];                                            /* each port's: one of 3 online */
      device_count(up, all);
      assert(!all[0] && all[1] == 3 && all[2] == 1 && !all[3] && !up[0] && up[1] == 1 && !up[2] && !up[3]); }

    /* Misses: after `stale` (5) in a row the block's values go, and the device is offline. */
    for (int k = 0; k < 4; k++) device_reply(1, tag(0, 0), -1, NULL, 0);
    assert(known(0, "total_voltage") && d0->online);
    device_reply(1, tag(0, 0), -1, NULL, 0);
    assert(!known(0, "total_voltage") && !known(0, "power") && !d0->online && (d0->stale & 1));
    assert(strstr(d0->why, "No reply (live1)"));
    /* A reply the profile rejects: a miss, values kept. */
    device_reply(1, tag(0, 0), 0, pdu, live1(pdu, 5312));
    assert(d0->online && known(0, "total_voltage") && !(d0->stale & 1));
    device_reply(1, tag(0, 0), 0, pdu, live1(pdu, 0));
    assert(known(0, "total_voltage") && strstr(d0->why, "A reply its profile rejects"));
    /* The wrong size, and a reply for a device since changed: */
    pdu[1] = 34;
    device_reply(1, tag(0, 0), 0, pdu, 36);
    assert(strstr(d0->why, "A reply of the wrong size"));
    uint32_t m0 = d0->misses;
    device_reply(1, (uint32_t)(d0->gen + 1) << 16 | 0, -1, NULL, 0);
    assert(d0->misses == m0);

    /* Reconfigure one device: only it starts afresh, and HA is told it went. */
    c.dev[1].address = 2;
    device_configure(&c);
    assert(gone_n == 1 && !strcmp(gone_name, "p1") && device_get(1)->polls == 0 && known(0, "total_voltage"));

    /* A request whose end never comes (still queued): the backstop frees the port after 90 s. */
    Ln = 0;
    t_now += 20000;
    device_poll();                                       /* queued */
    t_now += 89000;
    device_poll();
    assert(M[1].qn == 1);
    t_now += 2000;
    device_poll();
    assert(M[1].qn == 2);
    M[1].qn = 0; M[1].state = MB_IDLE;
    run(); assert(Ln == 8);                              /* it polls on */
    Ln = 0;

    /* A configure with our request still on the line must not add another. */
    Ln = 0;
    device_configure(&c);
    t_now += 10000;
    device_poll();
    { int nl = 0; for (int q = 0; q < M[1].qn; q++) nl += M[1].q[q].client == MB_LOCAL; assert(nl == 1); }
    /* A re-init drops the queue without a callback: the next poll polls again. */
    M[1].qn = 0; M[1].state = MB_IDLE;
    t_now += 10000;
    run(); assert(Ln == 8);
    Ln = 0;
    M[1].qn = 0; M[1].state = MB_IDLE;

    /* The profile's line against the port's: a mismatch is reported, polling goes on. */
    c.port[1].baud = 19200;
    device_configure(&c);
    assert(!strcmp(device_get(0)->error, "The profile wants 9600 8N1; the port is 19200 8N1"));
    t_now += 10000;
    run();
    assert(Ln == 8 && !strcmp(device_get(0)->error, "The profile wants 9600 8N1; the port is 19200 8N1"));
    Ln = 0;
    M[1].qn = 0; M[1].state = MB_IDLE;
    c.port[1].baud = 9600; c.port[1].parity = PARITY_EVEN;
    device_configure(&c);
    assert(!strcmp(device_get(0)->error, "The profile wants 9600 8N1; the port is 9600 8E1"));
    on[1] = false;
    device_poll();
    assert(strstr(device_get(0)->error, "Its port is off"));          /* off wins */
    on[1] = true;
    device_poll();
    assert(strstr(device_get(0)->error, "The profile wants"));
    c.port[1].parity = PARITY_NONE;
    device_configure(&c);
    assert(device_get(0)->error[0] == 0);
    device_poll();
    assert(device_get(0)->error[0] == 0);
    Ln = 0;
    M[1].qn = 0; M[1].state = MB_IDLE;

    /* The line's use, over a minute. */
    device_reply(1, tag(0, 0), -1, NULL, 0);                 /* a timeout: the line was ours for its wait */
    t_now += 61000;
    device_poll();
    assert(device_line_use(1) > 0 && device_line_use(1) <= 1000);

    /* As /api/devices shows it. */
    static char j[4600];
    n = device_json(0, j, sizeof j);
    assert(n && json_ok(j) && strstr(j, "\"name\":\"eg4m\"") && strstr(j, "\"total_voltage\""));
    for (size_t k = 0; k < n; k += 97) assert(!device_json(0, j, k));
    assert(device_json(2, j, sizeof j) > 0 && json_ok(j) && strstr(j, "No profile 'nope'") && strstr(j, "\"values\":[]"));

    n = device_json(0, j, sizeof j);                                  /* the unit keeps its UTF-8 */
    assert(n && strstr(j, ",\"\xC2\xB0" "C\"]") && !strstr(j, "??C"));

    /* The port off for more than 2^31 ms: the device still polls when it is back. */
    on[1] = false;
    device_poll();
    for (int k = 0; k < 3; k++) { t_now += 740000000u; device_poll(); }   /* 25.7 days */
    on[1] = true;
    M[1].qn = 0; M[1].state = MB_IDLE; Ln = 0;
    run(); assert(Ln == 8);
    Ln = 0; M[1].qn = 0; M[1].state = MB_IDLE;

    /* The last device on a port removed: no extra quiet, no line use. */
    device_reply(1, tag(0, 0), -1, NULL, 0);
    t_now += 61000;
    device_poll();
    assert(M[1].turn_us == 50000 && device_line_use(1) > 0);
    for (int k = 0; k < 3; k++) c.dev[k].profile[0] = 0;
    device_configure(&c);
    device_poll();
    assert(M[1].turn_us == 0 && device_line_use(1) == 0);

    /* A long bits text comes out whole. */
    {
        static pf_build_t B;
        static profile_t P;
        static device_t X;
        char names[400] = "profile t 1\nblock b hold 0 2 10s\ntext w \"W\" b 0 bits", e[64];
        for (int k = 0; k < 14; k++) { snprintf(e, sizeof e, " %d=\"Bit name number %d\"", k, k); strcat(names, e); }
        strcat(names, "\n");
        assert(profile_parse(&B, names, strlen(names), e, sizeof e));
        profile_view(&B, &P);
        pf_state_init(&X.st);
        X.p = &P;
        const uint8_t r[4] = { 0x3F, 0xFF, 0, 0 };
        assert(pf_decode(&P, 0, r, 4, &X.st));
        static char v[700], want[600];
        size_t w = 0;
        for (int k = 0; k < 14; k++) w += (size_t)snprintf(want + w, sizeof want - w, "%sBit name number %d", k ? "; " : "", k);
        assert(w > 160);
        assert(device_value(&X, 0, v, sizeof v) == w + 2 && v[0] == '"' && !strncmp(v + 1, want, w) && v[w + 1] == '"');
    }
    puts("device: ok");
    return 0;
}
