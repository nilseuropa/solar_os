set(U8G2_CLIB_SRCS
    "src/clib/u8g2_box.c"
    "src/clib/u8g2_buffer.c"
    "src/clib/u8g2_font.c"
    "src/clib/u8g2_hvline.c"
    "src/clib/u8g2_intersection.c"
    "src/clib/u8g2_kerning.c"
    "src/clib/u8g2_ll_hvline.c"
    "src/clib/u8g2_message.c"
    "src/clib/u8g2_setup.c"
    "src/clib/u8x8_display.c"
    "src/clib/u8x8_8x8.c"
    "src/clib/u8x8_byte.c"
    "src/clib/u8x8_cad.c"
    "src/clib/u8x8_d_pcd8544_84x48.c"
    "src/clib/u8x8_gpio.c"
    "src/clib/u8x8_message.c"
    "src/clib/u8x8_setup.c"
    "src/clib/u8x8_string.c"
)

set(SOLAR_OS_GENERATED_FONT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../fonts/build/u8g2")
file(GLOB SOLAR_OS_GENERATED_FONT_SRCS
    "${SOLAR_OS_GENERATED_FONT_DIR}/u8g2_font_solar_os_default_*_tf.c"
)
if(SOLAR_OS_GENERATED_FONT_SRCS)
    list(SORT SOLAR_OS_GENERATED_FONT_SRCS)
    list(APPEND U8G2_CLIB_SRCS ${SOLAR_OS_GENERATED_FONT_SRCS})
endif()

idf_component_register(
    SRCS ${U8G2_CLIB_SRCS}
    INCLUDE_DIRS "src/clib"
)
