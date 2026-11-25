/* bringup.c - minimal pin-identification firmware for the USR-N540.
 * Default HSI clock (no PLL/PWR config = safe first boot). Configures the 14
 * candidate output pins and blinks each count-encoded: pin #N blinks N times,
 * with a gap between. Watch the board and report blink counts -> pin mapping.
 * Restore stock anytime: flash dumps/internal_bootloader.bin to 0x08000000. */
#include "stm32h7xx.h"

static void delay(volatile uint32_t n){ while(n){ n--; __asm volatile("nop"); } }

static GPIO_TypeDef * const PORT[] = {
    GPIOA,GPIOA,GPIOA,GPIOA,GPIOA, GPIOC,GPIOC,
    GPIOD,GPIOD,GPIOD,GPIOD, GPIOE,GPIOE,GPIOE };
static const uint8_t PIN[] = {
    3,4,5,12,15, 8,10, 4,12,13,15, 4,12,14 };
#define NP (sizeof(PIN)/sizeof(PIN[0]))
#define ON   6000000u
#define GAP 40000000u

int main(void){
    RCC->AHB4ENR |= 0x1Fu;              /* GPIOA..E clocks */
    for(unsigned i=0;i<NP;i++){
        GPIO_TypeDef *p=PORT[i]; uint8_t pin=PIN[i];
        p->MODER = (p->MODER & ~(3u<<(2*pin))) | (1u<<(2*pin)); /* output */
        p->BSRR  = (1u<<(pin+16));       /* low */
    }
    for(;;){
        for(unsigned i=0;i<NP;i++){
            GPIO_TypeDef *p=PORT[i]; uint8_t pin=PIN[i];
            for(unsigned b=0;b<=i;b++){   /* i=0 -> 1 blink ... i=13 -> 14 */
                p->BSRR=(1u<<pin);     delay(ON);
                p->BSRR=(1u<<(pin+16)); delay(ON);
            }
            delay(GAP);
        }
    }
}
