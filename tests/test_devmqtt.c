/* test_devmqtt.c — devices' MQTT topics and payloads (src/app/devmqtt.c): discovery for
 * every field, status, state, pacing, removal, HA naming. */
#include "devmqtt.h"
#include "device.h"
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
    assert(strstr(ht, "'ON' if value_json.heating else 'OFF'") && !strstr(ht, "device_class"));   /* the profile gives heating none (as the ESPHome component) */
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
    printf("largest config %zu, largest state %zu (BUF 3584)\n", max_cfg, max_state);
    assert(max_cfg < 3584 && max_state < 3584);
    puts("devmqtt: ok");
    return 0;
}
