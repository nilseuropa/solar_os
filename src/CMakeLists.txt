set(SOLAR_OS_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../version.txt")
file(READ "${SOLAR_OS_VERSION_FILE}" SOLAR_OS_VERSION)
string(STRIP "${SOLAR_OS_VERSION}" SOLAR_OS_VERSION)
if(NOT SOLAR_OS_VERSION)
    set(SOLAR_OS_VERSION "0.0.0")
endif()

get_filename_component(SOLAR_OS_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED SOLAR_OS_BOARD OR SOLAR_OS_BOARD STREQUAL "")
    if(DEFINED ENV{SOLAR_OS_BOARD} AND NOT "$ENV{SOLAR_OS_BOARD}" STREQUAL "")
        set(SOLAR_OS_BOARD "$ENV{SOLAR_OS_BOARD}")
    elseif(DEFINED ENV{PIOENV} AND NOT "$ENV{PIOENV}" STREQUAL "")
        set(SOLAR_OS_BOARD "$ENV{PIOENV}")
    else()
        set(SOLAR_OS_BOARD "waveshare_esp32_s3_rlcd_4_2")
    endif()
endif()

set(SOLAR_OS_BOARD_CONFIG "${SOLAR_OS_PROJECT_DIR}/boards/${SOLAR_OS_BOARD}.cmake")
if(NOT EXISTS "${SOLAR_OS_BOARD_CONFIG}")
    message(FATAL_ERROR "SolarOS board target not found: ${SOLAR_OS_BOARD_CONFIG}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SOLAR_OS_BOARD_CONFIG}")
include("${SOLAR_OS_BOARD_CONFIG}")
if(NOT SOLAR_OS_BOARD_ID OR NOT SOLAR_OS_BOARD_NAME OR NOT SOLAR_OS_BOARD_DEFINE)
    message(FATAL_ERROR "SolarOS board profile is incomplete: ${SOLAR_OS_BOARD_CONFIG}")
endif()
message(STATUS "SolarOS board: ${SOLAR_OS_BOARD_NAME} (${SOLAR_OS_BOARD_ID})")

set(SOLAR_OS_BOARD_CAPABILITY_NAMES
    PSRAM
    DISPLAY
    GFX
    CDC
    UART
    SD
    I2C
    RTC
    BATTERY
    AUDIO
    WIFI
    BLE
    GPIO
    ADC
    PWM
    KEY
    TEMPERATURE
    HUMIDITY
)
set(SOLAR_OS_BOARD_CAPABILITY_DEFINES)
foreach(SOLAR_OS_BOARD_CAPABILITY IN LISTS SOLAR_OS_BOARD_CAPABILITY_NAMES)
    if(NOT DEFINED SOLAR_OS_BOARD_HAS_${SOLAR_OS_BOARD_CAPABILITY})
        set(SOLAR_OS_BOARD_HAS_${SOLAR_OS_BOARD_CAPABILITY} OFF)
    endif()
    if(SOLAR_OS_BOARD_HAS_${SOLAR_OS_BOARD_CAPABILITY})
        list(APPEND SOLAR_OS_BOARD_CAPABILITY_DEFINES
            "SOLAR_OS_BOARD_HAS_${SOLAR_OS_BOARD_CAPABILITY}=1"
        )
    else()
        list(APPEND SOLAR_OS_BOARD_CAPABILITY_DEFINES
            "SOLAR_OS_BOARD_HAS_${SOLAR_OS_BOARD_CAPABILITY}=0"
        )
    endif()
endforeach()

if(NOT DEFINED SOLAR_OS_BOARD_PSRAM_BYTES OR
        "${SOLAR_OS_BOARD_PSRAM_BYTES}" STREQUAL "")
    set(SOLAR_OS_BOARD_PSRAM_BYTES 0)
endif()
if(SOLAR_OS_BOARD_HAS_PSRAM AND SOLAR_OS_BOARD_PSRAM_BYTES EQUAL 0)
    message(FATAL_ERROR
        "SolarOS board ${SOLAR_OS_BOARD_ID} enables PSRAM without SOLAR_OS_BOARD_PSRAM_BYTES"
    )
endif()

if(NOT DEFINED SOLAR_OS_FLAVOR OR SOLAR_OS_FLAVOR STREQUAL "")
    if(DEFINED ENV{SOLAR_OS_FLAVOR} AND NOT "$ENV{SOLAR_OS_FLAVOR}" STREQUAL "")
        set(SOLAR_OS_FLAVOR "$ENV{SOLAR_OS_FLAVOR}")
    else()
        set(SOLAR_OS_FLAVOR "full")
    endif()
endif()

set(SOLAR_OS_FLAVOR_INPUT "${SOLAR_OS_PROJECT_DIR}/flavors/${SOLAR_OS_FLAVOR}.toml")
if(NOT EXISTS "${SOLAR_OS_FLAVOR_INPUT}")
    message(FATAL_ERROR "SolarOS flavor not found: ${SOLAR_OS_FLAVOR_INPUT}")
endif()
set(SOLAR_OS_PACKAGE_CATALOG "${SOLAR_OS_PROJECT_DIR}/packages/solar_os_packages.toml")
if(NOT EXISTS "${SOLAR_OS_PACKAGE_CATALOG}")
    message(FATAL_ERROR "SolarOS package catalog not found: ${SOLAR_OS_PACKAGE_CATALOG}")
endif()
set(SOLAR_OS_GENERATED_FONT_SENTINEL
    "${SOLAR_OS_PROJECT_DIR}/fonts/build/u8g2/u8g2_font_solar_os_default_r_14_tf.c"
)
if(NOT EXISTS "${SOLAR_OS_GENERATED_FONT_SENTINEL}")
    message(FATAL_ERROR "SolarOS generated default fonts missing: ${SOLAR_OS_GENERATED_FONT_SENTINEL}. Run: make -C fonts/tools/bdfconv && python3 fonts/generate_u8g2_fonts.py")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SOLAR_OS_FLAVOR_INPUT}"
    "${SOLAR_OS_PACKAGE_CATALOG}"
    "${SOLAR_OS_PROJECT_DIR}/scripts/generate_flavor_config.py"
    "${SOLAR_OS_GENERATED_FONT_SENTINEL}"
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(SOLAR_OS_CMAKE_BINARY_DIR "${CMAKE_BINARY_DIR}" ABSOLUTE)
if(SOLAR_OS_CMAKE_BINARY_DIR STREQUAL SOLAR_OS_PROJECT_DIR
        AND DEFINED ENV{PIOENV}
        AND NOT "$ENV{PIOENV}" STREQUAL "")
    set(SOLAR_OS_GENERATED_BASE "${SOLAR_OS_PROJECT_DIR}/.pio/build/$ENV{PIOENV}")
