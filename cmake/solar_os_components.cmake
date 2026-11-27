# SDK-only archives selected by the resolved flavor, including without ELF.
function(solar_os_link_components target)
    foreach(manifest IN LISTS SOLAR_OS_COMPONENT_BUILD_MANIFESTS)
        set(builder "${SOLAR_OS_PROJECT_DIR}/scripts/build_native_component.py")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${builder}"
            --manifest "${manifest}" --list-inputs
            OUTPUT_VARIABLE inputs RESULT_VARIABLE result)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Invalid native component build: ${manifest}")
        endif()
        # Paths and artifact names come from the same validated manifest.
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${builder}"
            --manifest "${manifest}" --identity --target "${IDF_TARGET}"
            OUTPUT_VARIABLE identity RESULT_VARIABLE result)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Invalid native component description: ${manifest}")
        endif()
        set(output "${CMAKE_BINARY_DIR}/native-components/${identity}")
        set(archive "${output}/libsolar_os_${identity}.a")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})
        add_custom_command(OUTPUT "${archive}"
            COMMAND "${Python3_EXECUTABLE}" "${builder}" --manifest "${manifest}"
                --output "${output}" --mode builtin --cc "${CMAKE_C_COMPILER}"
                --cxx "${CMAKE_CXX_COMPILER}" --target "${IDF_TARGET}"
            DEPENDS ${inputs} VERBATIM)
        add_custom_target(solar_os_component_${identity} DEPENDS "${archive}")
        add_dependencies(${target} solar_os_component_${identity})
        target_link_libraries(${target} PUBLIC "${archive}")
    endforeach()
endfunction()
