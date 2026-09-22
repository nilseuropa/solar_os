if(NOT SOLAR_OS_PICOTTS_SOURCE_DIR AND
        DEFINED ENV{SOLAR_OS_PICOTTS_SOURCE_DIR})
    set(SOLAR_OS_PICOTTS_SOURCE_DIR "$ENV{SOLAR_OS_PICOTTS_SOURCE_DIR}")
endif()

if(NOT SOLAR_OS_PICOTTS_SOURCE_DIR OR
        NOT EXISTS "${SOLAR_OS_PICOTTS_SOURCE_DIR}/pico/lib/picoapi.c")
    message(FATAL_ERROR "Pinned PicoTTS source is unavailable")
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
    PICOTTS_INPUT_QUEUE_SIZE=256
)

set(PICOTTS_VOICE_OUTPUT "${CMAKE_BINARY_DIR}/picotts_voices")
function(solar_os_picotts_voice locale ta_file sg_file)
    set(voice_dir "${PICOTTS_VOICE_OUTPUT}/${locale}")
    file(MAKE_DIRECTORY "${voice_dir}")
    configure_file(
        "${SOLAR_OS_PICOTTS_SOURCE_DIR}/pico/lang/${ta_file}"
        "${voice_dir}/ta.bin"
        COPYONLY
    )
    configure_file(
        "${SOLAR_OS_PICOTTS_SOURCE_DIR}/pico/lang/${sg_file}"
        "${voice_dir}/sg.bin"
        COPYONLY
    )
endfunction()

solar_os_picotts_voice(en-GB en-GB_ta.bin en-GB_kh0_sg.bin)
solar_os_picotts_voice(en-US en-US_ta.bin en-US_lh0_sg.bin)
solar_os_picotts_voice(de-DE de-DE_ta.bin de-DE_gl0_sg.bin)
solar_os_picotts_voice(es-ES es-ES_ta.bin es-ES_zl0_sg.bin)
solar_os_picotts_voice(fr-FR fr-FR_ta.bin fr-FR_nk0_sg.bin)
solar_os_picotts_voice(it-IT it-IT_ta.bin it-IT_cm0_sg.bin)