else()
    set(SOLAR_OS_GENERATED_BASE "${SOLAR_OS_CMAKE_BINARY_DIR}")
endif()
set(SOLAR_OS_GENERATED_DIR "${SOLAR_OS_GENERATED_BASE}/generated/solar_os")
set(SOLAR_OS_GENERATED_HEADER "${SOLAR_OS_GENERATED_DIR}/solar_os_config.h")
set(SOLAR_OS_GENERATED_CMAKE "${SOLAR_OS_GENERATED_DIR}/solar_os_config.cmake")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${SOLAR_OS_PROJECT_DIR}/scripts/generate_flavor_config.py"
            "--input" "${SOLAR_OS_FLAVOR_INPUT}"
            "--packages" "${SOLAR_OS_PACKAGE_CATALOG}"
            "--header" "${SOLAR_OS_GENERATED_HEADER}"
            "--cmake" "${SOLAR_OS_GENERATED_CMAKE}"
    RESULT_VARIABLE SOLAR_OS_FLAVOR_GENERATE_RESULT
)
if(NOT SOLAR_OS_FLAVOR_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "SolarOS flavor generation failed")
endif()
include("${SOLAR_OS_GENERATED_CMAKE}")
message(STATUS "SolarOS flavor: ${SOLAR_OS_FLAVOR_NAME} (${SOLAR_OS_FLAVOR_INPUT})")

if(NOT DEFINED SOLAR_OS_OTA_PUBLIC_KEY_FILE OR
        "${SOLAR_OS_OTA_PUBLIC_KEY_FILE}" STREQUAL "")
    if(DEFINED ENV{SOLAR_OS_OTA_PUBLIC_KEY_FILE} AND
            NOT "$ENV{SOLAR_OS_OTA_PUBLIC_KEY_FILE}" STREQUAL "")
        set(SOLAR_OS_OTA_PUBLIC_KEY_FILE "$ENV{SOLAR_OS_OTA_PUBLIC_KEY_FILE}")
    else()
        set(SOLAR_OS_OTA_PUBLIC_KEY_FILE "${SOLAR_OS_PROJECT_DIR}/keys/ota_signing_public.pem")
    endif()
