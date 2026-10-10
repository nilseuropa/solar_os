set(quirc_sources lib/quirc.c lib/identify.c lib/decode.c lib/version_db.c)
idf_component_register(SRCS ${quirc_sources} solar_os_quirc_alloc.c
    INCLUDE_DIRS lib PRIV_REQUIRES heap)
target_compile_definitions(${COMPONENT_LIB} PRIVATE QUIRC_FLOAT_TYPE=float QUIRC_USE_TGMATH=1)
set_source_files_properties(${quirc_sources} PROPERTIES COMPILE_DEFINITIONS
    "malloc=solar_os_quirc_malloc;calloc=solar_os_quirc_calloc;free=solar_os_quirc_free")
