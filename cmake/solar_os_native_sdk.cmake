# Package declared SDK headers before the artifact is catalogued and signed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SOLAR_OS_NATIVE_SDK_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../scripts/pack_native_sdk.py")
function(solar_os_native_sdk input output)
    set(manifest "${CMAKE_CURRENT_SOURCE_DIR}/module.toml")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${SOLAR_OS_NATIVE_SDK_SCRIPT}"
        --manifest "${manifest}" --list-inputs
        OUTPUT_VARIABLE sdk_inputs RESULT_VARIABLE sdk_status)
    if(NOT sdk_status EQUAL 0)
        message(FATAL_ERROR "Invalid SDK declarations in ${manifest}")
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${SOLAR_OS_NATIVE_SDK_SCRIPT}"
            --manifest "${manifest}" --input "${input}" --output "${output}"
        DEPENDS elf "${manifest}" "${SOLAR_OS_NATIVE_SDK_SCRIPT}" ${sdk_inputs}
        VERBATIM)
    add_custom_target(typed_elf ALL DEPENDS "${output}")
endfunction()
