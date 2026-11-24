cmake_minimum_required(VERSION 3.20)

project(lino-n540 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Vendored dependencies (pinned). Fetched into build/_deps on first configure.
# We only POPULATE them (download); we drive the build ourselves rather than
# use the vendors' own CMakeLists.
# ---------------------------------------------------------------------------
include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)   # allow single-arg FetchContent_Populate
endif()

FetchContent_Declare(cmsis_core
    GIT_REPOSITORY https://github.com/STMicroelectronics/cmsis_core.git
    GIT_TAG        v5.9.0        GIT_SHALLOW TRUE)
FetchContent_Declare(cmsis_device_h7
    GIT_REPOSITORY https://github.com/STMicroelectronics/cmsis_device_h7.git
    GIT_TAG        v1.10.8       GIT_SHALLOW TRUE)
FetchContent_Declare(stm32h7xx_hal_driver
    GIT_REPOSITORY https://github.com/STMicroelectronics/stm32h7xx_hal_driver.git
    GIT_TAG        v1.11.7       GIT_SHALLOW TRUE)

foreach(dep cmsis_core cmsis_device_h7 stm32h7xx_hal_driver)
    FetchContent_GetProperties(${dep})
    if(NOT ${dep}_POPULATED)
        FetchContent_Populate(${dep})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Locate headers/startup robustly (ST repo layouts vary between versions).
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _core_cm7 "${cmsis_core_SOURCE_DIR}/*core_cm7.h")
list(GET _core_cm7 0 _core_cm7_first)
get_filename_component(CMSIS_CORE_INC "${_core_cm7_first}" DIRECTORY)

set(CMSIS_DEV_INC "${cmsis_device_h7_SOURCE_DIR}/Include")
# Pick the GCC startup variant (there are also arm/ and iar/ assembler syntaxes).
file(GLOB_RECURSE _startup "${cmsis_device_h7_SOURCE_DIR}/*startup_stm32h750xx.s")
set(STARTUP_S "")
foreach(_s ${_startup})
    if(_s MATCHES "/gcc/")
        set(STARTUP_S "${_s}")
    endif()
endforeach()
if(NOT STARTUP_S)
    message(FATAL_ERROR "GCC startup_stm32h750xx.s not found")
endif()
file(GLOB_RECURSE _sysfile "${cmsis_device_h7_SOURCE_DIR}/*system_stm32h7xx.c")
list(GET _sysfile 0 SYSTEM_C)

set(HAL_INC "${stm32h7xx_hal_driver_SOURCE_DIR}/Inc")
file(GLOB HAL_SRC "${stm32h7xx_hal_driver_SOURCE_DIR}/Src/*.c")
list(FILTER HAL_SRC EXCLUDE REGEX ".*_template\\.c$")

message(STATUS "CMSIS core inc : ${CMSIS_CORE_INC}")
message(STATUS "Startup        : ${STARTUP_S}")
message(STATUS "System file    : ${SYSTEM_C}")

# ---------------------------------------------------------------------------
# Firmware target.
# ---------------------------------------------------------------------------
file(GLOB APP_SRC
    "${CMAKE_SOURCE_DIR}/src/core/*.c"
    "${CMAKE_SOURCE_DIR}/src/drivers/*.c"
    "${CMAKE_SOURCE_DIR}/src/app/*.c")

add_executable(${PROJECT_NAME}.elf
    ${APP_SRC}
    ${HAL_SRC}
    ${SYSTEM_C}
    ${STARTUP_S})

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/src/board
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/drivers
    ${CMSIS_CORE_INC}
    ${CMSIS_DEV_INC}
    ${HAL_INC}
    ${HAL_INC}/Legacy)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    STM32H750xx
    USE_HAL_DRIVER)

target_compile_options(${PROJECT_NAME}.elf PRIVATE
    -Wall -Wextra -Og -g3)

set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/linker/STM32H750VBTx_FLASH.ld)
target_link_options(${PROJECT_NAME}.elf PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=${PROJECT_NAME}.map,--cref
    -Wl,--print-memory-usage
    -Wl,--no-warn-rwx-segments)
set_target_properties(${PROJECT_NAME}.elf PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

# ---------------------------------------------------------------------------
# .bin / .hex + size report.
# ---------------------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>
    COMMENT "Generating .bin/.hex and size")

# ---------------------------------------------------------------------------
# Convenience: `ninja flash` via OpenOCD + ST-Link. Never runs automatically.
# DO NOT flash until the stock firmware is backed up (see docs/ROADMAP.md M0).
# ---------------------------------------------------------------------------
find_program(OPENOCD openocd)
if(OPENOCD)
    add_custom_target(flash
        COMMAND ${OPENOCD} -f interface/stlink.cfg -f target/stm32h7x.cfg
                -c "program $<TARGET_FILE:${PROJECT_NAME}.elf> verify reset exit"
        DEPENDS ${PROJECT_NAME}.elf
        USES_TERMINAL
        COMMENT "Flashing via OpenOCD + ST-Link")
endif()
