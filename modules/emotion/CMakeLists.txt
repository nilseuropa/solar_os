cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solaros_emotion)
include(elf_loader)
set(ELF_LIBS "-lgcc")
project_elf(solaros_emotion)
add_custom_command(OUTPUT solaros_emotion.library.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        solaros_emotion.app.elf solaros_emotion.library.elf
    DEPENDS elf "${CMAKE_CURRENT_BINARY_DIR}/solaros_emotion.app.elf")
add_custom_target(typed_elf ALL DEPENDS solaros_emotion.library.elf)
