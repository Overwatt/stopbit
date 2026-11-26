/*
 * pfdecode.h — A device's values, from its blocks' replies, by its profile (profile.h).
 * Each field has a number (v; for a binary 0 or 1), its raw reading, and whether it is
 * known now; an ascii field's text is in str[field.str].
 */
#ifndef PFDECODE_H
#define PFDECODE_H
#include "profile.h"

typedef struct { float v; uint32_t raw; } pf_val_t;
typedef struct {
    pf_val_t val[PF_FIELDS];
    uint8_t  ok[(PF_FIELDS + 7) / 8];
    char     str[PF_STRS][PF_STR_LEN];
} pf_state_t;

static inline bool pf_ok(const pf_state_t *s, int i) { return s->ok[i >> 3] >> (i & 7) & 1u; }

void   pf_state_init(pf_state_t *s);       /* nothing known */
/* A block's reply: register bytes (2 a register, high byte first) or coil bits (as Modbus
 * packs them). False, nothing changed, if the size is not the block's or a reject rule
 * says no. Then every calc is worked out again. */
bool   pf_decode(const profile_t *p, int block, const uint8_t *d, size_t n, pf_state_t *s);
/* A block gone stale: its fields unknown, and the calcs again. */
void   pf_drop(const profile_t *p, int block, pf_state_t *s);
/* A text field: an enum's name ("Unknown (5)" if none), the set bits' names joined by
 * "; " ("None" if none), or the ascii. "" if not known. */
size_t pf_text(const profile_t *p, int i, const pf_state_t *s, char *out, size_t max);
int    pf_prec(const pf_field_t *f);       /* decimals to show */
/* v with `prec` decimals, as JSON has it; "null" if not a finite number that fits.
 * 0 if out is too small. */
size_t pf_num(char *out, size_t max, float v, int prec);

#endif