endif()
if(NOT EXISTS "${SOLAR_OS_OTA_PUBLIC_KEY_FILE}")
    message(FATAL_ERROR "SolarOS OTA public key not found: ${SOLAR_OS_OTA_PUBLIC_KEY_FILE}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SOLAR_OS_OTA_PUBLIC_KEY_FILE}"
)
file(READ "${SOLAR_OS_OTA_PUBLIC_KEY_FILE}" SOLAR_OS_OTA_PUBLIC_KEY_PEM)
string(REPLACE "\\" "\\\\" SOLAR_OS_OTA_PUBLIC_KEY_ESC "${SOLAR_OS_OTA_PUBLIC_KEY_PEM}")
string(REPLACE "\"" "\\\"" SOLAR_OS_OTA_PUBLIC_KEY_ESC "${SOLAR_OS_OTA_PUBLIC_KEY_ESC}")
string(REPLACE "\n" "\\n" SOLAR_OS_OTA_PUBLIC_KEY_ESC "${SOLAR_OS_OTA_PUBLIC_KEY_ESC}")
set(SOLAR_OS_GENERATED_OTA_KEY_HEADER "${SOLAR_OS_GENERATED_DIR}/solar_os_ota_key.h")
file(WRITE "${SOLAR_OS_GENERATED_OTA_KEY_HEADER}"
    "/* Generated by CMake. Do not edit. */\n"
    "#pragma once\n\n"
    "#define SOLAR_OS_OTA_PUBLIC_KEY_PEM \"${SOLAR_OS_OTA_PUBLIC_KEY_ESC}\"\n"
    "#define SOLAR_OS_OTA_PUBLIC_KEY_FILE \"${SOLAR_OS_OTA_PUBLIC_KEY_FILE}\"\n"
)

set(SOLAR_OS_SRCS
    "main.c"
    "solar_os.c"
    "solar_os_jobs.c"
    "solar_os_task.c"
    "apps/solar_os_app_registry.c"
    "apps/solar_os_shell.c"
    "shell/solar_os_shell_commands.c"
    "shell/solar_os_shell_io.c"
    "shell/solar_os_vt100.c"
    "jobs/solar_os_job_registry.c"
    "services/solar_os_adc.c"
    "services/solar_os_audio.c"
    "services/solar_os_battery.c"
    "services/solar_os_ble_keyboard.c"
    "services/solar_os_board_caps.c"
    "services/solar_os_cdc.c"
    "services/solar_os_clipboard.c"
    "services/solar_os_crypto.c"
    "services/solar_os_doc.c"
    "services/solar_os_epub.c"
    "services/solar_os_gfx.c"
    "services/solar_os_html.c"
    "services/solar_os_gpio.c"
    "services/solar_os_i2c.c"
    "services/solar_os_identity.c"
    "services/solar_os_json.c"
    "services/solar_os_log.c"
    "services/solar_os_memory.c"
    "services/solar_os_ota.c"
    "services/solar_os_port.c"
    "services/solar_os_power.c"
    "services/solar_os_pwm.c"
    "services/solar_os_sensors.c"
    "services/solar_os_splash.c"
    "services/solar_os_storage.c"
    "services/solar_os_stream.c"
    "services/solar_os_terminal.c"
    "services/solar_os_time.c"
    "services/solar_os_transfer.c"
    "services/solar_os_tui.c"
    "services/solar_os_uart.c"
    "services/solar_os_wifi.c"
    "services/solar_os_xml.c"
    "services/solar_os_zip.c"
    "services/solar_os_zmodem.c"
)

if(SOLAR_OS_BOARD_HAS_CDC)
    list(APPEND SOLAR_OS_SRCS "drivers/cdc_port.c")
