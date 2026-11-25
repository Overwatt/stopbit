/* Just enough of the HAL to build slots.c, fwlib.c and http.c on the host. */
#ifndef STUB_HAL_H
#define STUB_HAL_H
#include <stdint.h>
extern uint32_t t_now;
static inline uint32_t HAL_GetTick(void) { return t_now; }
typedef struct { volatile uint32_t APB4ENR, RSR, CR1; } t_regs_t;
extern t_regs_t t_regs;
typedef struct { uint32_t BKP0R, rest[31]; } t_rtc_t;
extern t_rtc_t t_rtc;
extern uint32_t SystemCoreClock;
#define RCC (&t_regs)
#define PWR (&t_regs)
#define RTC (&t_rtc)
#define RCC_APB4ENR_RTCAPBEN 1u
#define PWR_CR1_DBP 1u
static inline void __DSB(void) { }
void NVIC_SystemReset(void);
#endif
