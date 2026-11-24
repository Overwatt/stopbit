/*
 * stm32h7xx_it.c — Cortex-M core exception handlers.
 *
 * The fault handlers deliberately spin so a debugger halts right at the fault
 * with the stack intact. SysTick drives HAL's millisecond tick (HAL_Delay).
 */
#include "stm32h7xx_hal.h"
#include "stm32h7xx_it.h"

void NMI_Handler(void)        { while (1) { } }
void HardFault_Handler(void)  { while (1) { } }
void MemManage_Handler(void)  { while (1) { } }
void BusFault_Handler(void)   { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }

void SVC_Handler(void)        { }
void DebugMon_Handler(void)   { }
void PendSV_Handler(void)     { }

void SysTick_Handler(void)
{
    HAL_IncTick();
}
