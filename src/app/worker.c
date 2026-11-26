#include "worker.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#ifndef AXI_BSS
#define AXI_BSS __attribute__((section(".d3_bss")))     /* D3: AXI is for the TLS heap */
#endif
/* Interrupts use them too while a job runs. The simulator cannot say how much the
 * gateway needs; worker_stats() reports the most each has used since the start.
 * Measured on the gateway (2026-11-25): HTTPS 2.7 KB, renewal 3.2 KB. */
#ifndef WORKER_TLS_STACK
#define WORKER_TLS_STACK   6144u
#endif
#ifndef WORKER_RENEW_STACK
#define WORKER_RENEW_STACK 8192u
#endif
#define FILL 0x5AFE57ACu
static const uint32_t SIZE[WORKERS] = { WORKER_TLS_STACK, WORKER_RENEW_STACK };

/* Only the stacks go in AXI_BSS (D3), which is not zeroed at start-up; the rest must be. */
static AXI_BSS uint64_t s_stacks[(WORKER_TLS_STACK + WORKER_RENEW_STACK) / 8];

typedef struct {
    uint32_t *s;                        /* lowest word of its stack */
    uint32_t size;                      /* bytes */
    void *sp;
    void (*fn)(void *);
    void *arg;
    bool busy;
    uint32_t slices, longest, peak;     /* peak: most stack any job here has used */
} job_t;
static job_t s_w[WORKERS];
static void *s_main_sp;
static job_t *s_cur;                    /* the one running, or NULL in the main loop */
static uint32_t s_from;
/* Saves what a call must preserve, keeps this stack in *save, and carries on from
 * whatever was saved at `load`. */
void worker_swap(void **save, void *load);
#if defined(__arm__)
__asm__(
    ".syntax unified\n.thumb\n.text\n.align 2\n.global worker_swap\n.type worker_swap, %function\nworker_swap:\n"
    "  push {r4-r11, lr}\n"
    "  sub sp, #4\n"                    /* keep 8-byte alignment */
    "  vpush {s16-s31}\n"
    "  str sp, [r0]\n"
    "  mov sp, r1\n"
    "  vpop {s16-s31}\n"
    "  add sp, #4\n"
    "  pop {r4-r11, pc}\n"
    ".size worker_swap, .-worker_swap\n");
#define FRAME 26                        /* words */
#define FRAME_LR 25
typedef uint32_t reg_t;
#elif defined(__aarch64__)              /* so the host tests run the real thing */
#ifdef __APPLE__
#define SWAP "_worker_swap"             /* Mach-O names C symbols with an underscore */
#else
#define SWAP "worker_swap"
#endif
__asm__(
    ".text\n.align 2\n.global " SWAP "\n" SWAP ":\n"
    "  sub sp, sp, #176\n"
    "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n  stp x25, x26, [sp, #48]\n"
    "  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n"
    "  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
    "  mov x9, sp\n  str x9, [x0]\n  mov sp, x1\n"
    "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n  ldp x25, x26, [sp, #48]\n"
    "  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n"
    "  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
    "  add sp, sp, #176\n  ret\n");
#define FRAME 22
#define FRAME_LR 11
typedef uint64_t reg_t;
#elif defined(__x86_64__)               /* the simulator on Linux, as GitHub's machines run it */
#ifdef __APPLE__
#define SWAP "_worker_swap"
#else
#define SWAP "worker_swap"
#endif
__asm__(
    ".text\n.p2align 4\n.globl " SWAP "\n" SWAP ":\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n");
#define FRAME 7                         /* r15 .. rbp, then where to return to */
#define FRAME_LR 6
typedef uint64_t reg_t;
#endif

static void entry(void)
{
    job_t *j = s_cur;
    j->fn(j->arg);
    j->busy = false;
    worker_swap(&j->sp, s_main_sp);
    for (;;) { }                        /* never resumed */
}

static uint32_t used(const job_t *j)
{
    uint32_t n = 0;
    while (j->s && n < j->size / 4 && j->s[n] == FILL) n++;
    return j->s ? j->size - 4 * n : 0;
}

bool worker_start(int w, void (*fn)(void *), void *arg)
{
    job_t *j = &s_w[w];
    if (j->busy) return false;
    uint32_t u = used(j);
    if (u > j->peak) j->peak = u;
    uint32_t at = 0;
    for (int i = 0; i < w; i++) at += SIZE[i];
    j->s = (uint32_t *)(void *)((uint8_t *)s_stacks + at);
    j->size = SIZE[w];
    for (uint32_t i = 0; i < j->size / 4; i++) j->s[i] = FILL;
    reg_t *sp = (reg_t *)(void *)((uint8_t *)j->s + j->size) - 2 - FRAME;   /* 8-byte aligned on entry */
#if defined(__aarch64__) || defined(__x86_64__)
    sp = (reg_t *)((uintptr_t)sp & ~(uintptr_t)15);
#endif
    memset(sp, 0, FRAME * sizeof *sp);
    sp[FRAME_LR] = (reg_t)(uintptr_t)entry;
    j->sp = sp;
    j->fn = fn;
    j->arg = arg;
    j->busy = true;
    return true;
}

bool worker_busy(int w) { return s_w[w].busy; }
bool worker_inside(int w) { return s_cur == &s_w[w]; }

static uint32_t us_since(uint32_t t) { return (DWT->CYCCNT - t) / (SystemCoreClock / 1000000u); }

void worker_poll(int w)
{
    job_t *j = &s_w[w];
    if (!j->busy || s_cur) return;
    s_from = DWT->CYCCNT;
    s_cur = j;
    worker_swap(&s_main_sp, j->sp);
    s_cur = NULL;
    uint32_t us = us_since(s_from);
    if (us > j->longest) j->longest = us;
    j->slices++;
    /* Ran off the end of its stack: what lies below it is no longer to be trusted. */
    if (j->s[0] != FILL) NVIC_SystemReset();
}

void worker_yield(void)
{
    if (s_cur) worker_swap(&s_cur->sp, s_main_sp);
}

void worker_breathe(void)
{
    if (s_cur && us_since(s_from) >= WORKER_SLICE_US) worker_yield();
}

void worker_stats(int w, worker_stats_t *o)
{
    const job_t *j = &s_w[w];
    uint32_t u = used(j);
    o->slices = j->slices;
    o->longest_us = j->longest;
    o->stack_used = u > j->peak ? u : j->peak;
    o->stack_size = SIZE[w];
}
