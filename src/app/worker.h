/*
 * worker.h — Long jobs beside the main loop, each on its own stack.
 *
 * Cooperative: a job calls worker_breathe() often, which goes back to the main loop
 * once its slice is used up, or worker_yield() to wait for something. Either returns
 * when the loop comes round again. So nothing else runs half way through a step, and
 * no locks are needed: anything a job shares must simply be left consistent wherever
 * it breathes or yields.
 */
#ifndef WORKER_H
#define WORKER_H
#include <stdint.h>
#include <stdbool.h>

#ifndef WORKER_SLICE_US
#define WORKER_SLICE_US 1000u
#endif

enum { WORKER_TLS, WORKER_RENEW, WORKERS };

typedef struct { uint32_t slices, longest_us, stack_used, stack_size; } worker_stats_t;   /* used: the most yet */

/* Starts `fn` on worker `w`. False if that one is already busy. */
bool worker_start(int w, void (*fn)(void *), void *arg);
bool worker_busy(int w);
/* One slice of worker `w`. Main loop only. */
void worker_poll(int w);
/* From within a job: back to the main loop if the slice is used up. Does nothing elsewhere. */
void worker_breathe(void);
/* From within a job: back to the main loop now. */
void worker_yield(void);
bool worker_inside(int w);           /* the code running is worker w's */
void worker_stats(int w, worker_stats_t *out);
#endif
