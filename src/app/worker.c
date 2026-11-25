#include "worker.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#ifndef AXI_BSS
#define AXI_BSS __attribute__((section(".axi_bss")))
#endif
#ifndef WORKER_STACK
#define WORKER_STACK 12288u             /* interrupts use it too while it runs */
#endif
#define FILL 0x5AFE57ACu

static AXI_BSS union { uint64_t align; uint32_t w[WORKER_STACK / 4]; uint8_t b[WORKER_STACK]; } s_stack;
static void *s_main_sp, *s_sp;
static void (*s_fn)(void *);
static void *s_arg;
static bool s_busy, s_inside;
static uint32_t s_from, s_slices, s_longest;

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
__asm__(
    ".text\n.align 2\n.global _worker_swap\n_worker_swap:\n"
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
#endif

static void entry(void)
{
    s_fn(s_arg);
    s_busy = false;
    worker_swap(&s_sp, s_main_sp);
    for (;;) { }                        /* never resumed */
}

bool worker_start(void (*fn)(void *), void *arg)
{
    if (s_busy) return false;
    for (uint32_t i = 0; i < WORKER_STACK / 4; i++) s_stack.w[i] = FILL;
    reg_t *sp = (reg_t *)(void *)(s_stack.b + WORKER_STACK) - 2 - FRAME;   /* 8-byte aligned on entry */
#if defined(__aarch64__)
    sp = (reg_t *)((uintptr_t)sp & ~(uintptr_t)15);
#endif
    memset(sp, 0, FRAME * sizeof *sp);
    sp[FRAME_LR] = (reg_t)(uintptr_t)entry;
    s_sp = sp;
    s_fn = fn;
    s_arg = arg;
    s_busy = true;
    return true;
}

bool worker_busy(void) { return s_busy; }

static uint32_t us_since(uint32_t t) { return (DWT->CYCCNT - t) / (SystemCoreClock / 1000000u); }

void worker_poll(void)
{
    if (!s_busy) return;
    s_from = DWT->CYCCNT;
    s_inside = true;
    worker_swap(&s_main_sp, s_sp);
    s_inside = false;
    uint32_t us = us_since(s_from);
    if (us > s_longest) s_longest = us;
    s_slices++;
    /* Ran off the end of its stack: what lies below it is no longer to be trusted. */
    if (s_stack.w[0] != FILL) NVIC_SystemReset();
}

void worker_breathe(void)
{
    if (!s_inside || us_since(s_from) < WORKER_SLICE_US) return;
    worker_swap(&s_sp, s_main_sp);
}

void worker_stats(worker_stats_t *o)
{
    uint32_t n = 0;
    while (n < WORKER_STACK / 4 && s_stack.w[n] == FILL) n++;
    o->slices = s_slices;
    o->longest_us = s_longest;
    o->stack_used = s_slices ? WORKER_STACK - 4 * n : 0;
    o->stack_size = WORKER_STACK;
}
