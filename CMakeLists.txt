cmake_minimum_required(VERSION 3.20)

project(stopbit LANGUAGES C ASM)

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

# The release archive, not the repository: it has the generated sources. Supported to March 2029.
FetchContent_Declare(mbedtls
    URL      https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.2/mbedtls-4.1.2.tar.bz2
    URL_HASH SHA256=392215e5e4846df04e2f4c4516079303bd73edde837d8f3e68fb4d4d7ddf0126
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/mbedtls4-src)

foreach(dep cmsis_core cmsis_device_h7 stm32h7xx_hal_driver lwip mbedtls)
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
set(LWIP_MQTT_SRC "${lwip_SOURCE_DIR}/src/apps/mqtt/mqtt.c" "${lwip_SOURCE_DIR}/src/apps/sntp/sntp.c")

# ---------------------------------------------------------------------------
# mbedTLS (app only). The calls a handshake spends its time in are renamed where they
# are made, to ones in tls.c that let the main loop in first; see worker.h.
# ---------------------------------------------------------------------------
set(PSA_DIR "${mbedtls_SOURCE_DIR}/tf-psa-crypto")
file(GLOB MBEDTLS_SRC "${mbedtls_SOURCE_DIR}/library/*.c" "${PSA_DIR}/core/*.c" "${PSA_DIR}/drivers/builtin/src/*.c"
    "${PSA_DIR}/extras/*.c" "${PSA_DIR}/platform/*.c" "${PSA_DIR}/utilities/*.c")
# Its private headers, which our own sources do not see.
set(MBEDTLS_PRIVATE "${mbedtls_SOURCE_DIR}/library" "${PSA_DIR}/core" "${PSA_DIR}/dispatch" "${PSA_DIR}/drivers/builtin/src"
    "${PSA_DIR}/extras" "${PSA_DIR}/platform" "${PSA_DIR}/utilities")
set(MBEDTLS_PUBLIC "${mbedtls_SOURCE_DIR}/include" "${PSA_DIR}/include" "${PSA_DIR}/drivers/builtin/include")
set_source_files_properties(${MBEDTLS_SRC} PROPERTIES COMPILE_OPTIONS "-w" INCLUDE_DIRECTORIES "${MBEDTLS_PRIVATE}")
set(BN "${PSA_DIR}/drivers/builtin/src")
set_source_files_properties("${BN}/ecp.c" PROPERTIES
    COMPILE_OPTIONS "-w;-O2" COMPILE_DEFINITIONS mbedtls_mpi_mul_mpi=tls_mpi_mul_mpi)
set_source_files_properties("${BN}/bignum_core.c" PROPERTIES
    COMPILE_OPTIONS "-w;-O2" COMPILE_DEFINITIONS memcpy=tls_core_memcpy)
set_source_files_properties("${BN}/psa_crypto_hash.c" "${PSA_DIR}/extras/md.c" PROPERTIES
    COMPILE_DEFINITIONS mbedtls_sha256_update=tls_sha256_update)
# AES-GCM for records goes to gcm_hw.c.
set_source_files_properties("${BN}/psa_crypto_aead.c" PROPERTIES COMPILE_DEFINITIONS
    "mbedtls_gcm_setkey=tls_gcm_setkey;mbedtls_gcm_crypt_and_tag=tls_gcm_crypt_and_tag;mbedtls_gcm_auth_decrypt=tls_gcm_auth_decrypt;mbedtls_gcm_free=tls_gcm_free")
set_source_files_properties("${BN}/bignum.c" "${BN}/ecp_curves.c" "${BN}/aes.c" "${BN}/gcm.c" "${BN}/sha256.c" PROPERTIES COMPILE_OPTIONS "-w;-O2")

# ---------------------------------------------------------------------------
# Update key: generated once into secrets/ (gitignored), compiled into images.
# ---------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_key.py
            ${CMAKE_SOURCE_DIR}/secrets/n540.key ${CMAKE_BINARY_DIR}/gen/secret_key.h
    COMMAND_ERROR_IS_FATAL ANY)

# Version: the release tag (YEAR.WEEK.N, annotated) when HEAD is one, else the short commit;
# "-dirty" marks uncommitted changes. It must fit fw_id_t.version: 23 characters.
execute_process(
    COMMAND git describe --exact-match --dirty --match "[0-9]*.[0-9]*.[0-9]*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE FW_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT FW_VERSION)
    execute_process(
        COMMAND git describe --always --dirty --exclude "*"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE FW_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
string(LENGTH "${FW_VERSION}" FW_VERSION_LEN)
if(FW_VERSION_LEN EQUAL 0 OR FW_VERSION_LEN GREATER 23)
    message(FATAL_ERROR "Firmware version '${FW_VERSION}' must be 1 to 23 characters")
endif()
message(STATUS "Firmware version: ${FW_VERSION}")

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
        COMMAND ${CMAKE_OBJCOPY} -O binary -R .dma_d2 -R .d2_bss -R .itcm_bss -R .d3_bss $<TARGET_FILE:${name}.elf> ${name}.bin
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
file(GLOB PROFILE_TXT CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/profiles/*.txt")
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gen/profiles_gen.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_profiles.py
            ${CMAKE_BINARY_DIR}/gen/profiles_gen.h ${PROFILE_TXT}
    DEPENDS ${PROFILE_TXT} ${CMAKE_SOURCE_DIR}/tools/embed_profiles.py)
n540_image(app app_ram.ld ${LWIP_MQTT_SRC}
    ${CMAKE_BINARY_DIR}/gen/web_page.h ${CMAKE_BINARY_DIR}/gen/profiles_gen.h
    ${APP_SRC} ${CORE_SRC} ${COMMON_SRC} ${NET_SRC} ${HTTP_SRC} ${LWIP_SRC} ${MBEDTLS_SRC})
target_include_directories(app.elf SYSTEM PRIVATE ${MBEDTLS_PUBLIC})
target_compile_definitions(app.elf PRIVATE MBEDTLS_CONFIG_FILE="mbedtls_cfg.h" TF_PSA_CRYPTO_CONFIG_FILE="crypto_cfg.h")
# -DAPP_TEST=TEST_HANG or TEST_NONET builds deliberately broken apps.
set(APP_TEST "" CACHE STRING "broken-app variant for rollback tests")
target_compile_definitions(app.elf PRIVATE APP_IMAGE ${APP_TEST})
