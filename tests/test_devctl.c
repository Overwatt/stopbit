/* test_devctl.c — writes to devices' controls (src/app/device.c, pfwrite.c): a command
 * queued, its block read, written only if that changes it, the block read back; refusals,
 * failures and their events. Own profile source; a real Modbus queue, a fake line and clock.
 *   see tests/host.sh for the build line */
#include "device.h"
#include "devmqtt.h"
#include "../tests/fuzz/json_ok.h"
#include "events.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
extern int ev_n, ev_port;
extern ev_kind_t ev_kind;
extern uint32_t ev_a;
extern char ev_s[64];
static modbus_t M;
static bool port2_off;
modbus_t *bridge_modbus_port(int p) { return p == 1 && !port2_off ? &M : NULL; }

static const char *T =
    "profile c 1\n"
    "name \"Controlled\"\n"
    "block live input 0 2 10s\n"
    "block set hold 20 6 1m info\n"
    "sensor v \"V\" live 0 u16\n"
    "number lim \"Limit\" set 20 u16 *0.1 min 0 max 25 kW\n"
    "switch ac \"AC\" set 21 bit 7\n"
    "time t1 \"Start\" set 22\n"
    "number big \"Big\" set 23 u32 min 0 max 100000\n"
    "select by \"By\" set 25 mask=0x000E 0=Off 2=Time 4=Voltage 6=SOC hidden\n";
const char *profiles_id(int i) { return i ? NULL : "c"; }
/* A second profile, for a number whose step is finer than its display precision. */
static const char *T2 =
    "profile d 1\n"
    "name \"Fine\"\n"
    "block set hold 0 1 1m info\n"
    "number st \"Step\" set 0 u16 *0.1 min 0 max 25 prec=0 kW\n";
const char *profiles_text(const char *id, size_t *len)
{
    const char *t = !strcmp(id, "c") ? T : !strcmp(id, "d") ? T2 : NULL;
    if (t) *len = strlen(t);
    return t;
}

static uint8_t L[512]; static size_t Ln;
static size_t line(void *ctx, const uint8_t *d, size_t n) { (void)ctx; memcpy(L + Ln, d, n); Ln += n; return n; }
static void reply(void *ctx, int c, const uint8_t *d, size_t n) { (void)ctx; (void)c; (void)d; (void)n; }
static void local(void *ctx, uint32_t tag, int st, const uint8_t *pdu, size_t n) { (void)ctx; device_reply(1, tag, st, pdu, n); }
static const mb_io_t IO = { line, reply, local, NULL };
static void run(void) { device_poll(); modbus_poll(&M, t_now, 1000000, true); modbus_poll(&M, t_now, 1000000, true); }
static void answer(const uint8_t *pdu, size_t n)           /* from unit 1, its CRC added */
{
    uint8_t b[300];
    b[0] = 1;
    memcpy(b + 1, pdu, n);
    uint16_t c = modbus_crc(b, n + 1);
    b[n + 1] = (uint8_t)c; b[n + 2] = (uint8_t)(c >> 8);
    modbus_from_line(&M, b, n + 3);
    modbus_poll(&M, t_now, 0, true);
    modbus_poll(&M, t_now, 1000000, true);
}
static bool sent(const uint8_t *pdu, size_t n)             /* what went to unit 1: this PDU */
{
    bool ok = Ln == n + 3 && L[0] == 1 && !memcmp(L + 1, pdu, n);
    if (!ok) { printf("sent %zu:", Ln); for (size_t k = 0; k < Ln; k++) printf(" %02x", L[k]); printf("\n"); }
    Ln = 0;
    return ok;
}
#define PDU(...) (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ })
#define SET_READ PDU(3, 0, 20, 0, 6)
static void set_reply(uint16_t lim, uint16_t ac, uint16_t t1, uint32_t big, uint16_t by)   /* the set block's six registers */
{
    const uint16_t r[6] = { lim, ac, t1, (uint16_t)(big >> 16), (uint16_t)big, by };
    uint8_t p[14] = { 3, 12 };
    for (int k = 0; k < 6; k++) { p[2 + 2 * k] = (uint8_t)(r[k] >> 8); p[3 + 2 * k] = (uint8_t)r[k]; }
    answer(p, sizeof p);
}
static bool event(int outcome, const char *s)
{
    bool ok = ev_kind == EV_WRITE && ev_port == 2 && ev_a == (1u | (uint32_t)outcome << 8) && !strcmp(ev_s, s);
    if (!ok) printf("event %d port %d a %lx \"%s\"\n", ev_kind, ev_port, (unsigned long)ev_a, ev_s);
    return ok;
}
static const char *val(const char *id) { static char v[32]; const device_t *d = device_get(0); assert(device_value(d, profile_field(d->p, id), v, sizeof v)); return v; }

