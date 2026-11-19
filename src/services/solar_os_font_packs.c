#include "solar_os_font_packs.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define PACK_MAGIC "SFP1"
#define PACK_RANGES_MAX 16
#define PACK_FACES_MAX 64
#define PACK_FILE_MAX_BYTES (512U * 1024U)
#define PACK_NVS_NAMESPACE "fonts"
#define PACK_NVS_KEY "packs"
#define PACK_SELECTION_MAX 80

/* Glyph drawing reads a pack's faces without a lock, so a pack that is
 * unloaded is first made invisible and freed only after drawing has had time
 * to finish the face it might be using. */
#define PACK_UNLOAD_GRACE_MS 250

static const char *TAG = "fontpack";
/* Size 8 is the 5x8 terminal face: regular only, cut from misc-fixed. */
static const uint8_t pack_sizes[SOLAR_OS_FONT_SIZE_COUNT] = {8, 10, 12, 14, 16, 18, 20};

typedef struct {
    bool active;
    char name[SOLAR_OS_FONT_PACK_NAME_MAX];
    uint32_t range[PACK_RANGES_MAX][2];
    uint8_t range_count;
    uint8_t *data;
    size_t bytes;
    const uint8_t *face[SOLAR_OS_FONT_STYLE_COUNT][SOLAR_OS_FONT_SIZE_COUNT];
    uint8_t faces_loaded;
    uint8_t faces_total;
    bool reduced;
} font_pack_t;

static font_pack_t packs[SOLAR_OS_FONT_PACK_MAX];
static SemaphoreHandle_t pack_lock;
static atomic_uint pack_generation = ATOMIC_VAR_INIT(0);

typedef struct {
    uint8_t style;
    uint8_t size;
    uint32_t offset;
    uint32_t length;
} pack_face_entry_t;

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* U8g2 face layout (u8g2_font.c): a 23 byte header, the glyphs up to 0xff
 * as <encoding, size, data> ended by a zero size, then the Unicode section:
 * a lookup table of <offset, encoding> words ended by encoding 0xffff, and
 * glyphs as <encoding hi, lo, size, data> ended by encoding 0. U8g2 trusts
 * every offset and size, so a face from a file is walked here first. */
#define FACE_HEADER_BYTES 23U

typedef struct {
    const uint8_t *face;
    size_t len;
    size_t pos;
    uint8_t bit;
    bool overrun;
} face_bits_t;

static uint16_t rd16be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* u8g2_font_decode_get_unsigned_bits(), with every byte it reads checked:
 * it touches the next byte as soon as bit + cnt reaches 8. */
static unsigned face_bits_get(face_bits_t *bits, uint8_t cnt)
{
    if (bits->pos >= bits->len) {
        bits->overrun = true;
        return 0;
    }
    unsigned val = (unsigned)bits->face[bits->pos] >> bits->bit;
    unsigned end = bits->bit + cnt;
    if (end >= 8U) {
        bits->pos++;
        if (bits->pos >= bits->len) {
            bits->overrun = true;
            return 0;
        }
        val |= (unsigned)bits->face[bits->pos] << (8U - bits->bit);
        end -= 8U;
    }
    bits->bit = (uint8_t)end;
    return val & ((1U << cnt) - 1U);
}

/* Replays u8g2_font_decode_glyph() without drawing: the glyph's bits must
 * end inside its own size, which also bounds a run-length loop that would
 * otherwise never reach the last row. U8g2 may read the byte after the last
 * bit it uses (the next glyph's header), so reads are bounded by the face. */
static bool face_glyph_ended(const face_bits_t *bits, size_t end)
{
    return !bits->overrun && (bits->pos < end || (bits->pos == end && bits->bit == 0U));
}

