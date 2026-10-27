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
include(scripts/cxx_sdkconfig.cmake)
solar_os_enable_required_cxx_exceptions()
include(scripts/sdkconfig_minimums.cmake)
solar_os_apply_sdkconfig_minimums()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "ESP_IDF_LWIP_HOOK_FILENAME=\"${CMAKE_CURRENT_LIST_DIR}/src/services/solar_os_lwip_hooks.h\""
    APPEND
)
idf_build_set_property(COMPILE_DEFINITIONS "MEMP_NUM_PPP_PCB=2" APPEND)
project(solar_os)
idf_build_get_property(solar_os_elf EXECUTABLE)
# SDK/prebuilt consumers share large PSRAM storage and can release OS-owned
# pointers. Constrained requests and pointers outside the arena retain SDK behavior.
target_link_options(${solar_os_elf} PRIVATE
    "-Wl,--wrap=heap_caps_free" "-Wl,--wrap=free"
    "-Wl,--wrap=heap_caps_malloc" "-Wl,--wrap=heap_caps_calloc"
    "-Wl,--wrap=heap_caps_aligned_alloc" "-Wl,--wrap=heap_caps_aligned_calloc"
    "-Wl,--wrap=heap_caps_realloc" "-Wl,--wrap=heap_caps_get_allocated_size"
    "-Wl,--wrap=heap_caps_malloc_default" "-Wl,--wrap=heap_caps_realloc_default")
include(patches/nimble/overlay.cmake)
