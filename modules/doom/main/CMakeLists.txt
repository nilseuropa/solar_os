file(GLOB core_sources "vendor/prboom/*.c" "vendor/tables/*.c")
list(FILTER core_sources EXCLUDE REGEX "/lprintf\\.c$")
idf_component_register(SRCS doom.c port.c libc.c video.c i_main.c i_sound.c sound_mixer.c ${core_sources}
    INCLUDE_DIRS "." "../../../include" "vendor/prboom/include" "vendor/tables/include")
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -g0 -fno-strict-aliasing -fstack-usage
    -include ${CMAKE_CURRENT_SOURCE_DIR}/port.h)
set_source_files_properties(${core_sources} i_main.c i_sound.c PROPERTIES
    COMPILE_OPTIONS "-Wno-error;-Wno-pointer-sign")
set_source_files_properties(port.c PROPERTIES COMPILE_DEFINITIONS SOLAR_OS_DOOM_PORT_IMPLEMENTATION)
set_source_files_properties(libc.c PROPERTIES
    COMPILE_DEFINITIONS SOLAR_OS_DOOM_PORT_IMPLEMENTATION COMPILE_OPTIONS -fno-builtin)
set_source_files_properties(vendor/prboom/d_deh.c PROPERTIES
    COMPILE_DEFINITIONS ProcessDehFile=solar_os_doom_unused_deh)
