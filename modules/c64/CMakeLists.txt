cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solaros_native_c64)
include(elf_loader)
# Follow project_elf's ABI, retaining an unstripped ELF for crash decoding.
idf_build_set_property(COMPILE_OPTIONS
    "-nostartfiles;-nostdlib;-fPIC;-fvisibility=hidden;-Dmain=app_main" APPEND)
set(elf_app "${CMAKE_BINARY_DIR}/solaros_native_c64.app.elf")
set(elf_debug "${CMAKE_BINARY_DIR}/solaros_native_c64.unstripped.elf")
string(REPLACE "-elf-gcc" "-elf-strip" c64_strip "${CMAKE_C_COMPILER}")
add_custom_command(OUTPUT "${elf_app}"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${elf_app}"
    COMMAND ${CMAKE_C_COMPILER} -nostartfiles -nostdlib -fPIC -shared -e app_main
        -fdata-sections -ffunction-sections -fvisibility=hidden -Wl,--gc-sections
        -O2 -g0 -Wl,--no-relax
        -Wl,-Map=solaros_native_c64.map -Wl,--undefined=solar_os_native_app_manifest
        esp-idf/main/libmain.a -lgcc -o "${elf_debug}"
    COMMAND ${c64_strip} --strip-unneeded --remove-section=.comment
        --remove-section=.got.loc --remove-section=.dynamic --remove-section=.xt.lit
        --remove-section=.xt.prop --remove-section=.xtensa.info
        -o "${elf_app}" "${elf_debug}"
    DEPENDS idf::main
    VERBATIM)
add_custom_target(elf ALL DEPENDS "${elf_app}")
add_custom_target(typed_elf ALL DEPENDS solaros_native_c64.app.elf)
