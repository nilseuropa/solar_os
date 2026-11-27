if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(lab_source "${CMAKE_CURRENT_LIST_DIR}/upstream/lab_table.c")
    set(lab_pack "${CMAKE_CURRENT_LIST_DIR}/pack_lab_table.py")
    set(lab_packed "${CMAKE_CURRENT_BINARY_DIR}/lab_table_packed.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lab_source}" "${lab_pack}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${lab_pack}" "${lab_source}" "${lab_packed}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()

idf_component_register(SRCS
    upstream/binary.c upstream/filter.c upstream/stats.c upstream/blob.c
    upstream/mathop.c upstream/collections.c upstream/fmath.c upstream/fsort.c
    upstream/core.c "${lab_packed}" upstream/sincos_table.c
    INCLUDE_DIRS include upstream)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CMSIS_MCU_H="cmsis_compiler.h" OMV_NO_GPL=1)
target_compile_definitions(${COMPONENT_LIB} PUBLIC IMLIB_SOLAROS_PACKED_LAB_LUT=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-strict-aliasing -Wno-error)
