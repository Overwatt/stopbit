/*
 * stm32h7xx_it.c — Cortex-M core exception handlers.
 *
 * A fault is written to the backup registers (slots.h), which survive a reset, and the
 * chip restarts: so the next start can say where it happened. Under a debugger the
 * handler spins instead, with the stack intact. SysTick drives HAL's millisecond tick.
 */
#include "stm32h7xx_hal.h"
#include "stm32h7xx_it.h"
#include "slots.h"

/* frame: what the core pushed: r0-r3, r12, lr, pc, xpsr. Called only from the assembly
 * below, which a compiler looking at the whole program (-flto) cannot see: hence "used". */
void fault_record(const uint32_t *frame, uint32_t which);
__attribute__((used)) void fault_record(const uint32_t *frame, uint32_t which)
{
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) for (;;) { }
    bkp_set(BKP_FAULT, FAULT_MAGIC | which);
    bkp_set(BKP_FAULT + 1, frame[6]);
    bkp_set(BKP_FAULT + 2, frame[5]);
    bkp_set(BKP_FAULT + 3, SCB->CFSR);
    bkp_set(BKP_FAULT + 4, (SCB->CFSR & SCB_CFSR_BFARVALID_Msk) ? SCB->BFAR :
                           (SCB->CFSR & SCB_CFSR_MMARVALID_Msk) ? SCB->MMFAR : 0);
    NVIC_SystemReset();
}

#define FAULT(name, n) \
    __attribute__((naked)) void name(void) \
    { __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n movs r1, #" #n "\n b fault_record"); }
FAULT(HardFault_Handler, 3)
FAULT(MemManage_Handler, 4)
FAULT(BusFault_Handler, 5)
FAULT(UsageFault_Handler, 6)

void NMI_Handler(void)        { while (1) { } }
void SVC_Handler(void)        { }
void DebugMon_Handler(void)   { }
void PendSV_Handler(void)     { }

void SysTick_Handler(void)
{
    HAL_IncTick();
}
