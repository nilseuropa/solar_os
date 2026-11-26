#include "solar_os_fonts.h"

#include <stddef.h>

#include "solar_os_font_packs.h"

typedef struct {
    const uint8_t *base;
    uint8_t style;
    uint8_t size_index;
} font_face_t;

#define FACES_FOR_SIZE(size, index)                                  \
    {u8g2_font_solar_os_default_r_##size##_tf, 0, index},            \
    {u8g2_font_solar_os_default_b_##size##_tf, 1, index},            \
    {u8g2_font_solar_os_default_i_##size##_tf, 2, index},            \
    {u8g2_font_solar_os_default_bi_##size##_tf, 3, index}

static const font_face_t font_faces[] = {
    {u8g2_font_5x8_tf, 0, 0},
    FACES_FOR_SIZE(10, 1), FACES_FOR_SIZE(12, 2), FACES_FOR_SIZE(14, 3),
    FACES_FOR_SIZE(16, 4), FACES_FOR_SIZE(18, 5), FACES_FOR_SIZE(20, 6),
};

bool solar_os_font_fallback(const uint8_t *font, uint32_t codepoint, solar_os_font_face_ref_t *ref)
{
    if (font == NULL || solar_os_font_pack_count() == 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(font_faces) / sizeof(font_faces[0]); i++) {
        if (font_faces[i].base == font) {
            return solar_os_font_pack_face(font_faces[i].style,
                                           font_faces[i].size_index,
                                           codepoint, ref);
        }
    }
    return false;
}

uint32_t solar_os_utf8_next(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    if (p == NULL || *p == 0) {
        return 0;
    }
    const uint8_t lead = *p++;
    uint32_t codepoint = lead;
    int extra = 0;
    if (lead >= 0xf0 && lead < 0xf8) {
        codepoint = lead & 0x07;
        extra = 3;
    } else if (lead >= 0xe0) {
        codepoint = lead & 0x0f;
        extra = lead < 0xf0 ? 2 : -1;
    } else if (lead >= 0xc0) {
        codepoint = lead & 0x1f;
        extra = 1;
    } else if (lead >= 0x80) {
        extra = -1;
    }
    if (extra < 0) {
        *text = (const char *)p;
        return 0xfffd;
    }
    while (extra-- > 0) {
        if ((*p & 0xc0) != 0x80) {
            *text = (const char *)p;
            return 0xfffd;
        }
        codepoint = (codepoint << 6) | (*p++ & 0x3f);
    }
    *text = (const char *)p;
    return codepoint;
}