endif()
if(SOLAR_OS_BOARD_HAS_DISPLAY)
    if(NOT DEFINED SOLAR_OS_BOARD_DISPLAY_DRIVER OR
            "${SOLAR_OS_BOARD_DISPLAY_DRIVER}" STREQUAL "")
        message(FATAL_ERROR
            "SolarOS board ${SOLAR_OS_BOARD_ID} enables DISPLAY without SOLAR_OS_BOARD_DISPLAY_DRIVER"
        )
    endif()
    if("${SOLAR_OS_BOARD_DISPLAY_DRIVER}" STREQUAL "st7305")
        list(APPEND SOLAR_OS_SRCS
            "board/solar_os_board_display_st7305.c"
            "drivers/rlcd_st7305.c"
        )
    else()
        message(FATAL_ERROR
            "Unsupported SolarOS display driver for ${SOLAR_OS_BOARD_ID}: ${SOLAR_OS_BOARD_DISPLAY_DRIVER}"
        )
    endif()
endif()
if(SOLAR_OS_BOARD_HAS_UART)
    list(APPEND SOLAR_OS_SRCS "drivers/uart_port.c")
endif()
if(SOLAR_OS_BOARD_HAS_SD)
    if(NOT DEFINED SOLAR_OS_BOARD_STORAGE_DRIVER OR
            "${SOLAR_OS_BOARD_STORAGE_DRIVER}" STREQUAL "")
        message(FATAL_ERROR
            "SolarOS board ${SOLAR_OS_BOARD_ID} enables SD without SOLAR_OS_BOARD_STORAGE_DRIVER"
        )
    endif()
    if("${SOLAR_OS_BOARD_STORAGE_DRIVER}" STREQUAL "sdmmc")
        list(APPEND SOLAR_OS_SRCS
            "board/solar_os_board_storage_sdmmc.c"
            "drivers/sd_card.c"
        )
    else()
        message(FATAL_ERROR
            "Unsupported SolarOS storage driver for ${SOLAR_OS_BOARD_ID}: ${SOLAR_OS_BOARD_STORAGE_DRIVER}"
        )
    endif()
endif()
if(SOLAR_OS_BOARD_HAS_I2C OR
        SOLAR_OS_BOARD_HAS_RTC OR
        SOLAR_OS_BOARD_HAS_TEMPERATURE OR
        SOLAR_OS_BOARD_HAS_HUMIDITY OR
        SOLAR_OS_BOARD_HAS_AUDIO)
    list(APPEND SOLAR_OS_SRCS "drivers/i2c_bus.c")
endif()
if(SOLAR_OS_BOARD_HAS_RTC)
    if(NOT DEFINED SOLAR_OS_BOARD_RTC_DRIVER OR
            "${SOLAR_OS_BOARD_RTC_DRIVER}" STREQUAL "")
        message(FATAL_ERROR
            "SolarOS board ${SOLAR_OS_BOARD_ID} enables RTC without SOLAR_OS_BOARD_RTC_DRIVER"
        )
    endif()
    if("${SOLAR_OS_BOARD_RTC_DRIVER}" STREQUAL "pcf85063")
        list(APPEND SOLAR_OS_SRCS
            "board/solar_os_board_rtc_pcf85063.c"
            "drivers/rtc_pcf85063.c"
        )
    else()
        message(FATAL_ERROR
            "Unsupported SolarOS RTC driver for ${SOLAR_OS_BOARD_ID}: ${SOLAR_OS_BOARD_RTC_DRIVER}"
        )
    endif()
endif()
if(SOLAR_OS_BOARD_HAS_TEMPERATURE OR SOLAR_OS_BOARD_HAS_HUMIDITY)
    if(NOT DEFINED SOLAR_OS_BOARD_SENSOR_DRIVER OR
            "${SOLAR_OS_BOARD_SENSOR_DRIVER}" STREQUAL "")
        message(FATAL_ERROR
            "SolarOS board ${SOLAR_OS_BOARD_ID} enables sensors without SOLAR_OS_BOARD_SENSOR_DRIVER"
        )
    endif()
    if("${SOLAR_OS_BOARD_SENSOR_DRIVER}" STREQUAL "shtc3")
        list(APPEND SOLAR_OS_SRCS
            "board/solar_os_board_sensors_shtc3.c"
            "drivers/shtc3.c"
        )
    else()
        message(FATAL_ERROR
            "Unsupported SolarOS sensor driver for ${SOLAR_OS_BOARD_ID}: ${SOLAR_OS_BOARD_SENSOR_DRIVER}"
        )
    endif()
