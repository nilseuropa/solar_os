cmake_minimum_required(VERSION 3.16)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(
    solar_os_picotts_source
    GIT_REPOSITORY https://github.com/DiUS/esp-picotts.git
    GIT_TAG bf1a8df9d2be1e088a03a775d439ac66e18438dc
    GIT_SHALLOW FALSE
    GIT_SUBMODULES ""
    SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/picotts"
)
FetchContent_GetProperties(solar_os_picotts_source)
if(NOT solar_os_picotts_source_POPULATED)
    FetchContent_Populate(solar_os_picotts_source)
endif()
set(SOLAR_OS_PICOTTS_SOURCE_DIR
    "${solar_os_picotts_source_SOURCE_DIR}"
    CACHE INTERNAL "Pinned PicoTTS source used by the SolarOS component"
)
set(ENV{SOLAR_OS_PICOTTS_SOURCE_DIR} "${solar_os_picotts_source_SOURCE_DIR}")
set(SOLAR_OS_PICOTTS_VOICE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/picotts_voices"
    CACHE INTERNAL "Versioned PicoTTS runtime voice directory"
)
set(ENV{SOLAR_OS_PICOTTS_VOICE_DIR} "${SOLAR_OS_PICOTTS_VOICE_DIR}")

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