static struct { char topic[160], payload[8192]; } Pb[64];
static int Pn;
static bool pub(const char *t, const char *p, bool r)
{
    (void)r;
    int k = 0;
    while (k < Pn && strcmp(Pb[k].topic, t)) k++;              /* the latest for each topic */
    if (k == Pn) { assert(Pn < 64); Pn++; }
    snprintf(Pb[k].topic, sizeof Pb[k].topic, "%s", t);
    snprintf(Pb[k].payload, sizeof Pb[k].payload, "%s", p);
    return true;
}
static void drain(void) { for (int k = 0; k < 400; k++) assert(devmqtt_poll(pub, "solar", "n540-test", true, t_now)); }
static const char *got(const char *t) { for (int k = 0; k < Pn; k++) if (!strcmp(Pb[k].topic, t)) return Pb[k].payload; return NULL; }

static void off_drain(void)                                    /* every waiting command failed, port 2 back on */
{
    port2_off = true;
    device_poll();
    port2_off = false;
    modbus_init(&M, &IO, 1000, 2000);
    Ln = 0;
}
static int fill(cfg_t *c)                                      /* commands queued of 20 tried, over 5 devices on port 2 */
{
    static const char *const CT[4] = { "lim", "ac", "t1", "big" }, *const PV[4] = { "1", "OFF", "09:00", "5" };
    int queued = 0;
    for (int i = 1; i < 5; i++) { c->dev[i] = c->dev[0]; snprintf(c->dev[i].name, sizeof c->dev[i].name, "inv%d", i); c->dev[i].address = (uint8_t)(i + 1); }
    device_configure(c);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) {
            char nm[25];
            if (i) snprintf(nm, sizeof nm, "inv%d", i); else snprintf(nm, sizeof nm, "inv");
            queued += device_command(nm, CT[k], PV[k], false);
        }
    return queued;
}

