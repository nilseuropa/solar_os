# Optional firmware deployment of the same compiler and command as the ELFs.
function(solar_os_link_builtin_cc target)
    set(cc_source "${SOLAR_OS_PROJECT_DIR}/modules/experimental/tinycc")
    set(cc_output "${CMAKE_BINARY_DIR}/tinycc")
    file(GLOB cc_inputs CONFIGURE_DEPENDS
        "${cc_source}/*.c" "${cc_source}/*.h" "${cc_source}/*.py" "${cc_source}/*.json")
    file(GLOB_RECURSE cc_extra_inputs CONFIGURE_DEPENDS
        "${cc_source}/include/*.h" "${cc_source}/sdk/*.h" "${cc_source}/examples/*.c")
    file(GLOB cc_headers CONFIGURE_DEPENDS
        "${SOLAR_OS_PROJECT_DIR}/include/solar_os_native*.h"
        "${SOLAR_OS_PROJECT_DIR}/include/solar_os_*_types_v1.h")
    add_custom_command(OUTPUT "${cc_output}/libsolar_os_cc.a"
        COMMAND "${Python3_EXECUTABLE}" "${cc_source}/native_build.py"
            --builtin-output "${cc_output}"
        DEPENDS ${cc_inputs} ${cc_extra_inputs} ${cc_headers}
            "${SOLAR_OS_PROJECT_DIR}/modules/cc/module.toml"
            "${SOLAR_OS_PROJECT_DIR}/modules/tinycc/module.toml"
            "${SOLAR_OS_PROJECT_DIR}/modules/examples/hello-library/main/hello_library_api.h"
            "${SOLAR_OS_PROJECT_DIR}/src/services/solar_os_native_elf.c"
            "${SOLAR_OS_PROJECT_DIR}/src/services/solar_os_native_elf.h"
        VERBATIM)
    add_custom_target(solar_os_cc_objects DEPENDS "${cc_output}/libsolar_os_cc.a")
    add_dependencies(${target} solar_os_cc_objects)
    target_link_libraries(${target} PUBLIC "${cc_output}/libsolar_os_cc.a")
endfunction()
