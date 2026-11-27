/* test_pfdecode.c — replies into values (src/app/pfdecode.c).
 *   cc -Isrc/app tests/test_pfdecode.c src/app/pfdecode.c src/app/profile.c -o /tmp/pd && /tmp/pd */
#include "pfdecode.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static pf_build_t B;
static profile_t P;
static pf_state_t S;
static const char *T =
    "profile t 1\n"
    "block a hold 0 12 10s\n"
    "block cells hold 100 4 10s\n"
    "block txt hold 200 6 1m info\n"
    "block c coil 0 10 10s\n"
    "sensor v \"V\" a 0 u16 *0.01 V\n"
    "sensor i \"I\" a 1 s16 *0.01 A\n"
    "sensor big \"Big\" a 2 u32\n"
    "sensor le \"Le\" a 4 s32le\n"
    "sensor th \"Th\" a 6 s8h\n"
    "sensor tl \"Tl\" a 6 s8l\n"
    "sensor off \"Off\" a 7 u16 +-500 *0.1\n"
    "sensor nd \"Nd\" a 8 u16 nodata 0xFFFF\n"
    "text st \"St\" a 9 enum mask=0x0F 0=Idle 1=Run 8=\"Run limited\"\n"
    "binary hot \"Hot\" a 9 bit 15\n"
    "text w \"W\" a 10 bits 0=\"A\" 3=\"B\" 15=\"C\"\n"
    "binary high \"High\" a 0 u16 > 5000\n"
    "sensor cell1..4 \"Cell #\" cells 100 u16 *0.001 V\n"
    "text model \"Model\" txt 200 ascii 4\n"
    "text lo \"Low\" txt 204 asciilo 2\n"
    "binary c3 \"C3\" c 3 coil\n"
    "calc p \"P\" mul v i W\n"
    "calc pp \"Pp\" pos p\n"
    "calc np \"Np\" neg p\n"
    "calc ab \"Ab\" abs i\n"
    "calc df \"Df\" sub v off\n"
    "calc cmin \"Cmin\" min cell1..4 within 0.001..4.999\n"
    "calc cmax \"Cmax\" max cell1..4 within 0.001..4.999\n"
    "calc cavg \"Cavg\" avg cell1..4 within 0.001..4.999\n"
    "calc cdel \"Cdel\" delta cell1..4 within 0.001..4.999\n"
    "calc clow \"Clow\" argmin cell1..4 within 0.001..4.999\n"
    "calc chigh \"Chigh\" argmax cell1..4 within 0.001..4.999\n"
    "calc ccnt \"Ccnt\" count cell1..4 within 0.001..4.999\n"
    "calc csum \"Csum\" sum cell1..4\n"
    "calc running \"Running\" eq st 1\n"
    "calc anyw \"Anyw\" anybits w 0x0008\n"
    "calc now \"Now\" nobits w 0x0009\n"
    "reject a if zero\n"
    "reject a if v == 0\n"
    "reject cells if zero cell1 cell2\n";

static int F(const char *id) { int i = profile_field(&P, id); assert(i >= 0); return i; }
static float V(const char *id) { int i = F(id); assert(pf_ok(&S, i)); return S.val[i].v; }
static bool near(float a, float b) { return fabsf(a - b) < 1e-4f * (fabsf(b) > 1 ? fabsf(b) : 1); }
static size_t regs(uint8_t *d, const uint16_t *r, int n) { for (int k = 0; k < n; k++) { d[2 * k] = (uint8_t)(r[k] >> 8); d[2 * k + 1] = (uint8_t)r[k]; } return (size_t)n * 2; }
static const char *text(const char *id) { static char b[100]; pf_text(&P, F(id), &S, b, sizeof b); return b; }
static const char *num(float v, int prec) { static char b[40]; pf_num(b, sizeof b, v, prec); return b; }

