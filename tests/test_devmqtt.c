/* test_devmqtt.c — devices' MQTT topics and payloads (src/app/devmqtt.c): discovery for
 * every field, status, state, pacing, removal, HA naming; each built-in profile's longest
 * state and device_json. */
#include "devmqtt.h"
#include "device.h"
#include "profiles.h"
#include "../tests/fuzz/json_ok.h"
#include "stm32h7xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t t_now = 1000;
uint32_t SystemCoreClock = 400000000;
modbus_t *bridge_modbus_port(int p) { (void)p; return NULL; }

static struct { char topic[160], payload[4096]; bool retain; } Pb[400];
static int Pn;
static bool full;
static size_t max_cfg, max_state;
static bool pub(const char *t, const char *p, bool r)
{
    if (full) return false;
    assert(Pn < 400 && strlen(p) < sizeof Pb[0].payload);
    if (strstr(t, "/state")) { if (strlen(p) > max_state) max_state = strlen(p); }
    else if (strstr(t, "/config") && strlen(p) > max_cfg) max_cfg = strlen(p);
    snprintf(Pb[Pn].topic, sizeof Pb[Pn].topic, "%s", t);
    snprintf(Pb[Pn].payload, sizeof Pb[Pn].payload, "%s", p);
    Pb[Pn++].retain = r;
    return true;
}
static int drain(bool announce) { int n0 = Pn; for (int k = 0; k < 1000; k++) { int b = Pn; assert(devmqtt_poll(pub, "solar", "n540-test", announce, t_now)); if (Pn == b) break; } return Pn - n0; }
static const char *find(const char *topic) { for (int k = Pn - 1; k >= 0; k--) if (!strcmp(Pb[k].topic, topic)) return Pb[k].payload; return NULL; }
static uint32_t tag(int i, int b) { return (uint32_t)device_get(i)->gen << 16 | (uint32_t)i << 8 | (uint32_t)b; }

