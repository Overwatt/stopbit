/*
 * pfwrite.c — Commands into writes (pfwrite.h). Numbers are read by hand, as profile.c reads
 * them: no exponents, no "inf" or "nan", nothing a locale could change.
 */
#include "pfwrite.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool no(char *why, size_t max, const char *s) { if (max) snprintf(why, max, "%s", s); return false; }

/* [-]digits[.digits]; NaN if it is not that. */
static double number(const char *t)
{
    double v = 0, f = 1;
    bool neg = *t == '-', any = false;
    if (*t == '-' || *t == '+') t++;
    for (; *t >= '0' && *t <= '9'; t++) { v = v * 10 + (*t - '0'); any = true; if (v > 1e12) return NAN; }
    if (*t == '.') for (t++; *t >= '0' && *t <= '9'; t++) { f /= 10; v += (*t - '0') * f; any = true; }
    return any && !*t ? (neg ? -v : v) : NAN;
}

static bool two(const char *t) { return t[0] >= '0' && t[0] <= '9' && t[1] >= '0' && t[1] <= '9'; }

bool pf_command(const profile_t *p, int i, const char *s, pf_write_t *w, char *why, size_t max)
{
    const pf_field_t *f = &p->field[i];
    memset(w, 0, sizeof *w);
    w->addr = f->addr;
    w->regs = 1;
    w->mask[0] = (uint16_t)f->mask;
    switch (f->kind) {
    case PF_NUMBER: {
        double v = number(s), step = f->k > 0 ? f->k : 1;
        if (isnan(v)) return no(why, max, "not a number");
        v = round(v / step) * step;
        if (v < f->lo - step / 1000 || v > f->hi + step / 1000) return no(why, max, "out of range");
        double r = round(v / f->scale - f->offset);
        uint32_t raw = f->type == T_S16 || f->type == T_S32 || f->type == T_S32LE ? (uint32_t)(int32_t)r : (uint32_t)r;
        w->mask[0] = 0xFFFF;
        if (f->type == T_U16 || f->type == T_S16) { w->value[0] = (uint16_t)raw; return true; }
        bool le = f->type == T_U32LE || f->type == T_S32LE;
        w->regs = 2;
        w->mask[1] = 0xFFFF;
        w->value[0] = (uint16_t)(le ? raw : raw >> 16);
        w->value[1] = (uint16_t)(le ? raw >> 16 : raw);
        return true;
    }
    case PF_SELECT:
        for (int k = 0; k < f->nvname; k++) {
            const pf_vname_t *n = &p->vname[f->vname + k];
            if (!strcmp(pf_str(p, n->name), s)) { w->value[0] = (uint16_t)n->value; return true; }
        }
        return no(why, max, "not one of the choices");
    case PF_SWITCH:
        if (!strcmp(s, "ON")) w->value[0] = (uint16_t)f->k;
        else if (!strcmp(s, "OFF")) w->value[0] = (uint16_t)f->lo;
        else return no(why, max, "not ON or OFF");
        return true;
    case PF_TIME: {
        if (strlen(s) != 5 || s[2] != ':' || !two(s) || !two(s + 3)) return no(why, max, "not a time of day");
        unsigned h = (unsigned)(s[0] - '0') * 10u + (unsigned)(s[1] - '0'), m = (unsigned)(s[3] - '0') * 10u + (unsigned)(s[4] - '0');
        if (h > 23 || m > 59) return no(why, max, "not a time of day");
        w->value[0] = (uint16_t)(m << 8 | h);
        return true;
    }
    default:
        return no(why, max, "not a control");
    }
}

bool pf_merge(const pf_write_t *w, const uint16_t have[2], uint16_t out[2])
{
    bool change = false;
    for (int r = 0; r < w->regs; r++) {
        out[r] = (uint16_t)((have[r] & ~w->mask[r]) | (w->value[r] & w->mask[r]));
        change = change || out[r] != have[r];
    }
    return change;
}
