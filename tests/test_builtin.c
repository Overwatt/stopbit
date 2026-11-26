/* test_builtin.c — every built-in profile parses; the EG4 one decodes as the ESPHome
 * component does (eg4_bms.cpp), with the identification strings of its REGISTER_MAP.md.
 *   python3 tools/embed_profiles.py /tmp/profiles_gen.h profiles/*.txt &&
 *   cc -I/tmp -Isrc/app tests/test_builtin.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c -lm -o /tmp/bi && /tmp/bi */
#include "profiles.h"
#include "pfdecode.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static profile_t P;
static pf_state_t S;

static int F(const char *id) { int i = profile_field(&P, id); if (i < 0) printf("no field %s\n", id); assert(i >= 0); return i; }
static float V(const char *id) { int i = F(id); assert(pf_ok(&S, i)); return S.val[i].v; }
static bool near(float a, float b) { return fabsf(a - b) < 1e-4f * (fabsf(b) > 1 ? fabsf(b) : 1); }
static size_t regs(uint8_t *d, const uint16_t *r, int n) { for (int k = 0; k < n; k++) { d[2 * k] = (uint8_t)(r[k] >> 8); d[2 * k + 1] = (uint8_t)r[k]; } return (size_t)n * 2; }
static const char *text(const char *id) { static char b[400]; pf_text(&P, F(id), &S, b, sizeof b); return b; }
static size_t str(uint8_t *d, const char *s, int nregs) { memset(d, 0, (size_t)nregs * 2); memcpy(d, s, strlen(s)); return (size_t)nregs * 2; }

