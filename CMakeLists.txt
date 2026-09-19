cmake_minimum_required(VERSION 3.16)

include(patches/nimble/config_guard.cmake)
solar_os_regenerate_stale_sdkconfig()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "ESP_IDF_LWIP_HOOK_FILENAME=\"${CMAKE_CURRENT_LIST_DIR}/src/services/solar_os_lwip_hooks.h\""
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS "MEMP_NUM_PPP_PCB=2" APPEND)
project(solar_os)
include(patches/nimble/overlay.cmake)