static bool face_glyph_valid(const uint8_t *face, size_t face_len, size_t data, size_t end)
{
    face_bits_t bits = {.face = face, .len = face_len, .pos = data};
    const uint8_t *info = face;
    const unsigned width = face_bits_get(&bits, info[4]);
    const unsigned height = face_bits_get(&bits, info[5]);
    (void)face_bits_get(&bits, info[6]);
    (void)face_bits_get(&bits, info[7]);
    (void)face_bits_get(&bits, info[8]);
    if (!face_glyph_ended(&bits, end)) {
        return false;
    }
    /* U8g2 keeps both in int8_t. */
    if (width > 127U || height > 127U) {
        return false;
    }
    if (width == 0U) {
        return true;
    }
    /* decode->x/y are int8_t, the run loop works in uint8_t. */
    int8_t x = 0;
    int8_t y = 0;
    for (;;) {
        const unsigned a = face_bits_get(&bits, info[2]);
        const unsigned b = face_bits_get(&bits, info[3]);
        unsigned repeat;
        do {
            const unsigned runs[2] = {a, b};
            for (unsigned r = 0; r < 2U; r++) {
                uint8_t cnt = (uint8_t)runs[r];
                uint8_t lx = (uint8_t)x;
                uint8_t ly = (uint8_t)y;
                for (;;) {
                    const uint8_t rem = (uint8_t)(width - lx);
                    if (cnt < rem) {
                        break;
                    }
                    cnt = (uint8_t)(cnt - rem);
                    lx = 0;
                    ly++;
                }
                lx = (uint8_t)(lx + cnt);
                x = (int8_t)lx;
                y = (int8_t)ly;
            }
            repeat = face_bits_get(&bits, 1U);
            if (!face_glyph_ended(&bits, end)) {
                return false;
            }
        } while (repeat != 0U);
        if (y >= (int8_t)height) {
            return true;
        }
    }
}

bool solar_os_font_face_valid(const uint8_t *face, size_t len)
{
    if (face == NULL || len < FACE_HEADER_BYTES + 2U || len > PACK_FILE_MAX_BYTES) {
        return false;
    }
    /* Bit counts of the glyph fields: U8g2 reads at most 8 at once, and a
     * signed field of 0 bits is undefined in its decoder. */
    for (unsigned i = 2; i <= 8U; i++) {
        if (face[i] == 0U || face[i] > 8U) {
            return false;
        }
    }

    /* Glyphs up to 0xff. The 'A' and 'a' start positions must land on one
     * of them or on the end marker. */
    const size_t start_upper = FACE_HEADER_BYTES + rd16be(face + 17);
    const size_t start_lower = FACE_HEADER_BYTES + rd16be(face + 19);
    bool upper_seen = false;
    bool lower_seen = false;
    size_t pos = FACE_HEADER_BYTES;
    for (;;) {
        if (pos + 2U > len) {
            return false;
        }
        upper_seen = upper_seen || pos == start_upper;
        lower_seen = lower_seen || pos == start_lower;
        const size_t size = face[pos + 1];
        if (size == 0U) {
            break;
        }
        if (size < 3U || pos + size > len ||
            !face_glyph_valid(face, len, pos + 2U, pos + size)) {
            return false;
        }
        pos += size;
    }
    if (!upper_seen || !lower_seen) {
        return false;
    }

    /* The Unicode section. */
    const size_t unicode = FACE_HEADER_BYTES + rd16be(face + 21);
    if (unicode + 4U > len) {
        return false;
    }
    const size_t table_bytes = rd16be(face + unicode);
    const size_t glyphs = unicode + table_bytes;
    if (table_bytes < 4U || (table_bytes % 4U) != 0U || glyphs > len) {
        return false;
    }

    /* Walk the glyph chain once, remembering where glyphs start so each
     * lookup entry can be checked against them. */
    size_t starts_max = 0;
    for (size_t p = glyphs;;) {
        if (p + 2U > len) {
            return false;
        }
        if (rd16be(face + p) == 0U) {
            break;
        }
        if (p + 3U > len) {
            return false;
        }
        const size_t size = face[p + 2];
        if (size < 4U || p + size > len || !face_glyph_valid(face, len, p + 3U, p + size)) {
            return false;
        }
        starts_max = p;
        p += size;
    }

    /* Each lookup entry moves the cursor by its offset and names the last
     * encoding of that block. The search stops at the first entry at or
     * above the wanted encoding, so the table must end with 0xffff and the
     * cursor must stay on a glyph (or the end marker) all the way. */
    size_t cursor = unicode;
    uint32_t last = 0;
    for (size_t entry = unicode;; entry += 4U) {
        if (entry + 4U > glyphs) {
            return false;
        }
        cursor += rd16be(face + entry);
        const uint16_t encoding = rd16be(face + entry + 2U);
        if (cursor < glyphs || cursor > len || encoding < last) {
            return false;
        }
        /* On a glyph boundary: walk from the chain start (faces are small). */
        bool on_glyph = false;
        for (size_t p = glyphs;;) {
            if (p == cursor) {
                on_glyph = true;
                break;
            }
            if (p > starts_max || p > cursor || rd16be(face + p) == 0U) {
                break;
            }
            p += face[p + 2];
        }
        if (!on_glyph) {
            return false;
        }
        if (encoding == 0xffffU) {
            break;
        }
        last = encoding;
    }
    return true;
}