int main(void)
{
    static char err[160];
    uint8_t d[64];
    if (!profile_parse(&B, T, strlen(T), err, sizeof err)) { puts(err); assert(0); }
    profile_view(&B, &P);
    pf_state_init(&S);
    assert(!pf_ok(&S, F("v")) && !pf_ok(&S, F("p")));

    const uint16_t a[12] = { 5312, (uint16_t)-1234, 0x0001, 0x0002, 0xFFFE, 0xFFFF, 0x19E7, 750, 0xFFFF, 0x8001, 0x0009, 0 };
    assert(pf_decode(&P, 0, d, regs(d, a, 12), &S));
    assert(near(V("v"), 53.12f) && near(V("i"), -12.34f) && V("big") == 65538.0f && V("le") == -2.0f);
    assert(V("th") == 25.0f && V("tl") == -25.0f && near(V("off"), 25.0f));
    assert(!pf_ok(&S, F("nd")));                                       /* 0xFFFF: no data */
    assert(V("hot") == 1.0f && V("high") == 1.0f && !strcmp(text("st"), "Run") && !strcmp(text("w"), "A; B"));
    assert(near(V("p"), 53.12f * -12.34f) && V("pp") == 0.0f && near(V("np"), 655.5008f) && near(V("ab"), 12.34f));
    assert(near(V("df"), 53.12f - 25.0f));
    assert(V("running") == 1.0f && V("anyw") == 1.0f && V("now") == 0.0f);
    assert(!pf_ok(&S, F("cmin")) && !pf_ok(&S, F("csum")));            /* cells not read yet */

    const uint16_t c[4] = { 3301, 3299, 0, 3310 };                    /* cell 3 missing: reads 0 */
    assert(pf_decode(&P, 1, d, regs(d, c, 4), &S));
    assert(near(V("cmin"), 3.299f) && near(V("cmax"), 3.310f) && near(V("cdel"), 0.011f));
    assert(near(V("cavg"), (3.301f + 3.299f + 3.310f) / 3) && V("clow") == 2.0f && V("chigh") == 4.0f);
    assert(V("ccnt") == 3.0f && near(V("csum"), 9.91f));

    const uint16_t t[6] = { 0x4142, 0x4300, 0x5859, 0x2020, 0x0048, 0x0069 };   /* "ABC" NUL "XY  "; "Hi" */
    assert(pf_decode(&P, 2, d, regs(d, t, 6), &S));
    assert(!strcmp(text("model"), "ABC") && !strcmp(text("lo"), "Hi"));
    const uint16_t t2[6] = { 0x4869, 0x2020, 0x2001, 0x2020, 0x0041, 0x0042 };  /* "Hi", spaces, a control byte */
    assert(pf_decode(&P, 2, d, regs(d, t2, 6), &S) && !strcmp(text("model"), "Hi") && !strcmp(text("lo"), "AB"));

    uint8_t coils[2] = { 0x08, 0x00 };
    assert(pf_decode(&P, 3, coils, 2, &S) && V("c3") == 1.0f);
    assert(!pf_decode(&P, 3, coils, 1, &S));                           /* the wrong size */

    /* Enum and bits text at the edges. */
    uint16_t a2[12];
    memcpy(a2, a, sizeof a2);
    a2[9] = 5; a2[10] = 0;
    assert(pf_decode(&P, 0, d, regs(d, a2, 12), &S));
    assert(!strcmp(text("st"), "Unknown (5)") && !strcmp(text("w"), "None") && V("hot") == 0.0f && V("anyw") == 0.0f && V("now") == 1.0f);

    /* Reject rules: nothing changes. */
    memset(d, 0, sizeof d);
    assert(!pf_decode(&P, 0, d, 24, &S) && near(V("v"), 53.12f));
    a2[0] = 0;
    assert(!pf_decode(&P, 0, d, regs(d, a2, 12), &S) && near(V("v"), 53.12f));
    const uint16_t c0[4] = { 0, 0, 3300, 3300 };
    assert(!pf_decode(&P, 1, d, regs(d, c0, 4), &S) && near(V("cmin"), 3.299f));
    assert(!pf_decode(&P, 0, d, 23, &S));

    /* A block gone stale: its fields and what depends on them unknown. */
    pf_drop(&P, 0, &S);
    assert(!pf_ok(&S, F("v")) && !pf_ok(&S, F("p")) && !pf_ok(&S, F("running")) && pf_ok(&S, F("cmin")));
    assert(!strcmp(text("st"), ""));

    /* Numbers. */
    assert(!strcmp(num(53.12f, 2), "53.12") && !strcmp(num(-12.5f, 0), "-13") && !strcmp(num(0, 0), "0"));
    assert(!strcmp(num(-0.004f, 2), "0.00") && !strcmp(num(3.3f, 3), "3.300") && !strcmp(num(1234567, 0), "1234567"));
    assert(!strcmp(num(NAN, 2), "null") && !strcmp(num(1e30f, 0), "null") && !strcmp(num(-25, 1), "-25.0"));
    assert(pf_prec(&P.field[F("v")]) == 2 && pf_prec(&P.field[F("big")]) == 0 && pf_prec(&P.field[F("off")]) == 1);
    assert(pf_prec(&P.field[F("cell1")]) == 3 && pf_prec(&P.field[F("p")]) == 3 && pf_prec(&P.field[F("ccnt")]) == 0);
    puts("pfdecode: ok");
    return 0;
}
