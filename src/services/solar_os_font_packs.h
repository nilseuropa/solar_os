#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_fonts.h"
#include "solar_os_native_font_packs_abi.h"

/* Font packs hold the glyphs a built-in face lacks (Cyrillic, Latin Extended,
 * ...) for every built-in style and size. A pack is the file
 * <default volume>/fonts/<name>.sfp written by fonts/generate_u8g2_fonts.py
 * --pack. It is loaded explicitly (`setterm font load <name>`), the selection
 * is saved, and it is restored once storage is mounted at boot. Text drawing
 * asks solar_os_font_fallback() for a glyph the built-in face does not have.
 *
 * With PSRAM a pack loads whole. Without it only a bounded set of faces loads
 * (regular and bold at the smaller sizes) so the face data never takes more than
 * SOLAR_OS_FONT_PACK_REDUCED_BYTES of internal RAM. */

#define SOLAR_OS_FONT_PACK_REDUCED_BYTES (24U * 1024U)

#define SOLAR_OS_FONT_STYLE_COUNT 4
#define SOLAR_OS_FONT_SIZE_COUNT 7

/* Load /fonts/<name>.sfp from the default storage volume. Loading a pack that
 * is already loaded reloads it. Does not change the saved selection. */
esp_err_t solar_os_font_pack_load(const char *name);
esp_err_t solar_os_font_pack_unload(const char *name);

/* Changes on every successful load, reload or unload. Renderers can compare
 * this across tasks to invalidate cached text when the available faces change. */
uint32_t solar_os_font_pack_generation(void);

size_t solar_os_font_pack_count(void);
bool solar_os_font_pack_get(size_t index, solar_os_font_pack_info_t *info);

/* Saved selection (NVS): the loaded packs, restored at boot. */
esp_err_t solar_os_font_pack_save_selection(void);
void solar_os_font_pack_restore_selection(void);
/* Comma separated names of the saved selection; "" when none. */
esp_err_t solar_os_font_pack_saved_selection(char *out, size_t out_len);

/* Call fn for each *.sfp in the fonts directory of the default volume. */
esp_err_t solar_os_font_pack_list_files(solar_os_font_pack_visit_fn fn, void *ctx);
esp_err_t solar_os_font_pack_dir(char *path, size_t path_len);

bool solar_os_font_pack_name_valid(const char *name);

/* True when every offset, size and glyph bit stream in the U8g2 face lies
 * within its len bytes, so U8g2 can draw any glyph of it safely. Each face
 * of a pack is checked before the pack is loaded. */
bool solar_os_font_face_valid(const uint8_t *face, size_t len);

/* The pack face for a style (0 r, 1 b, 2 i, 3 bi) and size index (0 for the
 * regular-only 5x8 face, 1..6 for sizes 10..20). The reference holds the
 * immutable glyph data through reload and unload. */
bool solar_os_font_pack_face(unsigned style, unsigned size_index, uint32_t codepoint,
                             solar_os_font_face_ref_t *ref);
