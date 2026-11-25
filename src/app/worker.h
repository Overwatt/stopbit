/*
 * worker.h — Runs one long calculation beside the main loop, on its own stack.
 *
 * It is cooperative: the calculation calls worker_breathe() often, which goes back to
 * the main loop once its slice is used up and returns when the loop comes round. So
 * nothing else ever runs half way through a step, and no locks are needed.
 */
#ifndef WORKER_H
#define WORKER_H
#include <stdint.h>
#include <stdbool.h>

#ifndef WORKER_SLICE_US
#define WORKER_SLICE_US 1000u
#endif

typedef struct { uint32_t slices, longest_us, stack_used, stack_size; } worker_stats_t;

/* Starts `fn`. False if something is already running. */
bool worker_start(void (*fn)(void *), void *arg);
bool worker_busy(void);
/* One slice. Call from the main loop only. */
void worker_poll(void);
/* From within `fn`; does nothing elsewhere. */
void worker_breathe(void);
void worker_stats(worker_stats_t *out);
#endif
