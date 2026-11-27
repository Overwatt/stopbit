/* test_builtin.c — every built-in profile parses; the EG4 battery's decodes as the ESPHome
 * component does (eg4_bms.cpp), with the identification strings of its REGISTER_MAP.md;
 * the EG4 hybrid inverters' decodes registers laid out as EG4's protocol document has them.
 *   python3 tools/embed_profiles.py /tmp/profiles_gen.h profiles/*.txt &&
 *   cc -I/tmp -Isrc/app tests/test_builtin.c src/app/pfwrite.c src/app/profiles.c src/app/profile.c src/app/pfdecode.c -lm -o /tmp/bi && /tmp/bi */
#include "profiles.h"
#include "pfdecode.h"
#include "pfwrite.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static pf_build_t B;
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
        if (!profile_parse(&B, t, n, err, sizeof err)) { printf("%s: %s\n", profiles_id(i), err); assert(0); }
        profile_view(&B, &P);
        printf("  %s: %d blocks, %d fields, %zu bytes packed\n", profiles_id(i), P.nblock, P.nfield, profile_size(&B));
        assert(profile_size(&B) <= 24576);                  /* device.c's PF_ROOM */
    }
    assert(!profiles_text("nope", &n) && !profiles_id(-1));

    const char *t = profiles_text("eg4-bms", &n);
    assert(t && profile_parse(&B, t, n, err, sizeof err));
    profile_view(&B, &P);
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

    /* ---- EG4 18kPV: as EG4's protocol document lays the registers out ---- */
    t = profiles_text("eg4-hybrid", &n);
    assert(t && profile_parse(&B, t, n, err, sizeof err));
    profile_view(&B, &P);
    assert(P.baud == 19200 && !strcmp(P.frame, "8N1") && P.turn_ms == 700 && P.nblock == 7);
    float per_min = 0;
    for (int b = 0; b < P.nblock; b++) {                  /* whole groups of 40, as the inverter insists */
        assert(P.block[b].count == 40 && P.block[b].addr % 40 == 0 && P.block[b].fn == (b >= 2 ? 3 : 4));
        per_min += 60000.0f / (float)P.block[b].period_ms;
    }
    assert(per_min < 10);                                   /* 9.5 a minute: SolarAssistant shares the line */
    pf_state_init(&S);
    uint16_t in[40] = { 0 };
    in[0] = 0x000C;                                         /* PV charge + on-grid */
    in[1] = 3805; in[2] = 3712; in[4] = 532;                /* 380.5 V, 371.2 V; battery 53.2 V */
    in[5] = (uint16_t)(100 << 8 | 87);                      /* health in the high byte, charge in the low */
    in[7] = 4200; in[8] = 3900; in[10] = 2500;              /* PV 8100 W, 2500 W of it into the battery */
    in[12] = 2412; in[15] = 5998; in[16] = 5200; in[20] = 2405; in[23] = 6000; in[24] = 1200;
    in[26] = 3000;                                          /* exporting */
    in[28] = 215; in[29] = 198; in[31] = 180; in[33] = 95; in[36] = 122; in[37] = 3;
    assert(pf_decode(&P, profile_block(&P, "live"), d, regs(d, in, 40), &S));
    assert(!strcmp(text("status"), "PV charge + on-grid"));
    assert(near(V("pv_voltage_1"), 380.5f) && near(V("pv_voltage_2"), 371.2f) && V("pv_voltage_3") == 0 && near(V("battery_voltage"), 53.2f));
    assert(V("state_of_charge") == 87 && V("state_of_health") == 100);
    assert(V("pv_power") == 8100 && V("battery_power") == 2500 && V("grid_power") == -3000 && V("inverter_power") == 5200);
    assert(near(V("grid_voltage"), 241.2f) && near(V("grid_frequency"), 59.98f) && near(V("eps_voltage"), 240.5f) && V("eps_frequency") == 60 && V("eps_power") == 1200);
    assert(near(V("pv_energy_today"), 41.3f) && near(V("inverter_energy_today"), 18.0f) && near(V("charge_energy_today"), 9.5f));
    assert(near(V("grid_export_today"), 12.2f) && near(V("grid_import_today"), 0.3f));
    in[0] = 0x0104;                                         /* only the low byte is the mode */
    in[10] = 0; in[11] = 1800; in[26] = 0; in[27] = 650;    /* discharging, importing */
    assert(pf_decode(&P, profile_block(&P, "live"), d, regs(d, in, 40), &S));
    assert(!strcmp(text("status"), "PV on-grid") && V("battery_power") == -1800 && V("grid_power") == 650);
    in[0] = 0x0060;                                         /* a mode the document does not list */
    assert(pf_decode(&P, profile_block(&P, "live"), d, regs(d, in, 40), &S) && !strcmp(text("status"), "Unknown (96)"));

    uint16_t tot[40] = { 0 };                               /* registers 40 to 79 */
    tot[0] = 0x86A0; tot[1] = 0x0001;                       /* 100,000 tenths, low word first: 10,000.0 kWh */
    tot[2] = 50000;                                         /* 5,000.0 kWh */
    tot[16] = 4321; tot[18] = 77;                           /* export 432.1 kWh, import 7.7 kWh */
    tot[20] = 0x1000; tot[21] = 0x0008;                     /* fault bits 12 and 19 */
    tot[22] = 0x0001; tot[23] = 0x0001;                     /* warning bits 0 and 16 */
    tot[24] = 41; tot[25] = 38; tot[26] = 39; tot[27] = (uint16_t)-5;
    assert(pf_decode(&P, profile_block(&P, "totals"), d, regs(d, tot, 40), &S));
    assert(near(V("pv_energy_1"), 10000.0f) && near(V("pv_energy_2"), 5000.0f) && near(V("pv_energy"), 15000.0f));
    assert(near(V("grid_export"), 432.1f) && near(V("grid_import"), 7.7f));
    assert(!strcmp(text("fault"), "E012 UPS short; E019 Bus voltage high"));
    assert(!strcmp(text("warning"), "W000 Battery communication; W016 Grid power outage"));
    assert(V("inner_temperature") == 41 && V("radiator_temperature_1") == 38 && V("radiator_temperature_2") == 39 && V("battery_temperature") == -5);
    tot[20] = tot[21] = tot[22] = tot[23] = 0;
    assert(pf_decode(&P, profile_block(&P, "totals"), d, regs(d, tot, 40), &S) && !strcmp(text("fault"), "None") && !strcmp(text("warning"), "None"));

    uint16_t id[40] = { 0 };                                /* holding 0 to 39: text, each register's low byte first */
    const char *sn = "AB12345678", *fw = "FAAB";
    for (int k = 0; k < 5; k++) id[2 + k] = (uint16_t)((uint8_t)sn[2 * k + 1] << 8 | (uint8_t)sn[2 * k]);
    for (int k = 0; k < 2; k++) id[7 + k] = (uint16_t)((uint8_t)fw[2 * k + 1] << 8 | (uint8_t)fw[2 * k]);
    assert(pf_decode(&P, profile_block(&P, "ident"), d, regs(d, id, 40), &S));
    assert(!strcmp(text("serial_number"), "AB12345678") && !strcmp(text("firmware"), "FAAB"));
    /* Settings, from registers read from a FlexBOSS on 2026-11-27 (values only). */
    static const uint16_t H40[40] = { 16, 2640, 5850, 6120, 30000, 30000, 5650, 6200, 16, 16, 5650, 6200, 16, 16, 44, 2352,
        2208, 2448, 2592, 0, 100, 0, 1000, 1000, 100, 100, 120, 100, 1, 3, 11, 16, 0, 0, 120, 100, 0, 15127, 0, 0 };
    static const uint16_t H80[40] = { 0, 0, 120, 20, 0, 0, 0, 0, 0, 0, 240, 60, 0, 0, 0, 0, 5, 300, 0, 565,
        490, 250, 250, 100, 0, 10, 65336, 550, 0, 400, 1065, 0, 1, 1, 0, 60036, 100, 65486, 500, 0 };
    static const uint16_t H120[40] = { 4, 0, 0, 0, 6304, 0, 0, 0, 0, 0, 0, 0, 0, 0, 59964, 5696, 20, 1420, 0, 0,
        0, 0, 0, 0, 550, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 540, 560 };
    static const uint16_t H160[40] = { 20, 0, 0, 0, 0, 0, 0, 0, 0, 500, 0, 0, 0, 0, 0, 0, 65535, 120, 0, 4160,
        100, 2544, 2640, 10, 20, 2400, 300, 0, 44, 0, 50, 100, 100, 20, 400, 560, 10, 100, 60, 0 };
    id[21] = 0xF355;
    assert(pf_decode(&P, profile_block(&P, "ident"), d, regs(d, id, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set40"), d, regs(d, H40, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set80"), d, regs(d, H80, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set120"), d, regs(d, H120, 40), &S));
    assert(pf_decode(&P, profile_block(&P, "set160"), d, regs(d, H160, 40), &S));
    /* Every control reads as Home Assistant is told it may: a number within its min and max,
     * a select one of its choices (else HA calls each state message invalid). */
    for (int i = 0; i < P.nfield; i++) {
        const pf_field_t *f = &P.field[i];
        if (!pf_control(f)) continue;
        bool ok = pf_ok(&S, i);
        float tol = (f->k > 0 ? f->k : 1) / 1000;
        if (ok && f->kind == PF_NUMBER) ok = S.val[i].v >= f->lo - tol && S.val[i].v <= f->hi + tol;
        if (ok && f->kind == PF_SELECT) {
            ok = false;
            for (int k = 0; k < f->nvname; k++) ok = ok || P.vname[f->vname + k].value == S.val[i].raw;
        }
        if (!ok) printf("%s reads %s\n", pf_str(&P, f->id), pf_ok(&S, i) ? "outside its range or choices" : "nothing");
        assert(ok);
    }
    assert(near(V("ac_charge_rate"), 12) && near(V("ac_charge_soc_limit"), 100) && near(V("charge_first_rate"), 12));
    assert(!strcmp(text("ac_charge_start_1"), "01:00") && !strcmp(text("ac_charge_end_1"), "03:00"));
    assert(!strcmp(text("ac_charge_start_2"), "11:00") && !strcmp(text("ac_charge_end_2"), "16:00"));
    assert(!strcmp(text("charge_first_end_1"), "23:59") && !strcmp(text("charge_first_start_3"), "00:00"));
    assert(near(V("forced_discharge_rate"), 12) && near(V("forced_discharge_soc"), 20));
    assert(near(V("charge_current"), 250) && near(V("discharge_current"), 250) && near(V("export_limit"), 100));
    assert(near(V("discharge_cutoff_soc"), 10) && near(V("off_grid_cutoff_soc"), 0) && near(V("ac_charge_start_soc"), 20));
    assert(!strcmp(text("ac_charge_by"), "Voltage") && !strcmp(text("discharge_control"), "Voltage") && !strcmp(text("output_priority"), "Battery first"));
    assert(V("ac_charge") == 0 && V("charge_first") == 0 && V("forced_discharge") == 0 && V("export") == 1 && V("off_grid_output") == 1 && V("power") == 1);
    assert((P.field[F("power")].flags & PFF_HIDDEN) && (P.field[F("off_grid_output")].flags & PFF_HIDDEN) && !(P.field[F("export")].flags & PFF_HIDDEN));
    assert(!strcmp(pf_str(&P, P.field[F("power")].name), "Power (off: standby)"));   /* what off does, where HA shows it */
    /* ... and a command for one: its bit alone. */
    pf_write_t w;
    char why[32];
    uint16_t have[2] = { 0xF355, 0 }, out[2];
    assert(pf_command(&P, F("ac_charge"), "ON", &w, why, sizeof why) && w.addr == 21 && pf_merge(&w, have, out) && out[0] == 0xF3D5);
    assert(pf_command(&P, F("ac_charge_rate"), "8.5", &w, why, sizeof why) && w.addr == 66 && w.value[0] == 85);
    assert(!pf_command(&P, F("ac_charge_rate"), "26", &w, why, sizeof why));
    assert(pf_command(&P, F("ac_charge_by"), "SOC", &w, why, sizeof why) && w.addr == 120 && w.mask[0] == 0x000E && w.value[0] == 6);
    uint16_t hv[2] = { 0x0004, 0 };
    assert(pf_merge(&w, hv, out) && out[0] == 0x0006);
    assert(pf_command(&P, F("ac_charge_start_1"), "07:30", &w, why, sizeof why) && w.addr == 68 && w.value[0] == 0x1E07);
    puts("builtin: ok");
    return 0;
}
