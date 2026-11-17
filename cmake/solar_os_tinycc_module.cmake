# Build both halves against the same bounded compiler and target SDK.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT SOLAROS_FIRMWARE_PROJECT_DIR)
    get_filename_component(SOLAROS_FIRMWARE_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
function(solar_os_tinycc_module artifact target)
    add_custom_target(${target} ALL
        COMMAND "${Python3_EXECUTABLE}"
            "${SOLAROS_FIRMWARE_PROJECT_DIR}/modules/experimental/tinycc/native_build.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SOLAROS_FIRMWARE_PROJECT_DIR}/.pio/tinycc/native/${artifact}"
            "${CMAKE_CURRENT_BINARY_DIR}/${artifact}"
        VERBATIM)
endfunction()
