/*
 * profile.h — A device profile: what to read from a device on an RS485 port, and how to
 * show it. profile_parse() turns the text (docs/superpowers/specs/
 * 2026-11-26-device-profiles-design.md, "Profile format") into these tables;
 * pfdecode.c uses them. Strings live in `pool`, referred to by offset (0 is "").
 */
#ifndef PROFILE_H
#define PROFILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PF_TEXT_MAX  16384u
#define PF_BLOCKS    16
#define PF_FIELDS    160
#define PF_REJECTS   16
#define PF_VNAMES    256         /* enum values' and bits' names */
#define PF_ARGS      512         /* calc arguments */
#define PF_POOL      6144        /* ids, names, units, HA classes */
#define PF_STRS      8           /* ascii texts */
#define PF_STR_LEN   49          /* up to 24 registers of text, and the NUL */
#define PF_NAME_MAX  40
#define PF_ID_MAX    24

enum { PF_SENSOR, PF_BINARY, PF_ENUM, PF_BITS, PF_ASCII, PF_CALC, PF_CALCBIN };
enum { T_U16, T_S16, T_U32, T_S32, T_U32LE, T_S32LE, T_U8H, T_U8L, T_S8H, T_S8L, T_BIT, T_COIL, T_ASCII, T_ASCIILO };
enum { OP_EQ, OP_NE, OP_GT, OP_LT };
enum { F_SUM, F_MIN, F_MAX, F_AVG, F_DELTA, F_ARGMIN, F_ARGMAX, F_COUNT, F_SUB, F_MUL, F_POS, F_NEG, F_ABS,
       F_EQ, F_NE, F_GT, F_LT, F_ANYBITS, F_NOBITS };        /* F_EQ..F_LT in OP_ order */
enum { RJ_ZERO, RJ_ZEROS, RJ_CMP };                        /* the block all 0; listed fields all 0; a comparison */
enum { PFF_DIAG = 1, PFF_WITHIN = 2 };

typedef struct {
    uint16_t id;                 /* pool */
    uint8_t  fn;                 /* Modbus function: 1 coils, 2 discrete inputs, 3 holding, 4 input registers */
    bool     info;               /* not needed for "online" */
    uint16_t addr, count;        /* registers, or bits for 1 and 2 */
    uint32_t period_ms;
} pf_block_t;

typedef struct {
    uint16_t id, name, unit, dc, sc;   /* pool; 0: none */
    uint8_t  kind, type, block, bit;   /* block 0xFF for calc; bit: T_BIT's bit, or T_ASCII*'s registers */
    uint16_t addr;
    int8_t   prec;                     /* decimals shown; -1: from the scale */
    uint8_t  flags, op, fn;            /* op: a binary's comparison; fn: a calc's function */
    uint8_t  nnodata, str;             /* str: PF_ASCII's slot in pf_state_t */
    uint16_t vname, nvname;            /* PF_ENUM, PF_BITS: their value names in vname[] */
    uint16_t arg, narg;                /* PF_CALC*: field indexes in arg[] */
    int32_t  offset;
    uint32_t mask, nodata[4];          /* mask: an enum's, or anybits/nobits' */
    float    scale, k, lo, hi;         /* k: comparison constant; lo, hi: within */
} pf_field_t;

typedef struct { uint8_t block, kind, op, nfield; uint16_t field[4]; float k; } pf_reject_t;
typedef struct { uint32_t value; uint16_t name; } pf_vname_t;

typedef struct {
    uint16_t id, name, maker;          /* pool */
    uint32_t baud;
    char     frame[4];                 /* "8N1" */
    uint8_t  stale;
    uint16_t timeout_ms, turn_ms;
    uint8_t  nblock, nreject, nstr;
    uint16_t nfield, nvname, narg, npool;
    pf_block_t  block[PF_BLOCKS];
    pf_field_t  field[PF_FIELDS];
    pf_reject_t reject[PF_REJECTS];
    pf_vname_t  vname[PF_VNAMES];
    uint16_t    arg[PF_ARGS];
    char        pool[PF_POOL];
} profile_t;

static inline const char *pf_str(const profile_t *p, uint16_t off) { return p->pool + off; }

/* The text into *p. False, with "line N: why" in err (or just "why"), if it will not do. */
bool profile_parse(profile_t *p, const char *text, size_t n, char *err, size_t errlen);
int  profile_field(const profile_t *p, const char *id);    /* index, or -1 */
int  profile_block(const profile_t *p, const char *id);    /* index, or -1 */

#endif
