cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(solaros_emotion)
include(elf_loader)
set(ELF_LIBS "-lgcc")
project_elf(solaros_emotion)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/solar_os_native_sdk.cmake")
solar_os_native_sdk(solaros_emotion.app.elf solaros_emotion.library.elf
    SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
