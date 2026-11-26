/*
 * profile.c — A device profile's text into tables (profile.h). One statement a line;
 * `#` starts a comment; words are split by spaces, "quoted" ones kept whole. Errors name
 * the line. No I/O, no allocation: fuzzed (tests/fuzz/profile.c).
 */
#include "profile.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX 512
#define TOK_MAX  96

typedef struct {
    profile_t *p;
    char *err;
    size_t errlen;
    int line;
    int ntok;
    char *tok[TOK_MAX];
    char buf[LINE_MAX + 1];
} ps_t;

static bool fail(ps_t *s, const char *fmt, ...)
{
    va_list a;
    int k = 0;
    if (!s->errlen) return false;
    if (s->line) k = snprintf(s->err, s->errlen, "line %d: ", s->line);
    va_start(a, fmt);
    if (k >= 0 && (size_t)k < s->errlen) vsnprintf(s->err + k, s->errlen - (size_t)k, fmt, a);
    va_end(a);
    return false;
}

/* s->buf into words. A "quoted" word keeps its quotes, and k="v w" is one word. */
static bool tokens(ps_t *s)
{
    char *c = s->buf;
    s->ntok = 0;
    for (;;) {
        while (*c == ' ' || *c == '\t' || *c == '\r') c++;
        if (!*c || *c == '#') return true;
        if (s->ntok == TOK_MAX) return fail(s, "too many words");
        s->tok[s->ntok++] = c;
        bool q = false;
        while (*c && (q || (*c != ' ' && *c != '\t' && *c != '\r' && *c != '#'))) {
            if (*c == '"') q = !q;
            c++;
        }
        if (q) return fail(s, "a quote is not closed");
        if (*c == '#') { *c = 0; return true; }
        if (*c) *c++ = 0;
    }
}

/* The word without its quotes; NULL if quoted badly. */
static char *unq(char *t)
{
    size_t n = strlen(t);
    if (t[0] != '"') return strchr(t, '"') ? NULL : t;
    if (n < 2 || t[n - 1] != '"' || memchr(t + 1, '"', n - 2)) return NULL;
    t[n - 1] = 0;
    return t + 1;
}