int main(void)
{
    static char err[160];
    static uint8_t d[256];
    size_t n;
    for (int i = 0; profiles_id(i); i++) {
        const char *t = profiles_text(profiles_id(i), &n);
        if (!profile_parse(&P, t, n, err, sizeof err)) { printf("%s: %s\n", profiles_id(i), err); assert(0); }
        printf("  %s: %d blocks, %d fields\n", profiles_id(i), P.nblock, P.nfield);
    }
    assert(!profiles_text("nope", &n) && !profiles_id(-1));

    const char *t = profiles_text("eg4-bms", &n);
    assert(t && profile_parse(&P, t, n, err, sizeof err));
    assert(P.turn_ms == 50 && P.timeout_ms == 1000 && P.baud == 9600 && !strcmp(pf_str(&P, P.maker), "EG4"));
    pf_state_init(&S);
    /* live1: 53.12 V, -12.34 A, cells 3.301 .. 3.316 with cell 16 missing (0) */
    uint16_t l1[18] = { 5312, (uint16_t)-1234 };
    for (int k = 0; k < 15; k++) l1[2 + k] = (uint16_t)(3301 + k);
    l1[17] = 0;
    assert(pf_decode(&P, profile_block(&P, "live1"), d, regs(d, l1, 18), &S));
    assert(near(V("total_voltage"), 53.12f) && near(V("current"), -12.34f) && near(V("cell_voltage_3"), 3.303f));
    assert(near(V("power"), -655.5008f) && V("charging_power") == 0 && near(V("discharging_power"), 655.5008f));
    assert(near(V("min_cell_voltage"), 3.301f) && near(V("max_cell_voltage"), 3.315f) && near(V("delta_cell_voltage"), 0.014f));
    assert(near(V("cell_average_voltage"), 3.308f) && V("min_voltage_cell") == 1 && V("max_voltage_cell") == 15 && V("cell_count") == 15);
    /* live2 */
    /* 0x12.. : temps, capacity, current, SOH, SOC, status 0x8001 (charging, heating), warnings 0x0003,
     * protection 0x1410 (charge OC, charge UT, low capacity), error 0x0001, cycles 0x0001_0002,
     * 0x1F-0x20 unused, temps 1-6 packed, 0x24, full capacity 2000, 0x26 */
    uint16_t l2[21] = { 31, 25, 33, 150, 100, 98, 76, 0x8001, 0x0003, 0x1410, 0x0001, 0x0001, 0x0002, 0, 0, 0x19E7, 0x1A1B, 0x1C1D, 0, 2000, 0 };
    assert(pf_decode(&P, profile_block(&P, "live2"), d, regs(d, l2, 21), &S));
    assert(V("pcb_temperature") == 31 && V("avg_temperature") == 25 && V("max_temperature") == 33 && V("remaining_capacity") == 150);
    assert(V("max_charge_current") == 100 && V("state_of_health") == 98 && V("state_of_charge") == 76 && V("cycle_count") == 65538);
    assert(!strcmp(text("status"), "Charging") && V("heating") == 1);
    /* Expected strings are eg4_bms.cpp's (decode_status, warnings, protection, error) except for
     * the documented differences: set bits are joined by "; " not ";"; no "Heating+" prefix on
     * status (heating is its own binary sensor); an unknown status reads "Unknown (N)". */
    assert(!strcmp(text("warnings"), "Pack OV; Cell OV") && !strcmp(text("protection"), "Charge OC; Charge UT; Low Capacity"));
    assert(!strcmp(text("error"), "Voltage Error"));
    {   /* every label, exactly as eg4_bms.cpp spells it (same documented differences) */
        uint16_t m[21]; memcpy(m, l2, sizeof m);
        m[7] = 0x0008; m[8] = 0x3FFF; m[9] = 0x3FFF; m[10] = 0x0017;
        assert(pf_decode(&P, profile_block(&P, "live2"), d, regs(d, m, 21), &S));
        assert(!strcmp(text("status"), "Charging Limited"));
        assert(!strcmp(text("warnings"), "Pack OV; Cell OV; Pack UV; Cell UV; Charge OC; Discharge OC; Abnormal Temp; MOS Overheat; "
                                         "Charge OT; Discharge OT; Charge UT; Discharge UT; Low Capacity; Float Stopped"));
        assert(!strcmp(text("protection"), "Pack OV; Cell OV; Pack UV; Cell UV; Charge OC; Discharge OC; Abnormal Temp; MOS Overheat; "
                                           "Charge OT; Discharge OT; Charge UT; Discharge UT; Low Capacity; Discharge SC"));
        assert(!strcmp(text("error"), "Voltage Error; Temperature Error; Current Flow Error; Cell Unbalance"));
        const char *st[] = { "Standby", "Charging", "Discharging", 0, "Protect" };
        for (int k = 0; k < 5; k++) if (st[k]) {
            m[7] = (uint16_t)k;
            assert(pf_decode(&P, profile_block(&P, "live2"), d, regs(d, m, 21), &S) && !strcmp(text("status"), st[k]));
        }
        assert(pf_decode(&P, profile_block(&P, "live2"), d, regs(d, l2, 21), &S));   /* back to the first frame */
    }
    assert(V("charging") == 0 && V("discharging") == 1);              /* 0x1410 & 0x0513: blocked from charging only */
    assert(V("temperature_1") == 25 && V("temperature_2") == -25 && V("temperature_3") == 26 && V("temperature_6") == 29);
    assert(near(V("full_capacity"), 200.0f));
    /* the second block's zero rule: remaining, SOH and full capacity all 0 */
    l2[3] = 0; l2[5] = 0; l2[19] = 0;
    assert(!pf_decode(&P, profile_block(&P, "live2"), d, regs(d, l2, 21), &S) && V("remaining_capacity") == 150);
    /* identification, as REGISTER_MAP.md shows it */
    assert(pf_decode(&P, profile_block(&P, "model"), d, str(d, "ZTR-16S200A-10KW-STD2", 11), &S));
    assert(pf_decode(&P, profile_block(&P, "firmware"), d, str(d, "S02T11", 3), &S));
    assert(pf_decode(&P, profile_block(&P, "serial"), d, str(d, "2309876543210987", 8), &S));
    assert(!strcmp(text("model"), "ZTR-16S200A-10KW-STD2") && !strcmp(text("firmware_version"), "S02T11") &&
           !strcmp(text("serial_number"), "2309876543210987"));
    puts("builtin: ok");
    return 0;
}
