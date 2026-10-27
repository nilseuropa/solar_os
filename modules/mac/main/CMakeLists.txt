set(core_sources vendor/via.c vendor/scc.c vendor/iwm.c vendor/mouse.c
    vendor/rtc.c vendor/ncr.c vendor/musashi/m68kcpu.c
    vendor/musashi/m68kopac.c vendor/musashi/m68kopdm.c
    vendor/musashi/m68kopnz.c vendor/musashi/m68kops_pre.c vendor/sony/disc.c)
idf_component_register(SRCS mac.c machine.c video.c disk.c input.c floppy.c ${core_sources}
    INCLUDE_DIRS "." "../../../include" "vendor" "vendor/musashi")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -g0 -fno-strict-aliasing)
set_source_files_properties(${core_sources} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/port.h;-Wno-error;-Wno-unused-variable;-Wno-unused-function")