static bool pack_lock_take(void)
{
    if (pack_lock == NULL) {
        pack_lock = xSemaphoreCreateMutex();
    }
    return pack_lock != NULL && xSemaphoreTake(pack_lock, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void pack_lock_give(void)
{
    xSemaphoreGive(pack_lock);
}

bool solar_os_font_pack_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' ||
        strlen(name) >= SOLAR_OS_FONT_PACK_NAME_MAX) {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        const bool ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                        *p == '-' || *p == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

esp_err_t solar_os_font_pack_dir(char *path, size_t path_len)
{
    return solar_os_storage_default_path("fonts", path, path_len);
}

static esp_err_t pack_file_path(const char *name, char *path, size_t path_len)
{
    char relative[SOLAR_OS_FONT_PACK_NAME_MAX + 12];
    snprintf(relative, sizeof(relative), "fonts/%s.sfp", name);
    return solar_os_storage_default_path(relative, path, path_len);
}

static font_pack_t *pack_find(const char *name)
{
    for (size_t i = 0; i < SOLAR_OS_FONT_PACK_MAX; i++) {
        if (packs[i].active && strcmp(packs[i].name, name) == 0) {
            return &packs[i];
        }
    }
    return NULL;
}

static bool pack_has_psram(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static void pack_discard(font_pack_t *pack)
{
    uint8_t *data = pack->data;
    pack->active = false;
    if (data != NULL) {
        /* Let a draw that may hold one of its faces finish first. */
        vTaskDelay(pdMS_TO_TICKS(PACK_UNLOAD_GRACE_MS));
        solar_os_memory_free(data);
    }
    memset(pack, 0, sizeof(*pack));
}

/* The order faces are taken in when memory is bounded: the regular face of
 * every size, then bold, italic and bold italic, smaller sizes first. */
static bool pack_take_in_reduced_mode(const pack_face_entry_t *entries,
                                      size_t count,
                                      bool *selected,
                                      size_t *total)
{
    size_t used = 0;
    bool all = true;
    for (unsigned style = 0; style < SOLAR_OS_FONT_STYLE_COUNT; style++) {
        for (unsigned size = 0; size < SOLAR_OS_FONT_SIZE_COUNT; size++) {
            for (size_t i = 0; i < count; i++) {
                if (entries[i].style != style || entries[i].size != pack_sizes[size]) {
                    continue;
                }
                if (used + entries[i].length <= SOLAR_OS_FONT_PACK_REDUCED_BYTES) {
                    selected[i] = true;
                    used += entries[i].length;
                } else {
                    all = false;
                }
            }
        }
    }
    *total = used;
    return all;
}

esp_err_t solar_os_font_pack_load(const char *name)
{
    if (!solar_os_font_pack_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = pack_file_path(name, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t head[8 + SOLAR_OS_FONT_PACK_NAME_MAX];
    if (fread(head, 1, sizeof(head), file) != sizeof(head) ||
        memcmp(head, PACK_MAGIC, 4) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const unsigned range_count = rd16(head + 4);
    const unsigned face_count = rd16(head + 6);
    if (range_count == 0 || range_count > PACK_RANGES_MAX || face_count == 0 ||
        face_count > PACK_FACES_MAX) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t table[PACK_RANGES_MAX * 8 + PACK_FACES_MAX * 12];
    const size_t table_len = range_count * 8U + face_count * 12U;
    if (fread(table, 1, table_len, file) != table_len) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    pack_face_entry_t entries[PACK_FACES_MAX];
    bool selected[PACK_FACES_MAX];
    size_t total = 0;
    for (unsigned i = 0; i < face_count; i++) {
        const uint8_t *e = table + range_count * 8U + i * 12U;
        entries[i].style = e[0];
        entries[i].size = e[1];
        entries[i].offset = rd32(e + 4);
        entries[i].length = rd32(e + 8);
        selected[i] = false;
        if (entries[i].style >= SOLAR_OS_FONT_STYLE_COUNT ||
            entries[i].length == 0 || entries[i].length > PACK_FILE_MAX_BYTES) {
            fclose(file);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    const bool psram = pack_has_psram();
    bool reduced = false;
    if (psram) {
        for (unsigned i = 0; i < face_count; i++) {
            selected[i] = true;
            total += entries[i].length;
        }
    } else {
        reduced = !pack_take_in_reduced_mode(entries, face_count, selected, &total);
    }
    if (total == 0 || total > PACK_FILE_MAX_BYTES) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *data = solar_os_memory_alloc(total,
                                          psram ? SOLAR_OS_MEMORY_EXTERNAL_REQUIRED
                                                : SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
                                          "fontpack");
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    font_pack_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    strlcpy(fresh.name, name, sizeof(fresh.name));
    fresh.range_count = (uint8_t)range_count;
    for (unsigned i = 0; i < range_count; i++) {
        fresh.range[i][0] = rd32(table + i * 8U);
        fresh.range[i][1] = rd32(table + i * 8U + 4U);
        if (fresh.range[i][0] > fresh.range[i][1]) {
            fclose(file);
            solar_os_memory_free(data);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    fresh.data = data;
    fresh.bytes = total;
    fresh.faces_total = (uint8_t)face_count;
    fresh.reduced = reduced;

    size_t cursor = 0;
    err = ESP_OK;
    for (unsigned i = 0; i < face_count && err == ESP_OK; i++) {
        if (!selected[i]) {
            continue;
        }
        if (fseek(file, (long)entries[i].offset, SEEK_SET) != 0 ||
            fread(data + cursor, 1, entries[i].length, file) != entries[i].length) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        /* U8g2 follows a face's offsets unchecked: check them against the
         * face's own length before any of it can be drawn. */
        if (!solar_os_font_face_valid(data + cursor, entries[i].length)) {
            SOLAR_OS_LOGW(TAG, "%s: face %u is malformed", name, i);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        for (unsigned size = 0; size < SOLAR_OS_FONT_SIZE_COUNT; size++) {
            if (entries[i].size == pack_sizes[size]) {
                fresh.face[entries[i].style][size] = data + cursor;
                fresh.faces_loaded++;
            }
        }
        cursor += entries[i].length;
    }
    fclose(file);
    if (err != ESP_OK) {
        solar_os_memory_free(data);
        return err;
    }

    if (!pack_lock_take()) {
        solar_os_memory_free(data);
        return ESP_ERR_TIMEOUT;
    }
    font_pack_t *slot = pack_find(name);
    font_pack_t old;
    bool replaced = false;
    if (slot != NULL) {
        old = *slot;
        replaced = true;
    } else {
        for (size_t i = 0; i < SOLAR_OS_FONT_PACK_MAX; i++) {
            if (!packs[i].active) {
                slot = &packs[i];
                break;
            }
        }
    }
    if (slot == NULL) {
        pack_lock_give();
        solar_os_memory_free(data);
        return ESP_ERR_NO_MEM;
    }
    fresh.active = true;
    *slot = fresh;
    atomic_fetch_add_explicit(&pack_generation, 1U, memory_order_release);
    pack_lock_give();

    if (replaced) {
        vTaskDelay(pdMS_TO_TICKS(PACK_UNLOAD_GRACE_MS));
        solar_os_memory_free(old.data);
    }
    SOLAR_OS_LOGI(TAG,
                  "loaded %s: %u of %u faces, %u bytes%s",
                  name,
                  (unsigned)fresh.faces_loaded,
                  (unsigned)fresh.faces_total,
                  (unsigned)total,
                  reduced ? " (reduced)" : "");
    return ESP_OK;
}

esp_err_t solar_os_font_pack_unload(const char *name)
{
    if (name == NULL || !pack_lock_take()) {
        return ESP_ERR_INVALID_ARG;
    }
    font_pack_t *pack = pack_find(name);
    if (pack == NULL) {
        pack_lock_give();
        return ESP_ERR_NOT_FOUND;
    }
    pack_discard(pack);
    atomic_fetch_add_explicit(&pack_generation, 1U, memory_order_release);
    pack_lock_give();
    return ESP_OK;
}

uint32_t solar_os_font_pack_generation(void)
{
    return atomic_load_explicit(&pack_generation, memory_order_acquire);
}

size_t solar_os_font_pack_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_FONT_PACK_MAX; i++) {
        if (packs[i].active) {
            count++;
        }
    }
    return count;
}

bool solar_os_font_pack_get(size_t index, solar_os_font_pack_info_t *info)
{
    size_t seen = 0;
    for (size_t i = 0; info != NULL && i < SOLAR_OS_FONT_PACK_MAX; i++) {
        if (!packs[i].active) {
            continue;
        }
        if (seen++ == index) {
            strlcpy(info->name, packs[i].name, sizeof(info->name));
            info->bytes = packs[i].bytes;
            info->faces_loaded = packs[i].faces_loaded;
            info->faces_total = packs[i].faces_total;
            info->reduced = packs[i].reduced;
            return true;
        }
    }
    return false;
}

const uint8_t *solar_os_font_pack_face(unsigned style, unsigned size_index, uint32_t codepoint)
{
    if (style >= SOLAR_OS_FONT_STYLE_COUNT || size_index >= SOLAR_OS_FONT_SIZE_COUNT) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_FONT_PACK_MAX; i++) {
        const font_pack_t *pack = &packs[i];
        if (!pack->active || pack->face[style][size_index] == NULL) {
            continue;
        }
        for (unsigned r = 0; r < pack->range_count; r++) {
            if (codepoint >= pack->range[r][0] && codepoint <= pack->range[r][1]) {
                return pack->face[style][size_index];
            }
        }
    }
    return NULL;
}

esp_err_t solar_os_font_pack_save_selection(void)
{
    char selection[PACK_SELECTION_MAX] = "";
    for (size_t i = 0; i < SOLAR_OS_FONT_PACK_MAX; i++) {
        if (!packs[i].active) {
            continue;
        }
        if (strlen(selection) + strlen(packs[i].name) + 2 > sizeof(selection)) {
            return ESP_ERR_NO_MEM;
        }
        if (selection[0] != '\0') {
            strlcat(selection, ",", sizeof(selection));
        }
        strlcat(selection, packs[i].name, sizeof(selection));
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PACK_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, PACK_NVS_KEY, selection);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t solar_os_font_pack_saved_selection(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PACK_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t length = out_len;
    err = nvs_get_str(nvs, PACK_NVS_KEY, out, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

void solar_os_font_pack_restore_selection(void)
{
    char selection[PACK_SELECTION_MAX];
    if (solar_os_font_pack_saved_selection(selection, sizeof(selection)) != ESP_OK) {
        return;
    }
    for (char *cursor = selection; *cursor != '\0';) {
        char *end = strchr(cursor, ',');
        if (end != NULL) {
            *end = '\0';
        }
        if (cursor[0] != '\0') {
            const esp_err_t err = solar_os_font_pack_load(cursor);
            if (err != ESP_OK) {
                /* The selection stays saved: the volume may appear later. */
                SOLAR_OS_LOGW(TAG, "font pack %s not restored: %s", cursor, esp_err_to_name(err));
            }
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
}

esp_err_t solar_os_font_pack_list_files(void (*fn)(const char *name, void *ctx), void *ctx)
{
    char dir_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_font_pack_dir(dir_path, sizeof(dir_path));
    if (err != ESP_OK) {
        return err;
    }
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".sfp") != 0) {
            continue;
        }
        char name[SOLAR_OS_FONT_PACK_NAME_MAX];
        if (length - 4 >= sizeof(name)) {
            continue;
        }
        memcpy(name, entry->d_name, length - 4);
        name[length - 4] = '\0';
        if (solar_os_font_pack_name_valid(name) && fn != NULL) {
            fn(name, ctx);
        }
    }
    closedir(dir);
    return ESP_OK;
}
