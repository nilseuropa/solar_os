#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded UTF-8 decoding. NUL ends a span; malformed/truncated sequences
 * consume one byte and yield '?'. No byte past length is inspected. */
static inline size_t solar_os_ui_utf8_decode_n(const char *text, size_t length,
    uint32_t *codepoint)
{
    const unsigned char *p = (const unsigned char *)text;
    if (!p || !length || !codepoint || !p[0]) return 0;
    if (p[0] < 0x80U) { *codepoint = p[0]; return 1; }
    size_t count = p[0] >= 0xc2U && p[0] <= 0xdfU ? 2 :
        p[0] >= 0xe0U && p[0] <= 0xefU ? 3 :
        p[0] >= 0xf0U && p[0] <= 0xf4U ? 4 : 0;
    bool valid = count && count <= length;
    for (size_t i = 1; valid && i < count; i++) valid = (p[i] & 0xc0U) == 0x80U;
    if (valid && count >= 3) valid =
        (p[0] != 0xe0U || p[1] >= 0xa0U) && (p[0] != 0xedU || p[1] < 0xa0U) &&
        (p[0] != 0xf0U || p[1] >= 0x90U) && (p[0] != 0xf4U || p[1] < 0x90U);
    if (!valid) { *codepoint = '?'; return 1; }
    uint32_t value = p[0] & (count == 2 ? 0x1fU : count == 3 ? 0x0fU : 0x07U);
    for (size_t i = 1; i < count; i++) value = (value << 6) | (p[i] & 0x3fU);
    *codepoint = value;
    return count;
}

static inline size_t solar_os_ui_utf8_decode(const char *text, uint32_t *codepoint)
{
    if (!text || !codepoint || !*text) return 0;
    const unsigned char lead = (unsigned char)*text;
    const size_t needed = lead >= 0xc2U && lead <= 0xdfU ? 2 :
        lead >= 0xe0U && lead <= 0xefU ? 3 : lead >= 0xf0U && lead <= 0xf4U ? 4 : 1;
    size_t length = 1;
    while (length < needed && text[length]) length++;
    return solar_os_ui_utf8_decode_n(text, length, codepoint);
}

static inline size_t solar_os_ui_utf8_encode(uint32_t codepoint, char out[4])
{
    if (!out || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return 0;
    if (codepoint <= 0x7fU) { out[0] = (char)codepoint; return 1; }
    const size_t count = codepoint <= 0x7ffU ? 2 : codepoint <= 0xffffU ? 3 : 4;
    out[0] = (char)((count == 2 ? 0xc0U : count == 3 ? 0xe0U : 0xf0U) |
        (codepoint >> (6 * (count - 1))));
    for (size_t i = 1; i < count; i++)
        out[i] = (char)(0x80U | ((codepoint >> (6 * (count - i - 1))) & 0x3fU));
    return count;
}

static inline size_t solar_os_ui_utf8_width(const char *text)
{
    size_t width = 0;
    while (text && *text) {
        uint32_t codepoint;
        const size_t consumed = solar_os_ui_utf8_decode(text, &codepoint);
        if (!consumed) break;
        text += consumed;
        width++;
    }
    return width;
}

static inline size_t solar_os_ui_utf8_previous(const char *text, size_t offset)
{
    if (!text || !offset) return 0;
    offset--;
    while (offset && (((unsigned char)text[offset] & 0xc0U) == 0x80U)) offset--;
    return offset;
}

static inline size_t solar_os_ui_utf8_next(const char *text, size_t offset)
{
    if (!text || !text[offset]) return offset;
    uint32_t codepoint;
    return offset + solar_os_ui_utf8_decode(text + offset, &codepoint);
}