int main(void)
{
    static cfg_t c;
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    c.dev[0] = (dev_cfg_t){ "c", "inv", "", "", 2, 1, 1, 0 };          /* controls on */
    device_init();
    device_configure(&c);
    modbus_init(&M, &IO, 1000, 2000);
    assert(device_get(0)->p);

    run(); assert(sent(PDU(4, 0, 0, 0, 2))); answer(PDU(4, 4, 0, 1, 0, 2));
    run(); assert(sent(SET_READ)); set_reply(100, 0xF355, 0x0001, 0, 0);
    assert(!strcmp(val("lim"), "10.0") && !strcmp(val("ac"), "false") && !strcmp(val("t1"), "\"01:00\"") && !strcmp(val("by"), "\"Off\""));

    /* A switch: its block read (as a poll reads it, its values taken), written with its bit
     * alone changed, the block read back. */
    int e0 = ev_n;
    t_now += 600;
    assert(device_command("inv", "ac", "ON", false));
    run(); assert(sent(SET_READ));
    set_reply(100, 0xF355, 0x0001, 0, 0);
    assert(device_get(0)->last_ok == t_now);
    run(); assert(sent(PDU(6, 0, 21, 0xF3, 0xD5)));
    answer(PDU(6, 0, 21, 0xF3, 0xD5));
    run(); assert(sent(SET_READ));
    set_reply(100, 0xF3D5, 0x0001, 0, 0);
    assert(ev_n == e0 + 1 && event(0, "ac=ON") && !strcmp(val("ac"), "true"));

    /* Already so: read, not written, and the block read again so HA sees what it holds. */
    t_now += 600;
    assert(device_command("inv", "ac", "ON", false));
    run(); assert(sent(SET_READ));
    set_reply(100, 0xF3D5, 0x0001, 0, 0);
    assert(event(1, "ac=ON"));
    run(); assert(sent(SET_READ));
    set_reply(100, 0xF3D5, 0x0001, 0, 0);

    /* A number, scaled; the device keeps something else: said so. */
    t_now += 600;
    assert(device_command("inv", "lim", "12.5", false));
    run(); assert(sent(SET_READ));
    set_reply(100, 0xF3D5, 0x0001, 0, 0);
    run(); assert(sent(PDU(6, 0, 20, 0, 125)));
    answer(PDU(6, 0, 20, 0, 125));
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 0, 0);
    assert(event(4, "lim=12.5; 12.0"));

    /* Two registers, by function 16. */
    t_now += 600;
    assert(device_command("inv", "big", "70000", false));
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 0, 0);
    run(); assert(sent(PDU(16, 0, 23, 0, 2, 4, 0, 1, 0x11, 0x70)));
    answer(PDU(16, 0, 23, 0, 2));
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 70000, 0);
    assert(event(0, "big=70000"));

    /* The device refuses, or does not answer: said so. */
    t_now += 600;
    assert(device_command("inv", "t1", "07:30", false));
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 70000, 0);
    run(); assert(sent(PDU(6, 0, 22, 0x1E, 0x07)));
    answer(PDU(0x86, 3));
    assert(event(3, "t1=07:30; error 3"));
    run(); assert(sent(SET_READ));                             /* a failed write: its block read at once */
    set_reply(120, 0xF3D5, 0x0001, 70000, 0);
    t_now += 600;
    assert(device_command("inv", "t1", "07:30", false));
    run(); assert(sent(SET_READ));
    t_now += 1000; run();
    assert(event(3, "t1=07:30; no reply"));
    /* The write unanswered: failed, and the block read at once, as it may have been made. */
    t_now += 600;
    assert(device_command("inv", "t1", "07:30", false));
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 70000, 0);
    run(); assert(sent(PDU(6, 0, 22, 0x1E, 0x07)));
    t_now += 1000; run();
    assert(event(3, "t1=07:30; no reply"));
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x1E07, 70000, 0);
    assert(!strcmp(val("t1"), "\"07:30\""));                  /* it was: HA shows it */

    /* Refused, each with its event: nothing queued, nothing sent. */
    assert(!device_command("inv", "lim", "30", false) && event(2, "lim=30; out of range"));
    assert(!device_command("inv", "v", "1", false) && event(2, "v=1; no such control"));
    assert(!device_command("inv", "nope", "1", false) && event(2, "nope=1; no such control"));
    assert(!device_command("inv", "ac", "ON", true) && event(2, "ac=ON; retained"));
    assert(!device_command("x", "ac", "ON", false) && ev_port == 0 && ev_a == 2u << 8 && !strcmp(ev_s, "x/ac=ON; no such device"));
    run(); assert(Ln == 0);

    /* Controls off: refused. */
    c.dev[0].controls = 0;
    device_configure(&c);
    assert(!device_command("inv", "ac", "ON", false) && event(2, "ac=ON; controls off"));
    c.dev[0].controls = 1;
    device_configure(&c);
    run(); assert(sent(PDU(4, 0, 0, 0, 2))); answer(PDU(4, 4, 0, 1, 0, 2));
    run(); assert(sent(SET_READ)); set_reply(120, 0xF3D5, 0x0001, 70000, 0);

    /* A newer command for a queued control replaces its value and keeps its place; writes go
     * before polls, 2 a second at most per device. */
    t_now += 600;
    assert(device_command("inv", "lim", "5", false) && device_command("inv", "t1", "08:00", false) && device_command("inv", "lim", "6", false));
    run(); assert(sent(SET_READ));
    set_reply(120, 0xF3D5, 0x0001, 70000, 0);
    run(); assert(sent(PDU(6, 0, 20, 0, 60)));
    answer(PDU(6, 0, 20, 0, 60));
    run(); assert(sent(SET_READ));
    set_reply(60, 0xF3D5, 0x0001, 70000, 0);
    assert(event(0, "lim=6"));
    run(); assert(Ln == 0);                                    /* t1 waits: 500 ms since lim's */
    t_now += 500;
    run(); assert(sent(SET_READ));
    set_reply(60, 0xF3D5, 0x0001, 70000, 0);
    run(); assert(sent(PDU(6, 0, 22, 0, 8)));
    answer(PDU(6, 0, 22, 0, 8));
    run(); assert(sent(SET_READ));
    set_reply(60, 0xF3D5, 0x0008, 70000, 0);
    assert(event(0, "t1=08:00"));

    /* Written, its read-back on the line when the port's queue starts again: the write was
     * accepted, so it is said so, and the block read at once. */
    t_now += 600;
    assert(device_command("inv", "lim", "7", false));
    run(); assert(sent(SET_READ));
    set_reply(60, 0xF3D5, 0x0008, 70000, 0);
    run(); assert(sent(PDU(6, 0, 20, 0, 70)));
    answer(PDU(6, 0, 20, 0, 70));
    run(); assert(sent(SET_READ));
    modbus_init(&M, &IO, 1000, 2000);
    run();
    assert(event(0, "lim=7; not read back"));
    run(); assert(sent(SET_READ));
    set_reply(70, 0xF3D5, 0x0008, 70000, 0);
    assert(!strcmp(val("lim"), "7.0"));
    /* The same with the write itself on the line: failed, and the block read at once too. */
    t_now += 600;
    assert(device_command("inv", "lim", "8", false));
    run(); assert(sent(SET_READ));
    set_reply(70, 0xF3D5, 0x0008, 70000, 0);
    run(); assert(sent(PDU(6, 0, 20, 0, 80)));
    modbus_init(&M, &IO, 1000, 2000);
    run();
    assert(event(3, "lim=8; lost"));
    run(); assert(sent(SET_READ));
    set_reply(80, 0xF3D5, 0x0008, 70000, 0);
    assert(!strcmp(val("lim"), "8.0"));

    /* 16 waiting at most, across devices. */
    for (int i = 1; i < 5; i++) { c.dev[i] = c.dev[0]; snprintf(c.dev[i].name, sizeof c.dev[i].name, "inv%d", i); c.dev[i].address = (uint8_t)(i + 1); }
    device_configure(&c);
    static const char *const CT[4] = { "lim", "ac", "t1", "big" }, *const PV[4] = { "1", "OFF", "09:00", "5" };
    int queued = 0;
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) {
            char nm[25];
            if (i) snprintf(nm, sizeof nm, "inv%d", i); else snprintf(nm, sizeof nm, "inv");
            queued += device_command(nm, CT[k], PV[k], false);
        }
    assert(queued == 16 && ev_a == (5u | 2u << 8) && !strcmp(ev_s, "big=5; too many waiting"));

    /* A write on the line when the port's queue starts again (as on a settings change):
     * given up, and said so. */
    t_now += 600;
    run(); assert(sent(SET_READ));
    modbus_init(&M, &IO, 1000, 2000);
    run();
    assert(event(3, "lim=1; lost"));

    /* Fix round 1. Start clean: nothing waiting, one device. */
    off_drain();
    for (int i = 1; i < 5; i++) c.dev[i] = (dev_cfg_t){ 0 };
    device_configure(&c);

    /* A device on an off port (3): refused. */
    c.dev[1] = c.dev[0];
    snprintf(c.dev[1].name, sizeof c.dev[1].name, "off3");
    c.dev[1].port = 3;
    device_configure(&c);
    assert(!device_command("off3", "ac", "ON", false) && ev_kind == EV_WRITE && ev_port == 3 && ev_a == (2u | 2u << 8) && !strcmp(ev_s, "ac=ON; port off"));
    c.dev[1] = (dev_cfg_t){ 0 };
    device_configure(&c);

    /* Queued, then the port goes off: failed, the places free. */
    t_now += 600;
    assert(device_command("inv", "lim", "2", false));
    port2_off = true;
    device_poll();
    assert(event(3, "lim=2; port off"));
    port2_off = false;
    modbus_init(&M, &IO, 1000, 2000);
    assert(fill(&c) == 16);                                    /* all 16 places were free */
    off_drain();
    for (int i = 1; i < 5; i++) c.dev[i] = (dev_cfg_t){ 0 };
    device_configure(&c);

    /* A device changed while its write's read is on the line. */
    c.dev[0].controls = 0; device_configure(&c); c.dev[0].controls = 1; device_configure(&c);
    modbus_init(&M, &IO, 1000, 2000);
    for (int k = 0; k < 4; k++) { run(); Ln = 0; t_now += 600; }
    assert(device_command("inv", "t1", "09:00", false));
    t_now += 600;
    for (int k = 0; k < 5 && Ln == 0; k++) run();
    assert(sent(SET_READ));
    c.dev[0].address = 1;                                      /* same address; change the label instead */
    snprintf(c.dev[0].label, sizeof c.dev[0].label, "changed");
    device_configure(&c);
    set_reply(60, 0xF3D5, 0x0001, 70000, 0);
    assert(event(3, "t1=09:00; device changed"));

    /* The last device on the port removed while its write is on the line, the queue started again. */
    run(); run(); Ln = 0;
    t_now += 600;
    assert(device_command("inv", "t1", "10:00", false));
    t_now += 600;
    for (int k = 0; k < 5 && Ln == 0; k++) run();
    assert(sent(SET_READ));
    c.dev[0] = (dev_cfg_t){ 0 };
    device_configure(&c);
    modbus_init(&M, &IO, 1000, 2000);
    run();
    assert(ev_kind == EV_WRITE && ev_port == 0 && ev_a == (1u | 3u << 8) && !strcmp(ev_s, "t1=10:00; lost"));   /* its device is gone: no port to name */
    c.dev[0] = (dev_cfg_t){ "c", "inv", "", "", 2, 1, 1, 0 };
    assert(fill(&c) == 16);                                    /* the slot was freed */
    /* Discovery: each control as its HA kind, with its command topic; a hidden one disabled.
     * From a clean slate: every device gone (their waiting commands with them), one back. */
    memset(c.dev, 0, sizeof c.dev);
    device_configure(&c);
    c.dev[0] = (dev_cfg_t){ "c", "inv", "", "", 2, 1, 1, 0 };
    device_configure(&c);
    modbus_init(&M, &IO, 1000, 2000);                           /* the write on the line goes with it */
    devmqtt_init();
    drain();
    const char *n = got("homeassistant/number/n540-test_inv/lim/config"), *s;
    assert(n && json_ok(n) && strstr(n, "\"command_topic\":\"~/set/lim\"") && strstr(n, "\"entity_category\":\"config\"") &&
           strstr(n, "\"min\":0.0,\"max\":25.0,\"step\":0.1,\"mode\":\"box\"") && strstr(n, "\"unit_of_measurement\":\"kW\""));
    s = got("homeassistant/switch/n540-test_inv/ac/config");
    assert(s && json_ok(s) && strstr(s, "{{ 'ON' if value_json['ac'] else 'OFF' }}") && strstr(s, "\"command_topic\":\"~/set/ac\""));
    s = got("homeassistant/text/n540-test_inv/t1/config");
    assert(s && json_ok(s) && strstr(s, "\"pattern\":\"^([01][0-9]|2[0-3]):[0-5][0-9]$\",\"min\":5,\"max\":5"));
    s = got("homeassistant/select/n540-test_inv/by/config");
    assert(s && json_ok(s) && strstr(s, "\"options\":[\"Off\",\"Time\",\"Voltage\",\"SOC\"]") && strstr(s, "\"enabled_by_default\":false"));
    s = got("homeassistant/sensor/n540-test_inv/v/config");
    assert(s && !strstr(s, "command_topic") && !strstr(s, "enabled_by_default"));
    run(); assert(sent(PDU(4, 0, 0, 0, 2))); answer(PDU(4, 4, 0, 1, 0, 2));
    run(); assert(sent(SET_READ)); set_reply(60, 0xF3D5, 0x0008, 70000, 4);
    t_now += 2000; drain();
    s = got("solar/dev/inv/state");
    assert(s && json_ok(s) && strstr(s, "\"lim\":6.0") && strstr(s, "\"ac\":true") && strstr(s, "\"t1\":\"08:00\"") && strstr(s, "\"by\":\"Voltage\""));
    /* /api/devices: a device in parts, whole JSON together (before the reconfigure below,
     * which restarts the device and clears its values). */
    static char j[16384];
    int w0 = device_json_part(0, 0, j, sizeof j);
    assert(w0 > 0 && device_json_part(0, 2, j + w0, sizeof j - (size_t)w0) == 2 && !strcmp(j + w0, "]}"));
    assert(device_json(0, j, sizeof j) && json_ok(j) && strstr(j, "[\"lim\",\"Limit\",6.0,\"kW\"]"));
    /* Controls off: their configs emptied, the sensor's kept. */
    c.dev[0].controls = 0;
    device_configure(&c);
    drain();
    assert(!strcmp(got("homeassistant/number/n540-test_inv/lim/config"), "") && !strcmp(got("homeassistant/select/n540-test_inv/by/config"), ""));
    assert(got("homeassistant/sensor/n540-test_inv/v/config")[0] == '{');
    /* A number's step is announced in full, though it shows no decimals (prec=0). */
    memset(c.dev, 0, sizeof c.dev);
    device_configure(&c);
    c.dev[0] = (dev_cfg_t){ "d", "fine", "", "", 2, 1, 1, 0 };
    device_configure(&c);
    devmqtt_init();
    drain();
    s = got("homeassistant/number/n540-test_fine/st/config");
    assert(s && json_ok(s) && strstr(s, "\"min\":0.0,\"max\":25.0,\"step\":0.1,"));
    /* A device whose profile is unknown: its JSON is whole, with empty values, in parts or together. */
    memset(c.dev, 0, sizeof c.dev);
    device_configure(&c);
    c.dev[0] = (dev_cfg_t){ "nope", "lost", "", "", 2, 1, 0, 0 };
    device_configure(&c);
    assert(device_get(0) && !device_get(0)->p);
    static char j2[4096], j3[4096];
    size_t tn = device_json(0, j2, sizeof j2);
    assert(tn && json_ok(j2) && strstr(j2, "\"values\":[]"));
    size_t pn = 0;
    for (int k = 0; k < DEV_PARTS; k++) { int w = device_json_part(0, k, j3 + pn, sizeof j3 - pn); assert(w >= 0); pn += (size_t)w; }
    assert(pn == tn && !strcmp(j2, j3));
    /* A device removed between part 0 and part 2: part 0 as it was, the rest after. */
    int w0b = device_json_part(0, 0, j3, sizeof j3);
    assert(w0b > 0);
    memset(c.dev, 0, sizeof c.dev);
    device_configure(&c);
    assert(!device_get(0));
    assert(device_json_part(0, 1, j3 + w0b, sizeof j3 - (size_t)w0b) == 0);
    assert(device_json_part(0, 2, j3 + w0b, sizeof j3 - (size_t)w0b) == 2);
    assert(json_ok(j3));
    puts("devctl: ok");
    return 0;
}