int main(void)
{
    static cfg_t c;
    static uint8_t pdu[60];
    cfg_defaults(&c);
    c.port[1].mode = MODE_MODBUS;
    c.dev[0] = (dev_cfg_t){ "eg4-bms", "eg4m", "EG4 master", "", 2, 0x10, 0, 0 };
    device_init();
    devmqtt_init();
    device_configure(&c);
    const device_t *d = device_get(0);
    memset(pdu, 0, sizeof pdu);
    pdu[0] = 3; pdu[1] = 36; pdu[2] = 0x14; pdu[3] = 0xC0;                       /* 53.12 V */
    for (int k = 0; k < 16; k++) { pdu[6 + 2 * k] = 0x0C; pdu[7 + 2 * k] = 0xE5; }
    device_reply(1, tag(0, 0), 0, pdu, 38);
    assert(d->online);

    /* Discovery: one config per field, then status, then state. */
    devmqtt_restart();
    int n = drain(true);
    assert(n == d->p->nfield + 2);
    for (int f = 0; f < d->p->nfield; f++) {
        const pf_field_t *x = &d->p->field[f];
        char t[160];
        bool bin = x->kind == PF_BINARY || x->kind == PF_CALCBIN;
        snprintf(t, sizeof t, "homeassistant/%s/n540-test_eg4m/%s/config", bin ? "binary_sensor" : "sensor", pf_str(d->p, x->id));
        const char *p = find(t);
        assert(p && json_ok(p) && strlen(p) < 1600);
        assert(strstr(p, "\"~\":\"solar/dev/eg4m\"") && strstr(p, "\"availability_mode\":\"all\"") && strstr(p, "\"via_device\":\"n540-test\""));
        char u[120];
        snprintf(u, sizeof u, "\"unique_id\":\"n540-test_eg4m_%s\"", pf_str(d->p, x->id));
        assert(strstr(p, u));
        snprintf(u, sizeof u, "\"default_entity_id\":\"%s.n540_eg4m_%s\"", bin ? "binary_sensor" : "sensor", pf_str(d->p, x->id));
        assert(strstr(p, u));
    }
    const char *tv = find("homeassistant/sensor/n540-test_eg4m/total_voltage/config");
    const char *tc = find("homeassistant/sensor/n540-test_eg4m/pcb_temperature/config");
    assert(tc && strstr(tc, "\"unit_of_measurement\":\"\xC2\xB0" "C\"") && !strstr(tc, "??C"));
    assert(strstr(tv, "\"unit_of_measurement\":\"V\"") && strstr(tv, "\"device_class\":\"voltage\"") &&
           strstr(tv, "\"state_class\":\"measurement\"") && strstr(tv, "\"suggested_display_precision\":2") &&
           strstr(tv, "\"name\":\"EG4 master\"") && strstr(tv, "\"manufacturer\":\"EG4\""));
    const char *ht = find("homeassistant/binary_sensor/n540-test_eg4m/heating/config");
    assert(strstr(ht, "'ON' if value_json['heating'] else 'OFF'") && !strstr(ht, "device_class"));   /* the profile gives heating none (as the ESPHome component) */
    assert(strstr(find("homeassistant/sensor/n540-test_eg4m/min_voltage_cell/config"), "\"entity_category\":\"diagnostic\""));
    assert(!strcmp(find("solar/dev/eg4m/status"), "online"));
    const char *s = find("solar/dev/eg4m/state");
    assert(s && json_ok(s) && strlen(s) < 3584 && strstr(s, "\"total_voltage\":53.12") && strstr(s, "\"pcb_temperature\":null") &&
           strstr(s, "\"heating\":null") && strstr(s, "\"cell_voltage_16\":3.301"));
    for (int k = 0; k < Pn; k++) assert(Pb[k].retain);

    /* Pacing: a change is sent no sooner than a second after the last, and without one a minute later. */
    assert(drain(true) == 0);
    pdu[3] = 0xC1;
    device_reply(1, tag(0, 0), 0, pdu, 38);
    assert(drain(true) == 0);
    t_now += 1000;
    assert(drain(true) == 1 && strstr(find("solar/dev/eg4m/state"), "\"total_voltage\":53.13"));
    t_now += 59999;
    assert(drain(true) == 0);
    t_now += 1;
    assert(drain(true) == 1);

    /* Every bit of warnings, protection and error set: the longest state; it must still go out whole. */
    {
        uint8_t l2[44] = { 3, 42 };
        uint16_t m[21] = { 31, 25, 33, 150, 100, 98, 76, 0x0001, 0x3FFF, 0x3FFF, 0x0017, 1, 2, 0, 0, 0x19E7, 0x1A1B, 0x1C1D, 0, 2000, 0 };
        for (int k = 0; k < 21; k++) { l2[2 + 2 * k] = (uint8_t)(m[k] >> 8); l2[3 + 2 * k] = (uint8_t)m[k]; }
        device_reply(1, tag(0, 1), 0, l2, 44);
        t_now += 1000;
        assert(drain(true) == 1);
        s = find("solar/dev/eg4m/state");
        assert(json_ok(s) && strstr(s, "Pack OV; Cell OV; Pack UV; Cell UV; Charge OC; Discharge OC; Abnormal Temp; MOS Overheat;"));
        const char *w = strstr(s, "\"warnings\":\"");
        assert(w && strchr(w + 13, '"') - (w + 13) > 100 && strlen(s) > 600 && strstr(s, "\"protection\":\"Pack OV"));
        printf("long state: %zu bytes\n", strlen(s));
    }

    /* No room: the same thing again next time. */
    full = true;
    devmqtt_restart();
    assert(!devmqtt_poll(pub, "solar", "n540-test", true, t_now));
    full = false;
    assert(drain(true) == d->p->nfield + 2);

    /* Discovery off: the configs go empty. */
    devmqtt_restart();
    Pn = 0;
    drain(false);
    assert(!strcmp(find("homeassistant/sensor/n540-test_eg4m/total_voltage/config"), ""));

    /* ESPHome naming: the ESP32's entity IDs. */
    c.dev[0].ids = 1;
    snprintf(c.dev[0].prefix, sizeof c.dev[0].prefix, "eg4_bms");
    Pn = 0;
    device_configure(&c);                                   /* a changed device: removed, then new */
    drain(true);
    assert(strstr(find("homeassistant/sensor/n540-test_eg4m/total_voltage/config"), "\"default_entity_id\":\"sensor.eg4_bms_total_voltage\""));
    assert(strstr(find("homeassistant/binary_sensor/n540-test_eg4m/heating/config"), "\"default_entity_id\":\"binary_sensor.eg4_bms_heating\""));
    assert(strstr(find("homeassistant/sensor/n540-test_eg4m/cell_voltage_3/config"), "\"default_entity_id\":\"sensor.eg4_bms_cell_voltage_3\""));

    {   /* esphome ids: the unique_id carries the prefix, so HA treats it as a new entity */
        const char *tv2 = find("homeassistant/sensor/n540-test_eg4m/total_voltage/config");
        assert(strstr(tv2, "\"unique_id\":\"n540-test_eg4m_eg4_bms_total_voltage\"") && !strstr(tv2, "\"unique_id\":\"n540-test_eg4m_total_voltage\""));
        int emptied = 0;                                    /* the old config (own ids) was emptied first */
        for (int k = 0; k < Pn; k++) emptied += !strcmp(Pb[k].topic, "homeassistant/sensor/n540-test_eg4m/total_voltage/config") && !Pb[k].payload[0];
        assert(emptied == 1);
        assert(strstr(tv2, "value_json['total_voltage']") && !strstr(tv2, "value_json."));
    }

    /* Removed: every config, the state and the status emptied. */
    int nf = d->p->nfield;
    memset(&c.dev[0], 0, sizeof c.dev[0]);
    Pn = 0;
    device_configure(&c);
    drain(true);
    assert(Pn == nf + 2);
    int empty = 0;
    for (int k = 0; k < Pn; k++) empty += Pb[k].payload[0] == 0;
    assert(empty == Pn && find("solar/dev/eg4m/state") && find("solar/dev/eg4m/status"));

    /* Each built-in profile at its longest: every field known, each number as long as pf_num
     * prints one at its precision, every bit of a bits text set, an enum at its longest name
     * (or "Unknown (4294967295)"), ascii texts full of quotes (each escaped), the device's
     * own strings and counts at their longest. The state must fit devmqtt.c's BUF (3584),
     * and device_json one part of GET /api/devices: http.c's RESP_MAX 4600 less GEN_HEAD 8
     * and the chunk's CRLF is 4590, less the ',' api_devices puts before all but the first. */
    for (int pi = 0; profiles_id(pi); pi++) {
        static char j[4590 - 1];
        char st[80], v[520];
        memset(&c.dev[0], 0, sizeof c.dev[0]);
        snprintf(c.dev[0].profile, sizeof c.dev[0].profile, "%s", profiles_id(pi));
        memset(c.dev[0].name, 'z', sizeof c.dev[0].name - 1);
        memset(c.dev[0].label, '"', sizeof c.dev[0].label - 1);
        memset(c.dev[0].prefix, 'z', sizeof c.dev[0].prefix - 1);
        c.dev[0].port = 2; c.dev[0].address = 247; c.dev[0].ids = 1; c.dev[0].controls = 1;
        device_configure(&c);
        device_t *w = (device_t *)device_get(0);           /* values set by hand */
        assert(w && w->p);
        const profile_t *p = w->p;
        for (int f = 0; f < p->nfield; f++) {
            const pf_field_t *x = &p->field[f];
            w->st.ok[f >> 3] |= (uint8_t)(1u << (f & 7));
            if (x->kind == PF_SENSOR || x->kind == PF_CALC) {
                double a = 1.7e19;                          /* pf_num: |v| x 10^prec below 1.8e19 */
                for (int k = 0; k < pf_prec(x) && k < 6; k++) a /= 10;
                w->st.val[f].v = (float)-a;
            } else if (x->kind == PF_BITS) {
                w->st.val[f].raw = 0xFFFFFFFFu;
            } else if (x->kind == PF_ASCII) {
                memset(w->st.str[x->str], '"', PF_STR_LEN - 1);
            } else if (x->kind == PF_ENUM) {
                uint32_t best = 0xFFFFFFFFu;
                w->st.val[f].raw = best;
                size_t bl = device_value(w, f, v, sizeof v);
                for (int k = 0; k < x->nvname; k++) {
                    w->st.val[f].raw = p->vname[x->vname + k].value;
                    size_t l = device_value(w, f, v, sizeof v);
                    if (l > bl) { bl = l; best = w->st.val[f].raw; }
                }
                w->st.val[f].raw = best;
            } else {
                w->st.val[f].v = 0;                         /* "false" */
            }
        }
        memset(w->error, 'x', sizeof w->error - 1);
        memset(w->why, 'x', sizeof w->why - 1);
        w->polls = w->misses = 0xFFFFFFFFu;
        w->seq++;
        t_now += 60000;
        w->last_ok = t_now + 1;                             /* ago_ms 2147483647 */
        Pn = 0;
        assert(drain(true) == p->nfield + 2);               /* nothing left out: every config, the status, the state */
        snprintf(st, sizeof st, "solar/dev/%s/state", c.dev[0].name);
        s = find(st);
        assert(s && s[0] && json_ok(s) && strlen(s) < 3584);
        size_t jn = device_json(0, j, sizeof j);
        assert(jn && json_ok(j));
        printf("worst case %s: state %zu bytes (BUF 3584), device_json %zu (part 4589)\n", profiles_id(pi), strlen(s), jn);
    }
    memset(&c.dev[0], 0, sizeof c.dev[0]);
    device_configure(&c);
    drain(true);
    printf("largest config %zu, largest state %zu (BUF 3584)\n", max_cfg, max_state);
    assert(max_cfg < 3584 && max_state < 3584);
    puts("devmqtt: ok");
    return 0;
}
