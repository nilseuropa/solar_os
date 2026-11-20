set(rom_source "${CMAKE_CURRENT_BINARY_DIR}/roms_data.c")
idf_component_register(SRCS c64.cpp input.cpp audio.cpp "${rom_source}"
    vendor/CPU6502.cpp vendor/C64Sys.cpp vendor/CIA.cpp vendor/VIC.cpp vendor/SID.cpp
    INCLUDE_DIRS "." "../../../include" "vendor")
add_custom_command(OUTPUT "${rom_source}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/embed_roms.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../rom" "${rom_source}"
    DEPENDS "../scripts/embed_roms.py" "../rom/sources.json"
        "../rom/basic.rom" "../rom/kernal.rom" "../rom/chargen.rom" "roms.h")
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2 -g0 -fno-strict-aliasing
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
# Let the compiler inline the sole C64 bus into the CPU without relying on
# Xtensa link-time optimization, which fails when linking this shared ELF.
set_target_properties(${COMPONENT_LIB} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 0)
