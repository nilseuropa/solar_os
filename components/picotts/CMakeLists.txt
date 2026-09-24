if(NOT SOLAR_OS_PICOTTS_SOURCE_DIR AND
        DEFINED ENV{SOLAR_OS_PICOTTS_SOURCE_DIR})
    set(SOLAR_OS_PICOTTS_SOURCE_DIR "$ENV{SOLAR_OS_PICOTTS_SOURCE_DIR}")
endif()

if(NOT SOLAR_OS_PICOTTS_SOURCE_DIR OR
        NOT EXISTS "${SOLAR_OS_PICOTTS_SOURCE_DIR}/pico/lib/picoapi.c")
    message(FATAL_ERROR "Pinned PicoTTS source is unavailable")
endif()

if(NOT SOLAR_OS_PICOTTS_VOICE_DIR AND
        DEFINED ENV{SOLAR_OS_PICOTTS_VOICE_DIR})
    set(SOLAR_OS_PICOTTS_VOICE_DIR "$ENV{SOLAR_OS_PICOTTS_VOICE_DIR}")
endif()

if(NOT SOLAR_OS_PICOTTS_VOICE_DIR OR
        NOT EXISTS "${SOLAR_OS_PICOTTS_VOICE_DIR}/README.md")
    message(FATAL_ERROR "Versioned PicoTTS voice directory is unavailable")
endif()

set(PICOTTS_CORE_SRCS
    picopr.c
    picosa.c
    picokpr.c
    picodata.c
    picokfst.c
    picodbg.c
    picokdt.c
    picoctrl.c
    picoktab.c
    picoacph.c
    picokdbg.c
    picoos.c
    picospho.c
    picotok.c
    picosig.c
    picocep.c
    picofftsg.c
    picotrns.c
    picosig2.c
    picoextapi.c
    picopal.c
    picokpdf.c
    picobase.c
    picopam.c
    picowa.c
    picoklex.c
    picoapi.c
    picoknow.c
)
list(TRANSFORM PICOTTS_CORE_SRCS
     PREPEND "${SOLAR_OS_PICOTTS_SOURCE_DIR}/pico/lib/")

idf_component_register(
    SRCS
        "picotts_runtime.c"
        "picotts_resource.c"
        ${PICOTTS_CORE_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "${SOLAR_OS_PICOTTS_SOURCE_DIR}"
        "${SOLAR_OS_PICOTTS_SOURCE_DIR}/pico/lib"
)

set_source_files_properties(
    ${PICOTTS_CORE_SRCS}
    PROPERTIES COMPILE_FLAGS
    "-Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-function"
)
set_source_files_properties(
    "${SOLAR_OS_PICOTTS_SOURCE_DIR}/pico/lib/picoos.c"
    PROPERTIES COMPILE_OPTIONS "-Dpicoos_quick_exp=picoos_quick_nope"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # One complete 512-byte request, pitch/speed markup, and the terminator.
    PICOTTS_INPUT_QUEUE_SIZE=576
)

# Pico's signal-generation pipeline must stay comfortably ahead of real-time
# playback even while the display and network stacks are active. The firmware
# default optimizes for size; use the balanced performance profile only for
# this CPU-bound component.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

set(PICOTTS_VOICE_OUTPUT "${CMAKE_BINARY_DIR}/picotts_voices")
function(solar_os_picotts_voice locale)
    set(voice_dir "${PICOTTS_VOICE_OUTPUT}/${locale}")
    file(MAKE_DIRECTORY "${voice_dir}")
    foreach(resource ta.bin sg.bin)
        set(source "${SOLAR_OS_PICOTTS_VOICE_DIR}/${locale}/${resource}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR
                "Missing PicoTTS voice resource: ${source}"
            )
        endif()
        configure_file("${source}" "${voice_dir}/${resource}" COPYONLY)
    endforeach()
endfunction()

solar_os_picotts_voice(en-GB)
solar_os_picotts_voice(en-US)
solar_os_picotts_voice(de-DE)
solar_os_picotts_voice(es-ES)
solar_os_picotts_voice(fr-FR)
solar_os_picotts_voice(it-IT)
