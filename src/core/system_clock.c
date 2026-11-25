/*
 * system_clock.c — 400 MHz from the 25 MHz HSE, or from HSI if HSE won't start.
 *
 *   HSE: 25 MHz / 5  = 5 MHz,  x160 = 800 MHz VCO, /2 = 400 MHz
 *   HSI: 64 MHz / 16 = 4 MHz,  x200 = 800 MHz VCO, /2 = 400 MHz
 *
 * HCLK = 200 MHz, APBx = 100 MHz, 2 flash wait states at VOS1. The STM32H750
 * has no SMPS, so LDO is the only supply mode.
 */
#include "stm32h7xx_hal.h"
#include "system.h"

static HAL_StatusTypeDef pll(bool hse)
{
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = hse ? RCC_OSCILLATORTYPE_HSE : RCC_OSCILLATORTYPE_HSI;
    osc.HSEState = hse ? RCC_HSE_ON : RCC_HSE_OFF;
    osc.HSIState = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = hse ? RCC_PLLSOURCE_HSE : RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = hse ? 5 : 16;
    osc.PLL.PLLN = hse ? 160 : 200;
    osc.PLL.PLLP = 2;
    osc.PLL.PLLQ = 8;
    osc.PLL.PLLR = 2;
    osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    return HAL_RCC_OscConfig(&osc);
}

bool SystemClock_Config(void)
{
    RCC_ClkInitTypeDef clk = {0};
    bool hse = true;

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    uint32_t t0 = HAL_GetTick();
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
        if (HAL_GetTick() - t0 > 100) NVIC_SystemReset();

    if (pll(true) != HAL_OK) {
        hse = false;
        if (pll(false) != HAL_OK) NVIC_SystemReset();
    }

    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
                    RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D1PCLK1 | RCC_CLOCKTYPE_D3PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider = RCC_HCLK_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV2;
    clk.APB1CLKDivider = RCC_APB1_DIV2;
    clk.APB2CLKDivider = RCC_APB2_DIV2;
    clk.APB4CLKDivider = RCC_APB4_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) NVIC_SystemReset();
    return hse;
}