endif()
if(SOLAR_OS_BOARD_HAS_AUDIO)
    if(NOT DEFINED SOLAR_OS_BOARD_AUDIO_DRIVER OR
            "${SOLAR_OS_BOARD_AUDIO_DRIVER}" STREQUAL "")
        message(FATAL_ERROR
            "SolarOS board ${SOLAR_OS_BOARD_ID} enables AUDIO without SOLAR_OS_BOARD_AUDIO_DRIVER"
        )
    endif()
    if("${SOLAR_OS_BOARD_AUDIO_DRIVER}" STREQUAL "es8311_es7210")
        list(APPEND SOLAR_OS_SRCS
            "board/solar_os_board_audio_es8311_es7210.c"
            "drivers/audio_codec_board.c"
        )
    else()
        message(FATAL_ERROR
            "Unsupported SolarOS audio driver for ${SOLAR_OS_BOARD_ID}: ${SOLAR_OS_BOARD_AUDIO_DRIVER}"
        )
    endif()
endif()
if(SOLAR_OS_BOARD_HAS_GPIO)
    list(APPEND SOLAR_OS_SRCS "drivers/gpio_port.c")
endif()
if(SOLAR_OS_BOARD_HAS_ADC OR SOLAR_OS_BOARD_HAS_BATTERY)
    list(APPEND SOLAR_OS_SRCS "drivers/adc_port.c")
endif()
if(SOLAR_OS_BOARD_HAS_BATTERY)
    if(NOT DEFINED SOLAR_OS_BOARD_BATTERY_DRIVER OR
            "${SOLAR_OS_BOARD_BATTERY_DRIVER}" STREQUAL "")
        message(FATAL_ERROR
            "SolarOS board ${SOLAR_OS_BOARD_ID} enables BATTERY without SOLAR_OS_BOARD_BATTERY_DRIVER"
        )
    endif()
    if("${SOLAR_OS_BOARD_BATTERY_DRIVER}" STREQUAL "adc")
        list(APPEND SOLAR_OS_SRCS
            "board/solar_os_board_battery_adc.c"
            "drivers/battery_adc.c"
        )
    else()
        message(FATAL_ERROR
            "Unsupported SolarOS battery driver for ${SOLAR_OS_BOARD_ID}: ${SOLAR_OS_BOARD_BATTERY_DRIVER}"
        )
    endif()
endif()
if(SOLAR_OS_BOARD_HAS_PWM)
    list(APPEND SOLAR_OS_SRCS "drivers/pwm_port.c")
endif()

set(SOLAR_OS_REQUIRES
    app_update
    bt
    esp-tls
    esp_adc
    esp_codec_dev
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_sdmmc
    esp_driver_spi
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_event
    esp_hid
    esp_http_client
    esp_https_ota
    esp_netif
    esp_rom
    esp_timer
    esp_wifi
    fatfs
    heap
    json
    lwip
    mbedtls
    nvs_flash
    sdmmc
    u8g2
)

list(APPEND SOLAR_OS_SRCS ${SOLAR_OS_PACKAGE_SRCS})
list(APPEND SOLAR_OS_REQUIRES ${SOLAR_OS_PACKAGE_REQUIRES})
list(REMOVE_DUPLICATES SOLAR_OS_SRCS)
list(REMOVE_DUPLICATES SOLAR_OS_REQUIRES)

set(SOLAR_OS_FLAVOR_STAMP
    "${SOLAR_OS_FLAVOR_NAME}:${SOLAR_OS_PACKAGE_LIST}"
)

idf_component_register(
    SRCS ${SOLAR_OS_SRCS}
    INCLUDE_DIRS "." "apps" "board" "drivers" "jobs" "services" "shell" "../include" "${SOLAR_OS_GENERATED_DIR}"
    REQUIRES ${SOLAR_OS_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ${SOLAR_OS_BOARD_DEFINE}=1
    ${SOLAR_OS_BOARD_CAPABILITY_DEFINES}
    SOLAR_OS_BOARD_PSRAM_BYTES=${SOLAR_OS_BOARD_PSRAM_BYTES}
    SOLAR_OS_BOARD_TARGET="${SOLAR_OS_BOARD_ID}"
    SOLAR_OS_VERSION="${SOLAR_OS_VERSION}"
    SOLAR_OS_FLAVOR_STAMP="${SOLAR_OS_FLAVOR_STAMP}"
)
