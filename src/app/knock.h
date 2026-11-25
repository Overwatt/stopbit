/*
 * knock.h — Who a port's access list turned away lately. In memory only: writing it
 * to flash would let anyone on the network wear it out. Pure C; tests/test_knock.c.
 */
#ifndef KNOCK_H
#define KNOCK_H
#include <stdint.h>
#include <stdbool.h>

#define KNOCK_N       8
#define KNOCK_RECENT  600000u       /* ms */
#define KNOCK_TRIES   3u            /* this many is a client that means it, not a scan */

typedef struct {
    uint32_t ip, count;             /* count 0: unused */
    uint32_t tries;                 /* since it last kept away for KNOCK_RECENT */
    uint32_t at, seq;               /* order of arrival: two can share a millisecond */
} knock_t;
typedef struct { knock_t k[KNOCK_N]; uint32_t seq; } knocks_t;

/* A ninth address takes the place of the one heard from longest ago. */
void knock_add(knocks_t *s, uint32_t ip, uint32_t now);
/* i-th most recent, or NULL. */
const knock_t *knock_get(const knocks_t *s, int i);
/* Someone who keeps trying, still at it lately. */
bool knock_persistent(const knocks_t *s, uint32_t now);
void knock_forget(knocks_t *s, int slot);
/* Call now and then, so that old times do not wrap round to "just now". */
void knock_clamp(knocks_t *s, uint32_t now);
#endif
