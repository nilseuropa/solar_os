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
set(SOLAR_OS_GENERATED_FONT_SENTINEL
    "${SOLAR_OS_PROJECT_DIR}/fonts/build/u8g2/u8g2_font_solar_os_default_r_14_tf.c"
)
if(NOT EXISTS "${SOLAR_OS_GENERATED_FONT_SENTINEL}")
    message(FATAL_ERROR "SolarOS generated default fonts missing: ${SOLAR_OS_GENERATED_FONT_SENTINEL}. Run: make -C fonts/tools/bdfconv && python3 fonts/generate_u8g2_fonts.py")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SOLAR_OS_FLAVOR_INPUT}"
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
            "--header" "${SOLAR_OS_GENERATED_HEADER}"
            "--cmake" "${SOLAR_OS_GENERATED_CMAKE}"
    RESULT_VARIABLE SOLAR_OS_FLAVOR_GENERATE_RESULT
)
if(NOT SOLAR_OS_FLAVOR_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "SolarOS flavor generation failed")
endif()
include("${SOLAR_OS_GENERATED_CMAKE}")
message(STATUS "SolarOS flavor: ${SOLAR_OS_FLAVOR_NAME} (${SOLAR_OS_FLAVOR_INPUT})")

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
    "jobs/solar_os_batmon_job.c"
    "jobs/solar_os_bridge_job.c"
    "jobs/solar_os_daq_job.c"
    "jobs/solar_os_job_registry.c"
    "jobs/solar_os_log_job.c"
    "jobs/solar_os_shell_job.c"
    "drivers/cdc_port.c"
    "drivers/adc_port.c"
    "drivers/audio_codec_board.c"
    "drivers/battery_adc.c"
    "drivers/gpio_port.c"
    "drivers/i2c_bus.c"
    "drivers/pwm_port.c"
    "drivers/rlcd_st7305.c"
    "drivers/rtc_pcf85063.c"
    "drivers/sd_card.c"
    "drivers/shtc3.c"
    "drivers/uart_port.c"
    "services/solar_os_adc.c"
    "services/solar_os_audio.c"
    "services/solar_os_battery.c"
    "services/solar_os_ble_keyboard.c"
    "services/solar_os_cdc.c"
    "services/solar_os_clipboard.c"
    "services/solar_os_doc.c"
    "services/solar_os_gfx.c"
    "services/solar_os_gpio.c"
    "services/solar_os_i2c.c"
    "services/solar_os_identity.c"
    "services/solar_os_log.c"
    "services/solar_os_memory.c"
    "services/solar_os_ota.c"
    "services/solar_os_port.c"
    "services/solar_os_power.c"
    "services/solar_os_pwm.c"
    "services/solar_os_sensors.c"
    "services/solar_os_storage.c"
    "services/solar_os_stream.c"
    "services/solar_os_terminal.c"
    "services/solar_os_time.c"
    "services/solar_os_transfer.c"
    "services/solar_os_tui.c"
    "services/solar_os_uart.c"
    "services/solar_os_wifi.c"
    "services/solar_os_zip.c"
    "services/solar_os_zmodem.c"
)

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
    lwip
    nvs_flash
    sdmmc
    u8g2
)

if(SOLAR_OS_PACKAGE_AUDIO)
    list(APPEND SOLAR_OS_SRCS "apps/solar_os_audio_apps.c")
    list(APPEND SOLAR_OS_REQUIRES minimp3)
endif()

if(SOLAR_OS_PACKAGE_NET)
    list(APPEND SOLAR_OS_SRCS
        "apps/solar_os_chat.c"
        "apps/solar_os_curl.c"
        "apps/solar_os_scp.c"
        "apps/solar_os_ssh.c"
        "apps/solar_os_web.c"
        "jobs/solar_os_httpd_job.c"
        "jobs/solar_os_ntp_sync_job.c"
        "jobs/solar_os_slip_job.c"
        "services/solar_os_chat.c"
        "services/solar_os_mqtt.c"
        "services/solar_os_net.c"
        "services/solar_os_scp.c"
        "services/solar_os_ssh.c"
        "services/solar_os_ssh_keys.c"
        "services/solar_os_ssh_transport.c"
    )
    list(APPEND SOLAR_OS_REQUIRES
        esp_http_server
        mbedtls
        mqtt
        stb_image
        webp_decoder
    )
endif()

if(SOLAR_OS_PACKAGE_MEDIA)
    list(APPEND SOLAR_OS_SRCS "apps/solar_os_view.c")
    list(APPEND SOLAR_OS_REQUIRES stb_image webp_decoder)
endif()

if(SOLAR_OS_PACKAGE_GAMES)
    list(APPEND SOLAR_OS_SRCS "apps/solar_os_invaders.c")
endif()

if(SOLAR_OS_PACKAGE_PYTHON)
    list(APPEND SOLAR_OS_SRCS "apps/solar_os_python.c")
    list(APPEND SOLAR_OS_REQUIRES micropython_embed)
endif()

if(SOLAR_OS_PACKAGE_LUA)
    list(APPEND SOLAR_OS_SRCS "apps/solar_os_lua.c")
    list(APPEND SOLAR_OS_REQUIRES lua)
endif()

if(SOLAR_OS_PACKAGE_UTILS)
    list(APPEND SOLAR_OS_SRCS
        "apps/solar_os_clock.c"
        "apps/solar_os_com.c"
        "apps/solar_os_reader.c"
        "apps/solar_os_edit.c"
        "apps/solar_os_less.c"
        "apps/solar_os_notes.c"
        "apps/solar_os_plot.c"
        "apps/solar_os_sheet.c"
    )
endif()

list(REMOVE_DUPLICATES SOLAR_OS_REQUIRES)

set(SOLAR_OS_FLAVOR_STAMP
    "${SOLAR_OS_FLAVOR_NAME}:${SOLAR_OS_PACKAGE_CORE}${SOLAR_OS_PACKAGE_AUDIO}${SOLAR_OS_PACKAGE_NET}${SOLAR_OS_PACKAGE_MEDIA}${SOLAR_OS_PACKAGE_GAMES}${SOLAR_OS_PACKAGE_PYTHON}${SOLAR_OS_PACKAGE_LUA}${SOLAR_OS_PACKAGE_UTILS}"
)

idf_component_register(
    SRCS ${SOLAR_OS_SRCS}
    INCLUDE_DIRS "." "apps" "drivers" "jobs" "services" "shell" "../include" "${SOLAR_OS_GENERATED_DIR}"
    REQUIRES ${SOLAR_OS_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ${SOLAR_OS_BOARD_DEFINE}=1
    SOLAR_OS_BOARD_TARGET="${SOLAR_OS_BOARD_ID}"
    SOLAR_OS_VERSION="${SOLAR_OS_VERSION}"
    SOLAR_OS_FLAVOR_STAMP="${SOLAR_OS_FLAVOR_STAMP}"
)
