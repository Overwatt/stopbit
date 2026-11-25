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
FetchContent_Declare(lwip
    GIT_REPOSITORY https://github.com/lwip-tcpip/lwip.git
    GIT_TAG        STABLE-2_2_1_RELEASE GIT_SHALLOW TRUE)

foreach(dep cmsis_core cmsis_device_h7 stm32h7xx_hal_driver lwip)
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
# lwIP (raw API only).
# ---------------------------------------------------------------------------
file(GLOB LWIP_SRC
    "${lwip_SOURCE_DIR}/src/core/*.c"
    "${lwip_SOURCE_DIR}/src/core/ipv4/*.c"
    "${lwip_SOURCE_DIR}/src/apps/mdns/*.c")
list(APPEND LWIP_SRC "${lwip_SOURCE_DIR}/src/netif/ethernet.c")
set(LWIP_MQTT_SRC "${lwip_SOURCE_DIR}/src/apps/mqtt/mqtt.c")

# ---------------------------------------------------------------------------
# Update key: generated once into secrets/ (gitignored), compiled into images.
# ---------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_key.py
            ${CMAKE_SOURCE_DIR}/secrets/n540.key ${CMAKE_BINARY_DIR}/gen/secret_key.h
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE FW_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)

# ---------------------------------------------------------------------------
# n540_image(<name> <linker script> <sources...>) -> <name>.elf / <name>.bin
# ---------------------------------------------------------------------------
function(n540_image name ldscript)
    add_executable(${name}.elf ${ARGN} ${HAL_SRC} ${SYSTEM_C} ${STARTUP_S})
    target_include_directories(${name}.elf PRIVATE
        ${CMAKE_SOURCE_DIR}/src/board
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_SOURCE_DIR}/src/app
        ${CMAKE_SOURCE_DIR}/src/http
        ${CMAKE_SOURCE_DIR}/src/net
        ${CMAKE_BINARY_DIR}/gen)
    target_include_directories(${name}.elf SYSTEM PRIVATE
        ${lwip_SOURCE_DIR}/src/include
        ${CMSIS_CORE_INC} ${CMSIS_DEV_INC} ${HAL_INC} ${HAL_INC}/Legacy)
    target_compile_definitions(${name}.elf PRIVATE
        STM32H750xx USE_HAL_DRIVER FW_VERSION="${FW_VERSION}")
    target_compile_options(${name}.elf PRIVATE -Wall -Wextra -Os -g3)
    target_link_options(${name}.elf PRIVATE
        -L${CMAKE_SOURCE_DIR}/linker -T${ldscript}
        -Wl,-Map=${name}.map,--cref -Wl,--print-memory-usage
        -Wl,--no-warn-rwx-segments)
    set_target_properties(${name}.elf PROPERTIES LINK_DEPENDS
        "${CMAKE_SOURCE_DIR}/linker/${ldscript};${CMAKE_SOURCE_DIR}/linker/sections.ld")
    add_custom_command(TARGET ${name}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary -R .dma_d2 -R .d2_bss $<TARGET_FILE:${name}.elf> ${name}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}.elf>)
endfunction()

file(GLOB CORE_SRC   "${CMAKE_SOURCE_DIR}/src/core/stm32h7xx_it.c" "${CMAKE_SOURCE_DIR}/src/core/system_clock.c")
file(GLOB COMMON_SRC "${CMAKE_SOURCE_DIR}/src/common/*.c")
file(GLOB NET_SRC    "${CMAKE_SOURCE_DIR}/src/net/*.c")
file(GLOB HTTP_SRC   "${CMAKE_SOURCE_DIR}/src/http/*.c")
file(GLOB LOADER_SRC "${CMAKE_SOURCE_DIR}/src/loader/*.c")
file(GLOB APP_SRC    "${CMAKE_SOURCE_DIR}/src/app/*.c")

# The same loader, linked for internal flash and for SRAM (SWD testing).
foreach(where flash ram)
    n540_image(loader_${where} loader_${where}.ld
        ${LOADER_SRC} ${CORE_SRC} ${COMMON_SRC} ${NET_SRC} ${HTTP_SRC} ${LWIP_SRC})
endforeach()

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gen/web_page.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed.py
            ${CMAKE_SOURCE_DIR}/web/index.html ${CMAKE_BINARY_DIR}/gen/web_page.h
    DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html ${CMAKE_SOURCE_DIR}/tools/embed.py)
n540_image(app app_ram.ld ${LWIP_MQTT_SRC}
    ${CMAKE_BINARY_DIR}/gen/web_page.h
    ${APP_SRC} ${CORE_SRC} ${COMMON_SRC} ${NET_SRC} ${HTTP_SRC} ${LWIP_SRC})
# -DAPP_TEST=TEST_HANG or TEST_NONET builds deliberately broken apps.
set(APP_TEST "" CACHE STRING "broken-app variant for rollback tests")
target_compile_definitions(app.elf PRIVATE APP_IMAGE ${APP_TEST})
