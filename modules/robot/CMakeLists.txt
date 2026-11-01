cmake_minimum_required(VERSION 3.16)
project(solaros_robot_library NONE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT SOLAROS_FIRMWARE_PROJECT_DIR)
    get_filename_component(SOLAROS_FIRMWARE_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
set(SOLAROS_ROBOT_ENVIRONMENT "esp32_s3_devkitc1_n16r8" CACHE STRING "ESP32-S3 SDK environment")
add_custom_target(typed_elf ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
        --environment "${SOLAROS_ROBOT_ENVIRONMENT}" --output "${CMAKE_CURRENT_BINARY_DIR}"
        --firmware-project "${SOLAROS_FIRMWARE_PROJECT_DIR}"
    VERBATIM)
