/*
 * system_clock.c — Clock tree bring-up for STM32H750 @ 400 MHz from 25 MHz HSE.
 *
 * PLL1 math (RM0433):
 *   HSE            = 25 MHz
 *   /PLLM (5)      = 5 MHz   PLL input  (VCIRANGE 4-8 MHz)
 *   xPLLN (160)    = 800 MHz VCO        (wide range 192-836 MHz)
 *   /PLLP (2)      = 400 MHz SYSCLK
 *   /PLLQ (8)      = 100 MHz (kernel clock for peripherals)
 *
 * Bus clocks: HCLK = SYSCLK/2 = 200 MHz (AHB bus-matrix max), APBx = 100 MHz.
 * At VOS1, 200 MHz AXI needs 2 flash wait states.
 *
 * TODO(dump): the power-supply topology (LDO vs SMPS) is board-specific. We
 * assume a direct LDO supply here. If VOSRDY never sets (boot hangs in
 * SystemClock_Config), this is the first thing to revisit — the N540 may use
 * the SMPS. To push to 480 MHz later: VOS0 + ODEN and re-check the PLL/VCO.
 */
#include "stm32h7xx_hal.h"

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* 1. Supply configuration. TODO(dump): verify LDO vs SMPS for this board. */
    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

    /* 2. Voltage scaling 1 supports up to 400 MHz CPU. */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) { }

    /* 3. HSE + PLL1. */
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM            = 5;
    osc.PLL.PLLN            = 160;
    osc.PLL.PLLP            = 2;
    osc.PLL.PLLQ            = 8;
    osc.PLL.PLLR            = 2;
    osc.PLL.PLLRGE          = RCC_PLL1VCIRANGE_2;   /* 4-8 MHz input */
    osc.PLL.PLLVCOSEL       = RCC_PLL1VCOWIDE;      /* 192-836 MHz VCO */
    osc.PLL.PLLFRACN        = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        /* Clocking failed — spin so a debugger catches it clearly. */
        while (1) { }
    }

    /* 4. Bus clocks. */
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2 |
                    RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_D3PCLK1;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider  = RCC_SYSCLK_DIV1;   /* 400 MHz */
    clk.AHBCLKDivider  = RCC_HCLK_DIV2;     /* 200 MHz */
    clk.APB3CLKDivider = RCC_APB3_DIV2;     /* 100 MHz */
    clk.APB1CLKDivider = RCC_APB1_DIV2;     /* 100 MHz */
    clk.APB2CLKDivider = RCC_APB2_DIV2;     /* 100 MHz */
    clk.APB4CLKDivider = RCC_APB4_DIV2;     /* 100 MHz */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) {
        while (1) { }
    }
}
