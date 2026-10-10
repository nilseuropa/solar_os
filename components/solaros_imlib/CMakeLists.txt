idf_component_register(SRCS
    upstream/binary.c upstream/filter.c upstream/stats.c upstream/blob.c
    upstream/mathop.c upstream/collections.c upstream/fmath.c upstream/fsort.c
    upstream/core.c upstream/lab_table.c upstream/sincos_table.c
    INCLUDE_DIRS include upstream)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CMSIS_MCU_H="cmsis_compiler.h" OMV_NO_GPL=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-strict-aliasing -Wno-error)
