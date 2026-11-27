/*
 * pfwrite.h — A command for a control (profile.h) as what is written: the register or two at
 * the control's address, the bits it owns there, and their new value. No I/O: device.c reads
 * the registers first and writes only if this changes them.
 */
#ifndef PFWRITE_H
#define PFWRITE_H
#include "profile.h"

typedef struct {
    uint16_t addr;
    uint8_t  regs;                 /* 1, or 2 for a 32-bit number */
    uint16_t mask[2], value[2];    /* per register: the bits written, and what they become */
} pf_write_t;

/* The payload for control field i: a number in decimal ("80", "12.5"), a choice's name, ON or
 * OFF, or HH:MM. False, with the reason in why, if it will not do. */
bool pf_command(const profile_t *p, int i, const char *payload, pf_write_t *w, char *why, size_t max);
/* `have`, the registers as read, with the write applied, into out. True if that changes them. */
bool pf_merge(const pf_write_t *w, const uint16_t have[2], uint16_t out[2]);
#endif
