cmake_minimum_required(VERSION 3.16)
project(solaros_espdl_library NONE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SOLAROS_ESPDL_ENVIRONMENT "freenove_esp32_s3_display_4_0" CACHE STRING "ESP32-S3 SDK build environment")
if(NOT SOLAROS_FIRMWARE_PROJECT_DIR)
    get_filename_component(SOLAROS_FIRMWARE_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
add_custom_target(typed_elf ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
        --firmware-project "${SOLAROS_FIRMWARE_PROJECT_DIR}"
        --environment "${SOLAROS_ESPDL_ENVIRONMENT}" --output "${CMAKE_CURRENT_BINARY_DIR}"
    VERBATIM)
