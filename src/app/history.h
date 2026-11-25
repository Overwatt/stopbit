/* history.h — Ten minutes of traffic per port, in 5 s steps. Lost on restart. */
#ifndef HISTORY_H
#define HISTORY_H
#include <stdint.h>
#include <stddef.h>

#define HIST_STEP_MS 5000u
#define HIST_N       120u

typedef struct { uint32_t rx, tx, err; } hist_count_t;      /* running totals */

void history_init(uint32_t now);
/* Call often with the ports' totals; takes a sample when one is due. */
void history_poll(uint32_t now, const hist_count_t *total, int ports);
uint32_t history_errors(int port);                          /* in the kept window */
/* {"step_s":5,"rx":[...],"tx":[...],"err":[...]}, oldest first. 0 if it does not fit. */
size_t history_json(int port, char *out, size_t max);
#endif
