/*
 * stm32h7xx_hal_conf.h — HAL configuration for this project.
 * Trimmed to the modules we actually use; add more HAL_*_MODULE_ENABLED
 * lines (and the matching source files in CMakeLists) as the project grows.
 */
#ifndef STM32H7xx_HAL_CONF_H
#define STM32H7xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Enabled HAL modules ------------------------------------------------- */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED
/* Ethernet / others added at their milestones:
 * #define HAL_ETH_MODULE_ENABLED
 */

/* ---- Oscillator values --------------------------------------------------- */
#if !defined(HSE_VALUE)
#define HSE_VALUE  25000000UL   /* 25 MHz crystal on this board */
#endif
#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT  100UL
#endif
#if !defined(CSI_VALUE)
#define CSI_VALUE  4000000UL
#endif
#if !defined(HSI_VALUE)
#define HSI_VALUE  64000000UL
#endif
#if !defined(LSI_VALUE)
#define LSI_VALUE  32000UL
#endif
#if !defined(LSE_VALUE)
#define LSE_VALUE  32768UL
#endif
#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT  5000UL
#endif
#if !defined(EXTERNAL_CLOCK_VALUE)
#define EXTERNAL_CLOCK_VALUE  12288000UL
#endif

/* ---- System configuration ------------------------------------------------ */
#define VDD_VALUE                     3300UL
#define TICK_INT_PRIORITY             15UL
#define USE_RTOS                      0U
#define USE_SPI_CRC                   0U
#define PREFETCH_ENABLE               1U
#define ART_ACCLERATOR_ENABLE         0U   /* CM7: no ART (that's an F7 feature) */

/* ART/caches: enable the M7 data/instruction caches. */
#define INSTRUCTION_CACHE_ENABLE      1U
#define DATA_CACHE_ENABLE             1U

#define USE_HAL_UART_REGISTER_CALLBACKS   0U

/* Assert — off by default; define USE_FULL_ASSERT to turn on. */
/* #define USE_FULL_ASSERT   1U */

/* ---- Module includes ----------------------------------------------------- */
#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32h7xx_hal_rcc.h"
#endif
#ifdef HAL_FLASH_MODULE_ENABLED
#include "stm32h7xx_hal_flash.h"
#endif
#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32h7xx_hal_gpio.h"
#endif
#ifdef HAL_DMA_MODULE_ENABLED
#include "stm32h7xx_hal_dma.h"
#endif
#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32h7xx_hal_cortex.h"
#endif
#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32h7xx_hal_pwr.h"
#endif
#ifdef HAL_UART_MODULE_ENABLED
#include "stm32h7xx_hal_uart.h"
#endif
#ifdef HAL_EXTI_MODULE_ENABLED
#include "stm32h7xx_hal_exti.h"
#endif

/* ---- assert_param -------------------------------------------------------- */
#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
void assert_failed(uint8_t *file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif

#ifdef __cplusplus
}
#endif
#endif /* STM32H7xx_HAL_CONF_H */
