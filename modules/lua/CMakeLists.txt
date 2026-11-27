cmake_minimum_required(VERSION 3.16)
project(solaros_lua NONE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(LUA_CC NAMES xtensa-esp32s3-elf-gcc)
if(NOT LUA_CC)
    message(FATAL_ERROR "xtensa-esp32s3-elf-gcc is required to build Lua")
endif()
if(NOT SOLAROS_FIRMWARE_PROJECT_DIR)
    get_filename_component(SOLAROS_FIRMWARE_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
file(GLOB LUA_MODULE_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/module.toml")
file(GLOB LUA_VENDOR_INPUTS CONFIGURE_DEPENDS
    "${SOLAROS_FIRMWARE_PROJECT_DIR}/components/lua/lua/src/*"
    "${SOLAROS_FIRMWARE_PROJECT_DIR}/include/solar_os*.h")
add_custom_command(OUTPUT lua.app.elf
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
        --firmware "${SOLAROS_FIRMWARE_PROJECT_DIR}" --cc "${LUA_CC}" --output "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS ${LUA_MODULE_INPUTS} ${LUA_VENDOR_INPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
        "${SOLAROS_FIRMWARE_PROJECT_DIR}/components/lua/sources.txt"
        "${SOLAROS_FIRMWARE_PROJECT_DIR}/modules/espdl/layout.ld"
        "${SOLAROS_FIRMWARE_PROJECT_DIR}/modules/espdl/verify.py"
        "${SOLAROS_FIRMWARE_PROJECT_DIR}/scripts/native_component_check.c"
        "${SOLAROS_FIRMWARE_PROJECT_DIR}/src/services/solar_os_native_elf.c"
    VERBATIM)
add_custom_target(elf ALL DEPENDS lua.app.elf)
