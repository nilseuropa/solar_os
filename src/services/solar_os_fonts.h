#pragma once

#include <stdint.h>

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

/* The loaded font-pack face that goes with a built-in face and has the glyph
 * for a code point, or NULL (no pack loaded, the 5x8 and icon faces, glyph not
 * in any pack). See solar_os_font_packs.h. */
const uint8_t *solar_os_font_fallback(const uint8_t *font, uint32_t codepoint);

/* Next code point of a UTF-8 string, advancing *text; U+FFFD for a bad
 * sequence and 0 at the end of the string. */
uint32_t solar_os_utf8_next(const char **text);
