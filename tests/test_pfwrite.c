/* test_pfwrite.c — commands into writes (src/app/pfwrite.c).
 *   cc -Isrc/app tests/test_pfwrite.c src/app/pfwrite.c src/app/profile.c -lm -o /tmp/pw && /tmp/pw */
#include "pfwrite.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static pf_build_t B;
static profile_t P;
static const char *T =
    "profile c 1\n"
    "block s hold 60 12 1m\n"
    "number lim \"Limit\" s 60 u16 *0.1 min 0 max 25 kW\n"
    "number amps \"Amps\" s 61 u16 min 0 max 250 step 5 A\n"
    "number tmp \"Temp\" s 62 s16 *0.1 min -20 max 60\n"
    "number big \"Big\" s 63 u32 min 0 max 100000\n"
    "number lit \"Little\" s 65 s32le min -100000 max 100000\n"
    "select by \"By\" s 67 mask=0x000E 0=Off 2=Time 4=Voltage 6=SOC\n"
    "switch ac \"AC\" s 68 bit 7\n"
    "switch run \"Run\" s 69 on 1 off 0\n"
    "time t1 \"Start\" s 70\n"
    "sensor v \"V\" s 71 u16\n";
static int F(const char *id) { int i = profile_field(&P, id); assert(i >= 0); return i; }
static pf_write_t W;
static char why[48];
static bool cmd(const char *id, const char *s) { return pf_command(&P, F(id), s, &W, why, sizeof why); }

int main(void)
{
    char e[96];
    assert(profile_parse(&B, T, strlen(T), e, sizeof e));
    profile_view(&B, &P);
    /* A number: scaled, rounded to its step, within min and max. */
    assert(cmd("lim", "12") && W.addr == 60 && W.regs == 1 && W.mask[0] == 0xFFFF && W.value[0] == 120);
    assert(cmd("lim", "12.04") && W.value[0] == 120 && cmd("lim", "12.0") && W.value[0] == 120);
    assert(cmd("lim", "25") && W.value[0] == 250 && cmd("lim", "0") && W.value[0] == 0);
    assert(!cmd("lim", "25.1") && !strcmp(why, "out of range"));
    assert(!cmd("lim", "-0.1") && !strcmp(why, "out of range"));
    assert(!cmd("lim", "abc") && !strcmp(why, "not a number"));
    assert(!cmd("lim", "") && !cmd("lim", "1e3") && !cmd("lim", "nan") && !strcmp(why, "not a number"));
    assert(cmd("amps", "52") && W.value[0] == 50 && cmd("amps", "53") && W.value[0] == 55);
    assert(!cmd("amps", "253") && !strcmp(why, "out of range"));          /* to the step: 255 */
    assert(cmd("tmp", "-5") && W.value[0] == (uint16_t)-50);
    assert(cmd("big", "70000") && W.regs == 2 && W.value[0] == 1 && W.value[1] == 4464);    /* high word first */
    assert(cmd("lit", "-2") && W.regs == 2 && W.addr == 65 && W.value[0] == 0xFFFE && W.value[1] == 0xFFFF);   /* low first */
    /* A select: a choice's name, in its bits. */
    assert(cmd("by", "SOC") && W.mask[0] == 0x000E && W.value[0] == 6);
    assert(!cmd("by", "soc") && !strcmp(why, "not one of the choices"));
    /* A switch: ON and OFF, as its bit or its two values. */
    assert(cmd("ac", "ON") && W.mask[0] == 0x80 && W.value[0] == 0x80 && cmd("ac", "OFF") && W.value[0] == 0);
    assert(cmd("run", "ON") && W.mask[0] == 0xFFFF && W.value[0] == 1);
    assert(!cmd("run", "on") && !strcmp(why, "not ON or OFF"));
    /* A time: HH:MM, the hour in the low byte. */
    assert(cmd("t1", "23:59") && W.value[0] == 0x3B17 && cmd("t1", "00:00") && W.value[0] == 0);
    assert(!cmd("t1", "24:00") && !cmd("t1", "7:30") && !cmd("t1", "07:60") && !cmd("t1", "0730") && !strcmp(why, "not a time of day"));
    assert(!cmd("v", "1") && !strcmp(why, "not a control"));
    /* Merged with what was read: only the control's bits change. */
    uint16_t have[2] = { 0xF355, 0 }, out[2];
    assert(cmd("ac", "ON") && pf_merge(&W, have, out) && out[0] == 0xF3D5);
    have[0] = 0xF3D5;
    assert(!pf_merge(&W, have, out) && out[0] == 0xF3D5);                 /* already so: nothing to write */
    assert(cmd("by", "Time") && pf_merge(&W, have, out) && out[0] == 0xF3D3);
    have[0] = 1; have[1] = 4464;
    assert(cmd("big", "70000") && !pf_merge(&W, have, out));
    have[1] = 0;
    assert(pf_merge(&W, have, out) && out[0] == 1 && out[1] == 4464);
    puts("pfwrite: ok");
    return 0;
}
