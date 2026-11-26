/* test_profile.c — profile text to tables (src/app/profile.c): what each statement makes,
 * and that each mistake is refused with its line.
 *   cc -Isrc/app tests/test_profile.c src/app/profile.c -o /tmp/pf && /tmp/pf */
#include "profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static profile_t P;
static char err[160];

static bool parse(const char *t) { return profile_parse(&P, t, strlen(t), err, sizeof err); }
static void bad(const char *t, const char *want)
{
    if (parse(t) || !strstr(err, want)) { printf("text:\n%s\nerror: \"%s\"\nwanted: \"%s\"\n", t, err, want); assert(0); }
}
static const pf_field_t *F(const char *id) { int i = profile_field(&P, id); assert(i >= 0); return &P.field[i]; }

#define HEAD "profile t 1\nblock b hold 0x10 20 10s\n"

int main(void)
{
    /* Everything once. */
    const char *ok =
        "# a test device\n"
        "profile test-dev 1\n"
        "name \"Test device\"\n"
        "maker \"ACME\"\n"
        "line 19200 8E1\n"
        "stale 3\n"
        "timeout 500ms\n"
        "turnaround 50ms\n"
        "block live hold 0x0000 18 10s\n"
        "block more input 100 4 2m\n"
        "block flags coil 0 16 1h info\n"
        "sensor volts \"Total voltage\" live 0x0000 u16 *0.01 V dc=voltage sc=measurement prec=2\n"
        "sensor amps \"Current\" live 1 s16 *0.01 +-5 A\n"
        "sensor cell1..16 \"Cell #\" live 2 u16 *0.001 V nodata 0xFFFF\n"
        "sensor big \"Energy\" more 100 u32le kWh sc=total_increasing diag\n"
        "binary heat \"Heating\" live 0x11 bit 15\n"
        "binary busy \"Busy\" more 102 u16 == 5 dc=running\n"
        "binary coil3 \"Coil 3\" flags 3 coil\n"
        "text state \"Status\" live 0x11 enum mask=0x000F 0=Standby 1=Charging 8=\"Charging limited\"\n"
        "text warn \"Warnings\" more 103 bits 0=\"Pack OV\" 13=\"Float stopped\" diag\n"
        "text model \"Model\" more 100 ascii 4\n"
        "calc power \"Power\" mul volts amps W dc=power\n"
        "calc low \"Lowest cell\" argmin cell1..16 within 0.001..4.999\n"
        "calc ok \"Charging allowed\" nobits warn 0x2001\n"
        "reject live if zero\n"
        "reject live if zero volts amps\n"
        "reject more if big > 100\n";
    if (!parse(ok)) { puts(err); assert(0); }
    assert(!strcmp(pf_str(&P, P.id), "test-dev") && !strcmp(pf_str(&P, P.name), "Test device") && !strcmp(pf_str(&P, P.maker), "ACME"));
    assert(P.baud == 19200 && !strcmp(P.frame, "8E1") && P.stale == 3 && P.timeout_ms == 500 && P.turn_ms == 50);
    assert(P.nblock == 3 && P.block[0].fn == 3 && P.block[0].count == 18 && P.block[0].period_ms == 10000);
    assert(P.block[1].fn == 4 && P.block[1].addr == 100 && P.block[1].period_ms == 120000 && !P.block[1].info);
    assert(P.block[2].fn == 1 && P.block[2].period_ms == 3600000 && P.block[2].info);
    const pf_field_t *f = F("volts");
    assert(f->kind == PF_SENSOR && f->type == T_U16 && f->block == 0 && f->addr == 0 && f->scale == 0.01f && f->prec == 2);
    assert(!strcmp(pf_str(&P, f->unit), "V") && !strcmp(pf_str(&P, f->dc), "voltage") && !strcmp(pf_str(&P, f->sc), "measurement"));
    assert(F("amps")->offset == -5 && F("amps")->type == T_S16 && F("amps")->prec == -1);
    for (int k = 1; k <= 16; k++) {
        char id[16], nm[16]; snprintf(id, sizeof id, "cell%d", k); snprintf(nm, sizeof nm, "Cell %d", k);
        f = F(id);
        assert(f->addr == 1 + k && !strcmp(pf_str(&P, f->name), nm) && f->nnodata == 1 && f->nodata[0] == 0xFFFF);
    }
    assert(F("big")->type == T_U32LE && (F("big")->flags & PFF_DIAG));
    assert(F("heat")->kind == PF_BINARY && F("heat")->type == T_BIT && F("heat")->bit == 15);
    assert(F("busy")->op == OP_EQ && F("busy")->k == 5.0f && F("coil3")->type == T_COIL && F("coil3")->addr == 3);
    f = F("state");
    assert(f->kind == PF_ENUM && f->mask == 0xF && f->nvname == 3 && P.vname[f->vname + 2].value == 8 &&
           !strcmp(pf_str(&P, P.vname[f->vname + 2].name), "Charging limited"));
    assert(F("warn")->kind == PF_BITS && F("warn")->nvname == 2 && P.vname[F("warn")->vname + 1].value == 13);
    assert(F("model")->kind == PF_ASCII && F("model")->type == T_ASCII && F("model")->bit == 4 && F("model")->str == 0);
    f = F("power");
    assert(f->kind == PF_CALC && f->fn == F_MUL && f->narg == 2 && P.arg[f->arg] == profile_field(&P, "volts") && f->block == 0xFF);
    f = F("low");
    assert(f->fn == F_ARGMIN && f->narg == 16 && (f->flags & PFF_WITHIN) && f->lo == 0.001f && f->hi == 4.999f);
    f = F("ok");
    assert(f->kind == PF_CALCBIN && f->fn == F_NOBITS && f->mask == 0x2001);
    assert(P.nreject == 3 && P.reject[0].kind == RJ_ZERO && P.reject[1].kind == RJ_ZEROS && P.reject[1].nfield == 2 &&
           P.reject[2].kind == RJ_CMP && P.reject[2].op == OP_GT && P.reject[2].k == 100.0f && P.reject[2].block == 1);

    /* A list calc with within: every field gets the range. */
    if (!parse(HEAD "sensor a1..2 \"A #\" b 0x10 u16\ncalc s1..2 \"S #\" sum a1 a2 within 0..9\n")) { puts(err); assert(0); }
    assert(F("s1")->lo == 0 && F("s1")->hi == 9 && F("s2")->hi == 9 && (F("s2")->flags & PFF_WITHIN));

    /* Mistakes, each with its line. */
    bad("", "the profile is empty");
    bad("# nothing\n\n", "the profile is empty");
    bad("name \"x\"\n", "line 1: the first statement must be: profile id 1");
    bad("profile EG4 1\n", "line 1: bad profile id 'EG4'");
    bad("profile eg4 2\n", "line 1: format version 2 is not known; this firmware reads 1");
    bad("profile t 1\n", "no blocks");
    bad("profile t 1\nprofile t 1\n", "line 2: profile is given twice");
    bad("profile t 1\nname x\n", "line 2: the name must be \"quoted\", 1 to 40 characters");
    bad("profile t 1\nline 9600 8X1\n", "line 2: bad line settings '8X1'");
    bad("profile t 1\ntimeout 10ms\n", "line 2: timeout must be 50ms to 10000ms");
    bad("profile t 1\nblock b hold 0 0 10s\n", "line 2: a block of holding or input registers counts 1 to 125");
    bad("profile t 1\nblock b hold 0 126 10s\n", "line 2: a block of holding or input registers counts 1 to 125");
    bad("profile t 1\nblock b fifo 0 2 10s\n", "line 2: unknown block kind 'fifo'");
    bad("profile t 1\nblock b hold 0 2 10x\n", "line 2: bad period '10x'");
    bad(HEAD "block b hold 0 2 10s\n", "line 3: 'b' is declared twice");
    bad(HEAD "sensor x \"X\" nope 0x10 u16\n", "line 3: no block 'nope'");
    bad(HEAD "sensor x \"X\" b 0x10 u17\n", "line 3: unknown type 'u17'");
    bad(HEAD "sensor x \"X\" b 0x0F u16\n", "line 3: 0x000f is outside block b");
    bad(HEAD "sensor x \"X\" b 0x23 u32\n", "line 3: 0x0023 is outside block b");
    bad(HEAD "sensor x X b 0x10 u16\n", "line 3: the name must be \"quoted\"");
    bad(HEAD "sensor x \"12345678901234567890123456789012345678901\" b 0x10 u16\n", "line 3: the name must be \"quoted\", 1 to 40");
    bad(HEAD "sensor X \"X\" b 0x10 u16\n", "line 3: bad id 'X': 1 to 24 of a-z 0-9 _");
    bad(HEAD "sensor x \"X\" b 0x10 u16\nsensor x \"Y\" b 0x11 u16\n", "line 4: 'x' is declared twice");
    bad(HEAD "sensor x \"X\" b 0x10 u16 *zz\n", "line 3: bad scale '*zz'");
    bad(HEAD "sensor x \"X\" b 0x10 u16 V A\n", "line 3: unknown word 'A'");
    bad(HEAD "sensor c1..4 \"Cell\" b 0x10 u16\n", "line 3: a list's name needs # for its number");
    bad(HEAD "sensor c5..3 \"C #\" b 0x10 u16\n", "line 3: bad id");
    bad(HEAD "sensor c3..x \"C #\" b 0x10 u16\n", "line 3: bad id");
    bad(HEAD "sensor c3.. \"C #\" b 0x10 u16\n", "line 3: bad id");
    bad(HEAD "sensor c2147483647..2147483647 \"C #\" b 0x10 u16\n", "line 3: bad id");
    bad(HEAD "sensor x \"X\" b 0x10 coil\n", "line 3: coils are read from a coil or discrete block");
    bad(HEAD "binary x \"X\" b 0x10 bit 16\n", "line 3: bit must be 0 to 15");
    bad(HEAD "binary x \"X\" b 0x10 u16 >= 2\n", "line 3: unknown comparison '>='");
    bad(HEAD "text x \"X\" b 0x10 enum\n", "line 3: enum needs at least one value=name");
    bad(HEAD "text x \"X\" b 0x10 bits 32=\"Y\"\n", "line 3: bad value '32'");
    bad(HEAD "text x \"X\" b 0x10 ascii 25\n", "line 3: ascii needs a length of 1 to 24 registers");
    bad(HEAD "text x \"X\" b 0x10 thing\n", "line 3: text needs enum, bits, ascii or asciilo");
    bad(HEAD "sensor x \"X\" b 0x10 u16\ncalc y \"Y\" frob x\n", "line 4: unknown function 'frob'");
    bad(HEAD "calc y \"Y\" pos nope\n", "line 3: no field 'nope'");
    bad(HEAD "sensor x \"X\" b 0x10 u16\ncalc y \"Y\" sub x\n", "line 4: sub needs 2 fields");
    bad(HEAD "sensor x \"X\" b 0x10 u16\ncalc y \"Y\" pos x within 0..1\n", "line 4: unknown word 'within'");
    bad("profile t 1\nblock b hold 0 2 10s\nblock c hold 2 2 10s\nsensor x \"X\" c 2 u16\nreject b if x == 0\n",
        "line 5: 'x' is not in block b");
    bad(HEAD "reject b when zero\n", "line 3: reject needs: block if zero [fields], or block if field op value");
    bad(HEAD "number n \"N\" reg 0x10 u16 min 0 max 9 step 1\n", "line 3: controls come in phase 2");
    bad(HEAD "sensor x \"X b 0x10 u16\n", "line 3: a quote is not closed");
    bad(HEAD "frobnicate\n", "line 3: unknown statement 'frobnicate'");
    {
        static char t[2048];
        snprintf(t, sizeof t, HEAD "sensor x \"%0600d\" b 0x10 u16\n", 0);
        bad(t, "line 3: longer than 512 characters");
        snprintf(t, sizeof t, HEAD "sensor x \"\x01\" b 0x10 u16\n");
        bad(t, "line 3: a control character");
    }
    {   /* Too many fields: 161. */
        static char t[16384];
        size_t n = (size_t)snprintf(t, sizeof t, "profile t 1\nblock b hold 0 125 10s\nblock c hold 125 125 10s\n");
        for (int k = 0; k <= PF_FIELDS; k++)
            n += (size_t)snprintf(t + n, sizeof t - n, "sensor f%d \"F\" %s %d u16\n", k, k < 125 ? "b" : "c", k);
        bad(t, "line 164: too many fields (160)");
    }
    {   /* Longer than 16 KB. */
        static char t[PF_TEXT_MAX + 2];
        memset(t, '#', sizeof t - 1);
        t[sizeof t - 1] = 0;
        bad(t, "the profile is longer than 16384 bytes");
    }
    puts("profile: ok");
    return 0;
}
