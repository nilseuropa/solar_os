#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Public-domain fixed bitmap face; terminal uses a 6x8 cell. */
extern const uint8_t u8g2_font_5x8_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_10_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_10_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_10_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_10_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_12_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_14_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_16_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_18_tf[];
extern const uint8_t u8g2_font_solar_os_default_r_20_tf[];
extern const uint8_t u8g2_font_solar_os_default_b_20_tf[];
extern const uint8_t u8g2_font_solar_os_default_i_20_tf[];
extern const uint8_t u8g2_font_solar_os_default_bi_20_tf[];

typedef struct {
    const uint8_t *face;
    struct solar_os_font_pack_data *pack;
} solar_os_font_face_ref_t;

/* Retain the matching pack face for a glyph in an empty reference. Restore
 * the built-in face after drawing, then release the retained reference once. */
bool solar_os_font_fallback(const uint8_t *font, uint32_t codepoint, solar_os_font_face_ref_t *ref);
void solar_os_font_face_release(solar_os_font_face_ref_t *ref);

/* Next code point of a UTF-8 string, advancing *text; U+FFFD for a bad
 * sequence and 0 at the end of the string. */
uint32_t solar_os_utf8_next(const char **text);
