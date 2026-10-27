cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solaros_native_hello_library)
include(elf_loader)
project_elf(solaros_native_hello_library)
add_custom_command(OUTPUT solaros_native_hello_library.library.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        solaros_native_hello_library.app.elf solaros_native_hello_library.library.elf
    DEPENDS elf)
add_custom_target(typed_elf ALL DEPENDS solaros_native_hello_library.library.elf)
