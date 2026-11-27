/*
 * pfdecode.c — A block's reply into a device's values, by its profile: registers and
 * bits to numbers, value names and text, then every calc again. And numbers written out
 * by hand: the gateway's printf has no floating point (newlib nano).
 */
#include "pfdecode.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t reg(const uint8_t *d, int i) { return (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]); }

static void set_ok(pf_state_t *s, int i, bool ok)
{
    if (ok) s->ok[i >> 3] |= (uint8_t)(1u << (i & 7));
    else s->ok[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

void pf_state_init(pf_state_t *s) { memset(s, 0, sizeof *s); }

static uint32_t raw_of(const pf_block_t *b, const pf_field_t *f, const uint8_t *d)
{
    int i = f->addr - b->addr;
    switch (f->type) {
    case T_U32: case T_S32:     return (uint32_t)reg(d, i) << 16 | reg(d, i + 1);
    case T_U32LE: case T_S32LE: return (uint32_t)reg(d, i + 1) << 16 | reg(d, i);
    case T_U8H: case T_S8H:     return d[2 * i];
    case T_U8L: case T_S8L:     return d[2 * i + 1];
    case T_BIT:                 return (uint32_t)(reg(d, i) >> f->bit & 1u);
    case T_COIL:                return (uint32_t)(d[i / 8] >> (i % 8) & 1u);
    default:                    return reg(d, i);
    }
}

/* A double as a float; one beyond the float range becomes infinite (a plain cast is undefined). */
static float to_float(double v)
{
    return v > (double)FLT_MAX ? INFINITY : v < -(double)FLT_MAX ? -INFINITY : (float)v;
}

static float value_of(const pf_field_t *f, uint32_t raw)
{
    double v;
    switch (f->type) {
    case T_S16:               v = (int16_t)raw; break;
    case T_S32: case T_S32LE: v = (int32_t)raw; break;
    case T_S8H: case T_S8L:   v = (int8_t)raw; break;
    default:                  v = raw;
    }
    return to_float((v + f->offset) * (double)f->scale);
}

static bool cmp(uint8_t op, float a, float k)
{
    switch (op) {
    case OP_EQ: return a == k;
    case OP_NE: return a != k;
    case OP_GT: return a > k;
    default:    return a < k;
    }
}

/* Printable characters up to a NUL, trailing spaces trimmed. Of each register: its high
 * byte then its low (ascii), its low byte alone (asciilo), or its low byte then its high
 * (asciile). */
static void ascii(const pf_block_t *b, const pf_field_t *f, const uint8_t *d, char *out)
{
    int i = f->addr - b->addr, n = 0;
    bool end = false;
    for (int r = 0; r < f->bit && !end; r++)
        for (int k = 0; k < (f->type == T_ASCIILO ? 1 : 2) && !end; k++) {
            uint8_t c = d[2 * (i + r) + (f->type == T_ASCII ? k : 1 - k)];
            if (!c) end = true;
            else if (c >= 0x20 && c <= 0x7E && n < PF_STR_LEN - 1) out[n++] = (char)c;
        }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void calc(const profile_t *p, pf_state_t *s)
{
    for (int i = 0; i < p->nfield; i++) {
        const pf_field_t *f = &p->field[i];
        if (f->kind < PF_CALC) continue;
        const uint16_t *a = &p->arg[f->arg];
        bool ok = true;
        float v = 0;
        for (int k = 0; k < f->narg; k++) ok = ok && pf_ok(s, a[k]);
        float x = s->val[a[0]].v;
        if (ok) switch (f->fn) {
        case F_SUB:     v = x - s->val[a[1]].v; break;
        case F_MUL:     v = x * s->val[a[1]].v; break;
        case F_POS:     v = x > 0 ? x : 0; break;
        case F_NEG:     v = x < 0 ? -x : 0; break;
        case F_ABS:     v = x < 0 ? -x : x; break;
        case F_EQ: case F_NE: case F_GT: case F_LT: v = cmp((uint8_t)(f->fn - F_EQ), x, f->k); break;
        case F_ANYBITS: v = (s->val[a[0]].raw & f->mask) != 0; break;
        case F_NOBITS:  v = (s->val[a[0]].raw & f->mask) == 0; break;
        default: {                                   /* over a list, inside `within` if given */
            int n = 0, at = 0;
            float sum = 0, lo = 0, hi = 0;
            for (int k = 0; k < f->narg; k++) {
                float y = s->val[a[k]].v;
                if ((f->flags & PFF_WITHIN) && (y < f->lo || y > f->hi)) continue;
                if (!n || y < lo) { lo = y; if (f->fn == F_ARGMIN) at = k + 1; }
                if (!n || y > hi) { hi = y; if (f->fn == F_ARGMAX) at = k + 1; }
                sum += y;
                n++;
            }
            if (f->fn == F_COUNT) v = (float)n;
            else if (!n) ok = false;
            else v = f->fn == F_SUM ? sum : f->fn == F_MIN ? lo : f->fn == F_MAX ? hi :
                     f->fn == F_AVG ? sum / (float)n : f->fn == F_DELTA ? hi - lo : (float)at;
        }
        }
        s->val[i].v = v;
        s->val[i].raw = 0;
        set_ok(s, i, ok);
    }
}

bool pf_decode(const profile_t *p, int bi, const uint8_t *d, size_t n, pf_state_t *s)
{
    const pf_block_t *b = &p->block[bi];
    if (n != (b->fn <= 2 ? (b->count + 7u) / 8u : b->count * 2u)) return false;
    for (int r = 0; r < p->nreject; r++) {
        const pf_reject_t *j = &p->reject[r];
        bool hit = true;
        if (j->block != bi) continue;
        if (j->kind == RJ_ZERO) {
            for (size_t k = 0; k < n && hit; k++) hit = !d[k];
        } else if (j->kind == RJ_ZEROS) {
            for (int k = 0; k < j->nfield && hit; k++) hit = !raw_of(b, &p->field[j->field[k]], d);
        } else {
            const pf_field_t *f = &p->field[j->field[0]];
            hit = cmp(j->op, value_of(f, raw_of(b, f, d)), j->k);
        }
        if (hit) return false;
    }
    for (int i = 0; i < p->nfield; i++) {
        const pf_field_t *f = &p->field[i];
        if (f->block != bi) continue;
        if (f->kind == PF_ASCII) { ascii(b, f, d, s->str[f->str]); set_ok(s, i, true); continue; }
        uint32_t raw = raw_of(b, f, d);
        bool ok = true;
        for (int k = 0; k < f->nnodata; k++) ok = ok && raw != f->nodata[k];
        if (f->kind == PF_ENUM || f->kind == PF_SELECT || f->kind == PF_SWITCH) raw &= f->mask;
        if (f->kind == PF_SWITCH) ok = ok && ((float)raw == f->k || (float)raw == f->lo);
        if (f->kind == PF_TIME) ok = ok && (raw & 0xFFu) < 24 && raw >> 8 < 60;
        s->val[i].raw = raw;
        if (f->kind == PF_SENSOR || f->kind == PF_NUMBER) s->val[i].v = value_of(f, raw);
        else if (f->kind == PF_SWITCH) s->val[i].v = (float)raw == f->k;
        else if (f->kind == PF_BINARY && f->type != T_BIT && f->type != T_COIL) s->val[i].v = cmp(f->op, value_of(f, raw), f->k);
        else s->val[i].v = (float)raw;
        set_ok(s, i, ok);
    }
    calc(p, s);
    return true;
}

void pf_drop(const profile_t *p, int bi, pf_state_t *s)
{
    for (int i = 0; i < p->nfield; i++) if (p->field[i].block == bi) set_ok(s, i, false);
    calc(p, s);
}

size_t pf_text(const profile_t *p, int i, const pf_state_t *s, char *out, size_t max)
{
    const pf_field_t *f = &p->field[i];
    size_t n = 0;
    int w;
    if (!max) return 0;
    out[0] = 0;
    if (!pf_ok(s, i)) return 0;
    if (f->kind == PF_ASCII) w = snprintf(out, max, "%s", s->str[f->str]);
    else if (f->kind == PF_TIME) w = snprintf(out, max, "%02u:%02u", (unsigned)(s->val[i].raw & 0xFFu), (unsigned)(s->val[i].raw >> 8));
    else {
        uint32_t raw = s->val[i].raw;
        for (int k = 0; k < f->nvname; k++) {
            const pf_vname_t *v = &p->vname[f->vname + k];
            if ((f->kind == PF_ENUM || f->kind == PF_SELECT) ? v->value != raw : !(raw >> v->value & 1u)) continue;
            w = snprintf(out + n, max - n, "%s%s", n ? "; " : "", pf_str(p, v->name));
            if (w < 0 || (size_t)w >= max - n) break;
            n += (size_t)w;
            if (f->kind == PF_ENUM || f->kind == PF_SELECT) break;
        }
        if (n) return n;
        w = (f->kind == PF_ENUM || f->kind == PF_SELECT) ? snprintf(out, max, "Unknown (%lu)", (unsigned long)raw) : snprintf(out, max, "None");
    }
    return w < 0 ? 0 : (size_t)w < max ? (size_t)w : max - 1;
}

int pf_prec(const pf_field_t *f)
{
    if (f->prec >= 0) return f->prec;
    if (f->kind == PF_CALC) return f->fn == F_COUNT || f->fn == F_ARGMIN || f->fn == F_ARGMAX ? 0 : 3;
    if (f->kind != PF_SENSOR && f->kind != PF_NUMBER) return 0;
    float sc = f->scale < 0 ? -f->scale : f->scale;
    for (int k = 0; k <= 4; k++, sc *= 10) if (sc >= 0.999f) return k;
    return 4;
}

size_t pf_num(char *out, size_t max, float v, int prec)
{
    static const uint32_t P10[7] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };
    char d[24];
    int k = (int)sizeof d, n;
    if (prec < 0) prec = 0;
    if (prec > 6) prec = 6;
    double a = v < 0 ? -(double)v : (double)v;
    if (!(a * P10[prec] < 1.8e19))   /* NaN, infinite, too big */ { n = snprintf(out, max, "null"); return n < 0 || (size_t)n >= max ? 0 : (size_t)n; }
    uint64_t u = (uint64_t)(a * P10[prec] + 0.5), whole = u / P10[prec];
    uint32_t frac = (uint32_t)(u % P10[prec]);
    d[--k] = 0;
    do { d[--k] = (char)('0' + whole % 10u); whole /= 10u; } while (whole);
    n = snprintf(out, max, "%s%s", v < 0 && u ? "-" : "", d + k);
    if (prec && n >= 0 && (size_t)n < max) {
        int m = snprintf(out + n, max - (size_t)n, ".%0*lu", prec, (unsigned long)frac);
        n = m < 0 ? -1 : n + m;
    }
    return n < 0 || (size_t)n >= max ? 0 : (size_t)n;      /* 0: it did not fit */
}