/* Decimal or 0x hex, with a sign; within [lo, hi]. */
static bool num_i(const char *t, int64_t lo, int64_t hi, int64_t *out)
{
    const char *d = t + (*t == '-' || *t == '+');
    char *e;
    if (!*d) return false;
    long long v = strtoll(t, &e, d[0] == '0' && (d[1] == 'x' || d[1] == 'X') ? 16 : 10);
    if (*e || e == t || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* [-]digits[.digits] */
static bool num_f(const char *t, float *out)
{
    double v = 0, f = 1;
    bool neg = *t == '-', any = false;
    if (*t == '-' || *t == '+') t++;
    while (isdigit((unsigned char)*t)) { v = v * 10 + (*t++ - '0'); any = true; }
    if (*t == '.') for (t++; isdigit((unsigned char)*t); t++) { f /= 10; v += (*t - '0') * f; any = true; }
    if (!any || *t) return false;
    *out = (float)(neg ? -v : v);
    return true;
}

/* A string into the pool, shared with an equal one already there. */
static bool pool_add(ps_t *s, const char *str, uint16_t *off)
{
    profile_t *p = s->p;
    size_t n = strlen(str) + 1;
    if (n == 1) { *off = 0; return true; }
    for (size_t i = 1; i + n <= p->npool; i += strlen(p->pool + i) + 1)
        if (!strcmp(p->pool + i, str)) { *off = (uint16_t)i; return true; }
    if (p->npool + n > PF_POOL) return fail(s, "the profile has too much text (%d bytes)", PF_POOL);
    memcpy(p->pool + p->npool, str, n);
    *off = p->npool;
    p->npool = (uint16_t)(p->npool + n);
    return true;
}

static bool id_ok(const char *t)
{
    size_t n = strlen(t);
    if (!n || n > PF_ID_MAX) return false;
    for (; *t; t++) if (!((*t >= 'a' && *t <= 'z') || (*t >= '0' && *t <= '9') || *t == '_')) return false;
    return true;
}

static bool word_ok(const char *t)       /* an HA class: a-z and _ */
{
    size_t n = strlen(t);
    if (!n || n > 32) return false;
    for (; *t; t++) if (!((*t >= 'a' && *t <= 'z') || *t == '_')) return false;
    return true;
}

int profile_block(const profile_t *p, const char *id)
{
    for (int i = 0; i < p->nblock; i++) if (!strcmp(pf_str(p, p->block[i].id), id)) return i;
    return -1;
}

int profile_field(const profile_t *p, const char *id)
{
    for (int i = 0; i < p->nfield; i++) if (!strcmp(pf_str(p, p->field[i].id), id)) return i;
    return -1;
}

static const char *name_of(ps_t *s, int i)
{
    char *n = i < s->ntok && s->tok[i][0] == '"' ? unq(s->tok[i]) : NULL;
    if (!n || !*n || strlen(n) > PF_NAME_MAX) { fail(s, "the name must be \"quoted\", 1 to %d characters", PF_NAME_MAX); return NULL; }
    return n;
}

static const struct { const char *name; uint8_t type, regs; } TYPES[] = {
    { "u16", T_U16, 1 }, { "s16", T_S16, 1 }, { "u32", T_U32, 2 }, { "s32", T_S32, 2 },
    { "u32le", T_U32LE, 2 }, { "s32le", T_S32LE, 2 }, { "u8h", T_U8H, 1 }, { "u8l", T_U8L, 1 },
    { "s8h", T_S8H, 1 }, { "s8l", T_S8L, 1 }, { "coil", T_COIL, 0 },
};

static int type_of(const char *t)
{
    for (unsigned i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) if (!strcmp(TYPES[i].name, t)) return (int)i;
    return -1;
}

static int op_of(const char *t)
{
    return !strcmp(t, "==") ? OP_EQ : !strcmp(t, "!=") ? OP_NE : !strcmp(t, ">") ? OP_GT : !strcmp(t, "<") ? OP_LT : -1;
}

/* "cell1..16": base "cell", from 1, to 16. False if not a list. */
static bool range(const char *t, char *base, size_t bmax, int *from, int *to)
{
    const char *dd = strstr(t, ".."), *d;
    char *e;
    if (!dd) return false;
    for (d = dd; d > t && isdigit((unsigned char)d[-1]); d--) { }
    if (d == dd || d == t || (size_t)(d - t) >= bmax) return false;
    memcpy(base, t, (size_t)(d - t));
    base[d - t] = 0;
    *from = (int)strtol(d, NULL, 10);
    *to = (int)strtol(dd + 2, &e, 10);
    return !*e && e != dd + 2 && *to >= *from && *to - *from < 64;
}

static pf_field_t *field_new(ps_t *s, const char *id, const char *name)
{
    profile_t *p = s->p;
    if (!id_ok(id)) { fail(s, "bad id '%s': 1 to %d of a-z 0-9 _", id, PF_ID_MAX); return NULL; }
    if (profile_field(p, id) >= 0) { fail(s, "'%s' is declared twice", id); return NULL; }
    if (p->nfield == PF_FIELDS) { fail(s, "too many fields (%d)", PF_FIELDS); return NULL; }
    pf_field_t *f = &p->field[p->nfield];
    memset(f, 0, sizeof *f);
    f->scale = 1;
    f->prec = -1;
    f->mask = 0xFFFFFFFFu;
    f->block = 0xFF;
    if (!pool_add(s, id, &f->id) || !pool_add(s, name, &f->name)) return NULL;
    p->nfield++;
    return f;
}

/* Is the field inside its block? regs: registers it spans; 0 for a coil. */
static bool place(ps_t *s, const pf_field_t *f, int regs)
{
    const pf_block_t *b = &s->p->block[f->block];
    bool bits = b->fn <= 2;
    if (bits && regs) return fail(s, "block %s holds coils only", pf_str(s->p, b->id));
    if (!bits && !regs) return fail(s, "coils are read from a coil or discrete block");
    if (f->addr < b->addr || (uint32_t)f->addr + (uint32_t)(regs ? regs : 1) > (uint32_t)b->addr + b->count)
        return fail(s, "0x%04x is outside block %s", f->addr, pf_str(s->p, b->id));
    return true;
}

enum { M_SCALE = 1, M_UNIT = 2, M_HA = 4, M_DIAG = 8, M_WITHIN = 16, M_NODATA = 32 };

/* A word with a meaning of its own: never taken for a unit where it is not allowed. */
static bool keyword(const char *t)
{
    return t[0] == '*' || t[0] == '+' || !strcmp(t, "nodata") || !strcmp(t, "diag") || !strcmp(t, "within") ||
           !strncmp(t, "dc=", 3) || !strncmp(t, "sc=", 3) || !strncmp(t, "prec=", 5);
}

/* The words after a field's own: scale, offset, unit, HA hints, ... */
static bool mods(ps_t *s, pf_field_t *f, int from, unsigned allow)
{
    bool unit = false;
    for (int i = from; i < s->ntok; i++) {
        char *t = s->tok[i];
        int64_t l;
        float x;
        if (t[0] == '*' && (allow & M_SCALE)) {
            if (!num_f(t + 1, &x) || x == 0) return fail(s, "bad scale '%s'", t);
            f->scale = x;
        } else if (t[0] == '+' && (allow & M_SCALE)) {
            if (!num_i(t + 1, -2000000000LL, 2000000000LL, &l)) return fail(s, "bad offset '%s'", t);
            f->offset = (int32_t)l;
        } else if (!strcmp(t, "nodata") && (allow & M_NODATA)) {
            while (i + 1 < s->ntok && num_i(s->tok[i + 1], 0, 0xFFFFFFFFLL, &l)) {
                if (f->nnodata == 4) return fail(s, "at most 4 nodata values");
                f->nodata[f->nnodata++] = (uint32_t)l;
                i++;
            }
            if (!f->nnodata) return fail(s, "nodata needs values");
        } else if (!strcmp(t, "diag") && (allow & M_DIAG)) {
            f->flags |= PFF_DIAG;
        } else if (!strncmp(t, "dc=", 3) && (allow & M_HA)) {
            if (!word_ok(t + 3)) return fail(s, "bad device class '%s'", t + 3);
            if (!pool_add(s, t + 3, &f->dc)) return false;
        } else if (!strncmp(t, "sc=", 3) && (allow & M_HA)) {
            if (strcmp(t + 3, "measurement") && strcmp(t + 3, "total") && strcmp(t + 3, "total_increasing"))
                return fail(s, "state class is measurement, total or total_increasing");
            if (!pool_add(s, t + 3, &f->sc)) return false;
        } else if (!strncmp(t, "prec=", 5) && (allow & M_HA)) {
            if (!num_i(t + 5, 0, 6, &l)) return fail(s, "prec is 0 to 6");
            f->prec = (int8_t)l;
        } else if (!strcmp(t, "within") && (allow & M_WITHIN)) {
            char *dd = i + 1 < s->ntok ? strstr(s->tok[i + 1], "..") : NULL;
            if (!dd) return fail(s, "within needs lo..hi");
            *dd = 0;
            if (!num_f(s->tok[i + 1], &f->lo) || !num_f(dd + 2, &f->hi) || f->hi < f->lo) return fail(s, "within needs lo..hi");
            f->flags |= PFF_WITHIN;
            i++;
        } else if ((allow & M_UNIT) && !unit && !keyword(t) && t[0] != '"' && !strchr(t, '=') && strlen(t) <= 15) {
            unit = true;
            if (!pool_add(s, t, &f->unit)) return false;
        } else {
            return fail(s, "unknown word '%s'", t);
        }
    }
    return true;
}

/* One field, or one for each number of a list (id "cell1..16", name "Cell #"), from a
 * template: addresses step by `regs` (a coil: by 1). */
static bool each(ps_t *s, const char *idt, const char *name, const pf_field_t *tpl, int regs, int modfrom, unsigned allow)
{
    char base[PF_ID_MAX + 1], id[PF_ID_MAX + 8], nm[PF_NAME_MAX + 8];
    int from = 0, to = 0;
    bool list = range(idt, base, sizeof base, &from, &to);
    const char *hash = strchr(name, '#');
    if (list && !hash) return fail(s, "a list's name needs # for its number");
    for (int k = from; k <= to; k++) {
        if (list) {
            snprintf(id, sizeof id, "%s%d", base, k);
            snprintf(nm, sizeof nm, "%.*s%d%s", (int)(hash - name), name, k, hash + 1);
            if (strlen(nm) > PF_NAME_MAX) return fail(s, "the name must be \"quoted\", 1 to %d characters", PF_NAME_MAX);
        } else {
            snprintf(id, sizeof id, "%s", idt);
            snprintf(nm, sizeof nm, "%s", name);
        }
        uint16_t idoff, nmoff;
        pf_field_t *f = field_new(s, id, nm);
        if (!f) return false;
        idoff = f->id;
        nmoff = f->name;
        *f = *tpl;
        f->id = idoff;
        f->name = nmoff;
        f->addr = (uint16_t)(tpl->addr + (k - from) * (regs ? regs : 1));
        if (f->block != 0xFF && !place(s, f, regs)) return false;
        if (!mods(s, f, modfrom, allow)) return false;
    }
    return true;
}

static void tpl_init(pf_field_t *t)
{
    memset(t, 0, sizeof *t);
    t->scale = 1;
    t->prec = -1;
    t->mask = 0xFFFFFFFFu;
    t->block = 0xFF;
}

/* id "name" block address: the start every read field shares. */
static bool where(ps_t *s, pf_field_t *t, const char **name)
{
    int64_t a;
    if (!(*name = name_of(s, 2))) return false;
    int b = profile_block(s->p, s->tok[3]);
    if (b < 0) return fail(s, "no block '%s'", s->tok[3]);
    if (!num_i(s->tok[4], 0, 0xFFFF, &a)) return fail(s, "bad address '%s'", s->tok[4]);
    t->block = (uint8_t)b;
    t->addr = (uint16_t)a;
    return true;
}

/* sensor id "name" block address type [...] */
static bool st_sensor(ps_t *s)
{
    pf_field_t t;
    const char *name;
    tpl_init(&t);
    if (s->ntok < 6) return fail(s, "sensor needs: id \"name\" block address type");
    if (!where(s, &t, &name)) return false;
    int ti = type_of(s->tok[5]);
    if (ti < 0) return fail(s, "unknown type '%s'", s->tok[5]);
    t.kind = PF_SENSOR;
    t.type = TYPES[ti].type;
    return each(s, s->tok[1], name, &t, TYPES[ti].regs, 6, M_SCALE | M_UNIT | M_HA | M_DIAG | M_NODATA);
}

/* binary id "name" block address bit N | type op value */
static bool st_binary(ps_t *s)
{
    pf_field_t t;
    const char *name;
    int64_t l;
    tpl_init(&t);
    if (s->ntok < 6) return fail(s, "binary needs: id \"name\" block address, then bit N, coil, or type op value");
    if (!where(s, &t, &name)) return false;
    t.kind = PF_BINARY;
    if (!strcmp(s->tok[5], "bit")) {
        if (s->ntok < 7 || !num_i(s->tok[6], 0, 15, &l)) return fail(s, "bit must be 0 to 15");
        t.type = T_BIT;
        t.bit = (uint8_t)l;
        return each(s, s->tok[1], name, &t, 1, 7, M_HA | M_DIAG);
    }
    if (!strcmp(s->tok[5], "coil")) {
        t.type = T_COIL;
        return each(s, s->tok[1], name, &t, 0, 6, M_HA | M_DIAG);
    }
    int ti = type_of(s->tok[5]);
    if (ti < 0 || !TYPES[ti].regs) return fail(s, "unknown type '%s'", s->tok[5]);
    if (s->ntok < 8) return fail(s, "binary needs: type op value");
    int op = op_of(s->tok[6]);
    if (op < 0) return fail(s, "unknown comparison '%s'", s->tok[6]);
    if (!num_f(s->tok[7], &t.k)) return fail(s, "bad value '%s'", s->tok[7]);
    t.type = TYPES[ti].type;
    t.op = (uint8_t)op;
    return each(s, s->tok[1], name, &t, TYPES[ti].regs, 8, M_SCALE | M_HA | M_DIAG);
}

/* text id "name" block address [type] enum [mask=M] v=name... | bits n=name... | ascii N | asciilo N */
static bool st_text(ps_t *s)
{
    profile_t *p = s->p;
    pf_field_t t;
    const char *name;
    int64_t l;
    int i = 5, regs = 1;
    tpl_init(&t);
    t.type = T_U16;
    if (s->ntok < 6) return fail(s, "text needs: id \"name\" block address, then enum, bits, ascii or asciilo");
    if (!where(s, &t, &name)) return false;
    int ti = type_of(s->tok[5]);
    if (ti >= 0 && TYPES[ti].regs) { t.type = TYPES[ti].type; regs = TYPES[ti].regs; i = 6; }
    const char *how = i < s->ntok ? s->tok[i++] : "";
    if (!strcmp(how, "ascii") || !strcmp(how, "asciilo")) {
        char base[PF_ID_MAX + 1];
        int a, b;
        if (i >= s->ntok || !num_i(s->tok[i], 1, 24, &l)) return fail(s, "ascii needs a length of 1 to 24 registers");
        if (range(s->tok[1], base, sizeof base, &a, &b)) return fail(s, "ascii cannot be a list");
        if (p->nstr == PF_STRS) return fail(s, "too many ascii texts (%d)", PF_STRS);
        t.kind = PF_ASCII;
        t.type = how[5] ? T_ASCIILO : T_ASCII;
        t.bit = (uint8_t)l;
        t.str = p->nstr++;
        return each(s, s->tok[1], name, &t, (int)l, i + 1, M_DIAG);
    }
    if (strcmp(how, "enum") && strcmp(how, "bits")) return fail(s, "text needs enum, bits, ascii or asciilo");
    t.kind = how[0] == 'e' ? PF_ENUM : PF_BITS;
    t.vname = p->nvname;
    for (; i < s->ntok; i++) {
        char *w = s->tok[i], *eq = strchr(w, '=');
        if (!strcmp(w, "diag")) { t.flags |= PFF_DIAG; continue; }
        if (!eq) return fail(s, "expected value=name, not '%s'", w);
        *eq = 0;
        if (!strcmp(w, "mask") && t.kind == PF_ENUM) {
            if (!num_i(eq + 1, 1, 0xFFFFFFFFLL, &l)) return fail(s, "bad mask '%s'", eq + 1);
            t.mask = (uint32_t)l;
            continue;
        }
        if (!num_i(w, 0, t.kind == PF_BITS ? 31 : 0xFFFFFFFFLL, &l)) return fail(s, "bad value '%s'", w);
        char *vn = unq(eq + 1);
        if (!vn || !*vn || strlen(vn) > PF_NAME_MAX) return fail(s, "the name must be \"quoted\", 1 to %d characters", PF_NAME_MAX);
        if (p->nvname == PF_VNAMES) return fail(s, "too many value names (%d)", PF_VNAMES);
        p->vname[p->nvname].value = (uint32_t)l;
        if (!pool_add(s, vn, &p->vname[p->nvname].name)) return false;
        p->nvname++;
    }
    t.nvname = (uint16_t)(p->nvname - t.vname);
    if (!t.nvname) return fail(s, "%s needs at least one value=name", how);
    return each(s, s->tok[1], name, &t, regs, s->ntok, 0);
}

static const struct { const char *name; uint8_t fn, args; } FNS[] = {    /* args: 0 a list, 1, 2 fields, 3 a field and a number */
    { "sum", F_SUM, 0 }, { "min", F_MIN, 0 }, { "max", F_MAX, 0 }, { "avg", F_AVG, 0 }, { "delta", F_DELTA, 0 },
    { "argmin", F_ARGMIN, 0 }, { "argmax", F_ARGMAX, 0 }, { "count", F_COUNT, 0 },
    { "sub", F_SUB, 2 }, { "mul", F_MUL, 2 }, { "pos", F_POS, 1 }, { "neg", F_NEG, 1 }, { "abs", F_ABS, 1 },
    { "eq", F_EQ, 3 }, { "ne", F_NE, 3 }, { "gt", F_GT, 3 }, { "lt", F_LT, 3 }, { "anybits", F_ANYBITS, 3 }, { "nobits", F_NOBITS, 3 },
};

/* A field, or each of a list, into arg[]. 0 added if the word is not a field. */
static int args_of(ps_t *s, const char *w)
{
    profile_t *p = s->p;
    char base[PF_ID_MAX + 1], id[PF_ID_MAX + 8];
    int from, to, n = 0;
    if (!range(w, base, sizeof base, &from, &to)) { from = to = 0; base[0] = 0; }
    for (int k = from; k <= to; k++) {
        if (base[0]) snprintf(id, sizeof id, "%s%d", base, k); else snprintf(id, sizeof id, "%s", w);
        int f = profile_field(p, id);
        if (f < 0) return n ? -1 : 0;
        if (p->field[f].kind == PF_ASCII) return -2;
        if (p->narg == PF_ARGS) return -3;
        p->arg[p->narg++] = (uint16_t)f;
        n++;
    }
    return n;
}

/* calc id "name" function arguments [...] */
static bool st_calc(ps_t *s)
{
    profile_t *p = s->p;
    pf_field_t t;
    const char *name;
    int fi = -1, i = 4;
    tpl_init(&t);
    if (s->ntok < 5) return fail(s, "calc needs: id \"name\" function arguments");
    if (!(name = name_of(s, 2))) return false;
    for (unsigned k = 0; k < sizeof FNS / sizeof FNS[0]; k++) if (!strcmp(FNS[k].name, s->tok[3])) fi = (int)k;
    if (fi < 0) return fail(s, "unknown function '%s'", s->tok[3]);
    t.fn = FNS[fi].fn;
    t.kind = t.fn >= F_EQ ? PF_CALCBIN : PF_CALC;
    t.arg = p->narg;
    int want = FNS[fi].args == 3 ? 1 : FNS[fi].args;
    while (i < s->ntok && (want == 0 || t.narg < want)) {
        int n = args_of(s, s->tok[i]);
        if (n == -2) return fail(s, "'%s' is text", s->tok[i]);
        if (n == -3) return fail(s, "too many calc arguments (%d)", PF_ARGS);
        if (n <= 0) {
            if (want || t.narg) break;
            return fail(s, "no field '%s'", s->tok[i]);
        }
        t.narg = (uint16_t)(t.narg + n);
        i++;
    }
    if (!t.narg && i < s->ntok) return fail(s, "no field '%s'", s->tok[i]);
    if (want && t.narg != want) return fail(s, "%s needs %d field%s", FNS[fi].name, want, want > 1 ? "s" : "");
    if (FNS[fi].args == 3) {
        int64_t l;
        if (i >= s->ntok) return fail(s, "%s needs a field and a number", FNS[fi].name);
        if (t.fn >= F_ANYBITS) {
            if (!num_i(s->tok[i], 1, 0xFFFFFFFFLL, &l)) return fail(s, "bad mask '%s'", s->tok[i]);
            t.mask = (uint32_t)l;
        } else if (!num_f(s->tok[i], &t.k)) {
            return fail(s, "bad value '%s'", s->tok[i]);
        }
        i++;
    }
    return each(s, s->tok[1], name, &t, 0, i, FNS[fi].args == 0 ? M_UNIT | M_HA | M_DIAG | M_WITHIN : M_UNIT | M_HA | M_DIAG);
}

/* reject block if zero [fields] | reject block if field op value */
static bool st_reject(ps_t *s)
{
    profile_t *p = s->p;
    if (s->ntok < 4 || strcmp(s->tok[2], "if"))
        return fail(s, "reject needs: block if zero [fields], or block if field op value");
    int b = profile_block(p, s->tok[1]);
    if (b < 0) return fail(s, "no block '%s'", s->tok[1]);
    if (p->nreject == PF_REJECTS) return fail(s, "too many reject rules (%d)", PF_REJECTS);
    pf_reject_t *j = &p->reject[p->nreject];
    memset(j, 0, sizeof *j);
    j->block = (uint8_t)b;
    int first = 3, last = s->ntok;
    if (!strcmp(s->tok[3], "zero")) {
        j->kind = s->ntok == 4 ? RJ_ZERO : RJ_ZEROS;
        first = 4;
        if (last - first > 4) return fail(s, "at most 4 fields");
    } else {
        if (s->ntok != 6) return fail(s, "reject needs: block if zero [fields], or block if field op value");
        int op = op_of(s->tok[4]);
        if (op < 0) return fail(s, "unknown comparison '%s'", s->tok[4]);
        if (!num_f(s->tok[5], &j->k)) return fail(s, "bad value '%s'", s->tok[5]);
        j->kind = RJ_CMP;
        j->op = (uint8_t)op;
        last = 4;
    }
    for (int i = first; i < last; i++) {
        int f = profile_field(p, s->tok[i]);
        if (f < 0) return fail(s, "no field '%s'", s->tok[i]);
        if (p->field[f].block != b) return fail(s, "'%s' is not in block %s", s->tok[i], s->tok[1]);
        if (p->field[f].kind == PF_ASCII || p->field[f].kind >= PF_CALC) return fail(s, "'%s' is not a number read from the block", s->tok[i]);
        j->field[j->nfield++] = (uint16_t)f;
    }
    p->nreject++;
    return true;
}

/* block id kind address count period [info] */
static bool st_block(ps_t *s)
{
    profile_t *p = s->p;
    static const struct { const char *name; uint8_t fn; } KINDS[] = { { "coil", 1 }, { "discrete", 2 }, { "hold", 3 }, { "input", 4 } };
    int64_t a, n, v;
    if (s->ntok < 6 || s->ntok > 7) return fail(s, "block needs: id kind address count period [info]");
    if (!id_ok(s->tok[1])) return fail(s, "bad id '%s': 1 to %d of a-z 0-9 _", s->tok[1], PF_ID_MAX);
    if (profile_block(p, s->tok[1]) >= 0) return fail(s, "'%s' is declared twice", s->tok[1]);
    if (p->nblock == PF_BLOCKS) return fail(s, "too many blocks (%d)", PF_BLOCKS);
    pf_block_t *b = &p->block[p->nblock];
    memset(b, 0, sizeof *b);
    for (unsigned k = 0; k < 4; k++) if (!strcmp(KINDS[k].name, s->tok[2])) b->fn = KINDS[k].fn;
    if (!b->fn) return fail(s, "unknown block kind '%s'", s->tok[2]);
    if (!num_i(s->tok[3], 0, 0xFFFF, &a)) return fail(s, "bad address '%s'", s->tok[3]);
    if (b->fn >= 3 && !num_i(s->tok[4], 1, 125, &n)) return fail(s, "a block of holding or input registers counts 1 to 125");
    if (b->fn <= 2 && !num_i(s->tok[4], 1, 2000, &n)) return fail(s, "a block of coils or discrete inputs counts 1 to 2000");
    if (a + n > 0x10000) return fail(s, "the block runs past 0xFFFF");
    char *t = s->tok[5], unit = t[0] ? t[strlen(t) - 1] : 0;
    uint32_t mul = unit == 's' ? 1000u : unit == 'm' ? 60000u : unit == 'h' ? 3600000u : 0;
    if (mul) t[strlen(t) - 1] = 0;
    if (!mul || !num_i(t, 1, 86400, &v) || v * mul > 86400000LL) { if (mul) t[strlen(t)] = (char)unit; return fail(s, "bad period '%s'", s->tok[5]); }
    if (s->ntok == 7 && strcmp(s->tok[6], "info")) return fail(s, "unknown word '%s'", s->tok[6]);
    if (!pool_add(s, s->tok[1], &b->id)) return false;
    b->addr = (uint16_t)a;
    b->count = (uint16_t)n;
    b->period_ms = (uint32_t)(v * mul);
    b->info = s->ntok == 7;
    p->nblock++;
    return true;
}

static bool ms_of(const char *t, int64_t lo, int64_t hi, uint16_t *out)
{
    char b[16];
    int64_t v;
    size_t n = strlen(t);
    if (n < 3 || n >= sizeof b || strcmp(t + n - 2, "ms")) return false;
    memcpy(b, t, n - 2);
    b[n - 2] = 0;
    if (!num_i(b, lo, hi, &v)) return false;
    *out = (uint16_t)v;
    return true;
}

/* profile, name, maker, line, stale, timeout, turnaround */
static bool st_head(ps_t *s, const char *w)
{
    profile_t *p = s->p;
    int64_t v;
    if (!strcmp(w, "profile")) {
        const char *id = s->tok[1];
        if (s->ntok != 3) return fail(s, "profile needs: id version");
        size_t n = strlen(id);
        bool ok = n && n <= PF_ID_MAX;
        for (const char *c = id; *c; c++) ok = ok && ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-');
        if (!ok) return fail(s, "bad profile id '%s': 1 to %d of a-z 0-9 -", id, PF_ID_MAX);
        if (!num_i(s->tok[2], 0, 1000, &v) || v != 1) return fail(s, "format version %s is not known; this firmware reads 1", s->tok[2]);
        return pool_add(s, id, &p->id);
    }
    if (s->ntok != 2 && strcmp(w, "line")) return fail(s, "%s needs one value", w);
    if (!strcmp(w, "name") || !strcmp(w, "maker")) {
        const char *n = name_of(s, 1);
        return n && pool_add(s, n, w[0] == 'n' ? &p->name : &p->maker);
    }
    if (!strcmp(w, "line")) {
        const char *f = s->ntok == 3 ? s->tok[2] : "";
        if (s->ntok != 3 || !num_i(s->tok[1], 300, 2000000, &v)) return fail(s, "line needs: baud frame, e.g. 9600 8N1");
        if (strlen(f) != 3 || (f[0] != '7' && f[0] != '8') || !strchr("NEO", f[1]) || (f[2] != '1' && f[2] != '2'))
            return fail(s, "bad line settings '%s'", f);
        p->baud = (uint32_t)v;
        memcpy(p->frame, f, 4);
        return true;
    }
    if (!strcmp(w, "stale")) {
        if (!num_i(s->tok[1], 1, 255, &v)) return fail(s, "stale must be 1 to 255");
        p->stale = (uint8_t)v;
        return true;
    }
    if (!strcmp(w, "timeout")) return ms_of(s->tok[1], 50, 10000, &p->timeout_ms) || fail(s, "timeout must be 50ms to 10000ms");
    return ms_of(s->tok[1], 0, 1000, &p->turn_ms) || fail(s, "turnaround must be 0ms to 1000ms");
}

bool profile_parse(profile_t *p, const char *text, size_t n, char *err, size_t errlen)
{
    static ps_t s;
    memset(p, 0, sizeof *p);
    p->npool = 1;
    p->stale = 5;
    p->timeout_ms = 1000;
    memset(&s, 0, sizeof s);
    s.p = p;
    s.err = err;
    s.errlen = errlen;
    if (errlen) err[0] = 0;
    if (n > PF_TEXT_MAX) return fail(&s, "the profile is longer than %u bytes", PF_TEXT_MAX);
    bool first = true;
    for (size_t at = 0; at < n;) {
        size_t e = at;
        while (e < n && text[e] != '\n') e++;
        s.line++;
        if (e - at > LINE_MAX) return fail(&s, "longer than %d characters", LINE_MAX);
        for (size_t k = at; k < e; k++)
            if ((unsigned char)text[k] < 0x20 && text[k] != '\t' && text[k] != '\r') return fail(&s, "a control character");
        memcpy(s.buf, text + at, e - at);
        s.buf[e - at] = 0;
        at = e + 1;
        if (!tokens(&s)) return false;
        if (!s.ntok) continue;
        const char *w = s.tok[0];
        bool ok;
        if (first && strcmp(w, "profile")) return fail(&s, "the first statement must be: profile id 1");
        if (!first && !strcmp(w, "profile")) return fail(&s, "profile is given twice");
        if (!strcmp(w, "profile") || !strcmp(w, "name") || !strcmp(w, "maker") || !strcmp(w, "line") ||
            !strcmp(w, "stale") || !strcmp(w, "timeout") || !strcmp(w, "turnaround")) ok = st_head(&s, w);
        else if (!strcmp(w, "block")) ok = st_block(&s);
        else if (!strcmp(w, "sensor")) ok = st_sensor(&s);
        else if (!strcmp(w, "binary")) ok = st_binary(&s);
        else if (!strcmp(w, "text")) ok = st_text(&s);
        else if (!strcmp(w, "calc")) ok = st_calc(&s);
        else if (!strcmp(w, "reject")) ok = st_reject(&s);
        else if (!strcmp(w, "number") || !strcmp(w, "select") || !strcmp(w, "switch") || !strcmp(w, "button"))
            ok = fail(&s, "controls come in phase 2");
        else ok = fail(&s, "unknown statement '%s'", w);
        if (!ok) return false;
        first = false;
    }
    s.line = 0;
    if (first) return fail(&s, "the profile is empty");
    if (!p->nblock) return fail(&s, "no blocks");
    return true;
}
