set(core_sources vendor/memory.cpp vendor/cia.cpp vendor/custom.cpp
    vendor/drive.cpp vendor/input.cpp vendor/video.cpp vendor/blitter.cpp
    vendor/disk.cpp vendor/paula.cpp vendor/musashi/m68kcpu.c vendor/musashi/m68kops.c)
idf_component_register(SRCS amiga.cpp rtc.cpp keyboard.cpp ${core_sources}
    INCLUDE_DIRS "." "../../../include" "vendor" "vendor/musashi")
target_compile_definitions(${COMPONENT_LIB} PRIVATE SOLAR_OS_AMIGA=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -g0 -fno-strict-aliasing -flto)
set_source_files_properties(${core_sources} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/port.h;-Wno-error;-Wno-unused-variable;-Wno-unused-function")
