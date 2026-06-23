idf_component_register(
    SRCS "minimp3_impl.c"
    INCLUDE_DIRS "include"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MINIMP3_NO_SIMD
    MINIMP3_ONLY_MP3
)
