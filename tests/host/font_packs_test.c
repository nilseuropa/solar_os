#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_font_packs.h"
#include "solar_os_fonts.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_native_font_packs.h"
#include "solar_os_storage.h"

/* Built-in faces: solar_os_font_fallback() only compares their addresses. */
#define FACE(name) const uint8_t name[1];
FACE(u8g2_font_5x8_tf)
#define FACES(size)                               \
    FACE(u8g2_font_solar_os_default_r_##size##_tf) \
    FACE(u8g2_font_solar_os_default_b_##size##_tf) \
    FACE(u8g2_font_solar_os_default_i_##size##_tf) \
    FACE(u8g2_font_solar_os_default_bi_##size##_tf)
FACES(10) FACES(12) FACES(14) FACES(16) FACES(18) FACES(20)

static char volume[256];
static size_t psram_bytes;
static size_t live_allocs;
static char nvs_value[128];
static int nvs_has_value;

static bool has_fallback(const uint8_t *font, uint32_t codepoint)
{
    solar_os_font_face_ref_t ref = {0};
    bool found = solar_os_font_fallback(font, codepoint, &ref);
    if (found) solar_os_font_face_release(&ref);
    return found;
}

size_t heap_caps_get_total_size(uint32_t caps)
{
    return caps == MALLOC_CAP_SPIRAM ? psram_bytes : 0U;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t solar_os_storage_default_path(const char *relative, char *path, size_t path_len)
{
    int n = snprintf(path, path_len, "%s/%s", volume, relative);
    return n > 0 && (size_t)n < path_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t memory_class, const char *tag)
{
    (void)tag;
    /* Without PSRAM a pack must not ask for external memory. */
    assert(psram_bytes > 0U || memory_class != SOLAR_OS_MEMORY_EXTERNAL_REQUIRED);
    live_allocs++;
    return malloc(size);
}

void solar_os_memory_free(void *ptr)
{
    if (ptr != NULL) {
        live_allocs--;
    }
    free(ptr);
}

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)level;
    (void)tag;
    (void)fmt;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "err";
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "fonts") == 0);
    if (mode == NVS_READONLY && !nvs_has_value) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *handle = 1U;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *length)
{
    (void)handle;
    assert(strcmp(key, "packs") == 0);
    if (strlen(nvs_value) + 1U > *length) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(value, nvs_value);
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle;
    assert(strcmp(key, "packs") == 0);
    snprintf(nvs_value, sizeof(nvs_value), "%s", value);
    nvs_has_value = 1;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

static void copy_pack(const char *name)
{
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/%s.sfp", PACK_DIR, name);
    snprintf(dst, sizeof(dst), "%s/fonts/%s.sfp", volume, name);
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    assert(in != NULL && out != NULL);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        assert(fwrite(buf, 1, n, out) == n);
    }
    fclose(in);
    fclose(out);
}

static void write_file(const char *name, const void *data, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/fonts/%s", volume, name);
    FILE *out = fopen(path, "wb");
    assert(out != NULL);
    assert(fwrite(data, 1, len, out) == len);
    fclose(out);
}

static void unload_all(void)
{
    solar_os_font_pack_info_t info;
    while (solar_os_font_pack_get(0, &info)) {
        assert(solar_os_font_pack_unload(info.name) == ESP_OK);
    }
    assert(live_allocs == 0U);
}

static void test_names(void)
{
    assert(solar_os_font_pack_name_valid("cyrillic"));
    assert(solar_os_font_pack_name_valid("latin-ext"));
    assert(!solar_os_font_pack_name_valid(""));
    assert(!solar_os_font_pack_name_valid("../cyrillic"));
    assert(!solar_os_font_pack_name_valid("Cyrillic"));
    assert(!solar_os_font_pack_name_valid("a-name-that-is-too-long"));
    assert(solar_os_font_pack_load("../x") == ESP_ERR_INVALID_ARG);
    assert(solar_os_font_pack_load("missing") == ESP_ERR_NOT_FOUND);
}

static void test_bad_files(void)
{
    write_file("bad.sfp", "SFP0\1\0\1\0", 8U);
    assert(solar_os_font_pack_load("bad") == ESP_ERR_INVALID_RESPONSE);
    write_file("short.sfp", "SFP1", 4U);
    assert(solar_os_font_pack_load("short") == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_font_pack_count() == 0U);
    assert(live_allocs == 0U);
}

/* The first face of a pack file, as written by generate_u8g2_fonts.py. */
static uint8_t *read_first_face(const char *pack, size_t *len, size_t *file_offset)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.sfp", PACK_DIR, pack);
    FILE *in = fopen(path, "rb");
    assert(in != NULL);
    uint8_t head[8 + SOLAR_OS_FONT_PACK_NAME_MAX];
    assert(fread(head, 1, sizeof(head), in) == sizeof(head));
    const unsigned ranges = head[4] | (head[5] << 8);
    assert(fseek(in, (long)(sizeof(head) + ranges * 8U), SEEK_SET) == 0);
    uint8_t entry[12];
    assert(fread(entry, 1, sizeof(entry), in) == sizeof(entry));
    const uint32_t offset = entry[4] | (entry[5] << 8) | (entry[6] << 16) | ((uint32_t)entry[7] << 24);
    *len = entry[8] | (entry[9] << 8) | (entry[10] << 16) | ((uint32_t)entry[11] << 24);
    uint8_t *face = malloc(*len);
    assert(face != NULL && fseek(in, (long)offset, SEEK_SET) == 0);
    assert(fread(face, 1, *len, in) == *len);
    fclose(in);
    *file_offset = offset;
    return face;
}

static uint8_t *face_copy(const uint8_t *face, size_t len)
{
    uint8_t *copy = malloc(len);
    assert(copy != NULL);
    memcpy(copy, face, len);
    return copy;
}

static size_t face_unicode(const uint8_t *face)
{
    return 23U + ((size_t)face[21] << 8 | face[22]);
}

static void test_face_validation(void)
{
    size_t len = 0, offset = 0;
    uint8_t *face = read_first_face("cyrillic", &len, &offset);
    assert(solar_os_font_face_valid(face, len));
    assert(!solar_os_font_face_valid(NULL, len));
    assert(!solar_os_font_face_valid(face, 10U));

    /* Cut short: the glyph chain runs past the end. */
    assert(!solar_os_font_face_valid(face, len - 3U));

    const size_t unicode = face_unicode(face);
    const size_t table = (size_t)face[unicode] << 8 | face[unicode + 1U];
    const size_t first_glyph = unicode + table;
    uint8_t *bad;

    /* Unicode section start beyond the face. */
    bad = face_copy(face, len);
    bad[21] = 0xff;
    bad[22] = 0xff;
    assert(!solar_os_font_face_valid(bad, len));
    free(bad);

    /* 'A' start position off the glyph table. */
    bad = face_copy(face, len);
    bad[18] = (uint8_t)(bad[18] + 1U);
    assert(!solar_os_font_face_valid(bad, len));
    free(bad);

    /* A run length of zero bits. */
    bad = face_copy(face, len);
    bad[2] = 0;
    assert(!solar_os_font_face_valid(bad, len));
    free(bad);

    /* Lookup table: an offset that leaves the glyphs, and a table that
     * never ends with 0xffff. */
    bad = face_copy(face, len);
    bad[unicode] = 0xff;
    assert(!solar_os_font_face_valid(bad, len));
    free(bad);
    bad = face_copy(face, len);
    for (size_t e = unicode; e + 4U <= first_glyph; e += 4U) {
        if (bad[e + 2U] == 0xff && bad[e + 3U] == 0xff) {
            bad[e + 3U] = 0xfe;
        }
    }
    assert(!solar_os_font_face_valid(bad, len));
    free(bad);

    /* A glyph whose size runs past the end, or is too small for its own
     * bits. */
    bad = face_copy(face, len);
    bad[first_glyph + 2U] = 0xff;
    assert(!solar_os_font_face_valid(bad, len));
    free(bad);
    bad = face_copy(face, len);
    bad[first_glyph + 2U] = 4U;
    assert(!solar_os_font_face_valid(bad, len));
    free(bad);

    /* Every face of both packs passes (the loads below check that), and a
     * pack with a broken face does not load at all. */
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/cyrillic.sfp", PACK_DIR);
    snprintf(dst, sizeof(dst), "%s/fonts/broken.sfp", volume);
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    assert(in != NULL && out != NULL);
    int c;
    for (size_t i = 0; (c = fgetc(in)) != EOF; i++) {
        if (i == offset + first_glyph + 2U) {
            c = 0xff;
        }
        fputc(c, out);
    }
    fclose(in);
    fclose(out);
    psram_bytes = 8U * 1024U * 1024U;
    assert(solar_os_font_pack_load("broken") == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_font_pack_count() == 0U);
    assert(live_allocs == 0U);
    free(face);
}

static void test_full_load_with_psram(void)
{
    psram_bytes = 8U * 1024U * 1024U;
    assert(solar_os_font_pack_load("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_count() == 1U);
    solar_os_font_pack_info_t info;
    assert(solar_os_font_pack_get(0, &info));
    assert(strcmp(info.name, "cyrillic") == 0);
    assert(info.faces_total == 25U && info.faces_loaded == 25U && !info.reduced);

    /* Every built-in face, including the 5x8 terminal face, gets a fallback
     * for Cyrillic and none for what the pack does not cover. */
    assert(has_fallback(u8g2_font_5x8_tf, 0x043f));
    assert(has_fallback(u8g2_font_solar_os_default_r_10_tf, 0x0490));
    assert(has_fallback(u8g2_font_solar_os_default_bi_20_tf, 0x0401));
    assert(!has_fallback(u8g2_font_solar_os_default_r_14_tf, 0x0400));
    assert(!has_fallback(u8g2_font_solar_os_default_r_14_tf, 0x0105));
    assert(!has_fallback(u8g2_font_solar_os_default_r_14_tf, 'A'));
    /* Faces differ per size and style. */
    solar_os_font_face_ref_t r8 = {0}, r10 = {0}, b10 = {0};
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f, &r8));
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_r_10_tf, 0x043f, &r10));
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_b_10_tf, 0x043f, &b10));
    assert(r8.face != r10.face && r10.face != b10.face);
    /* A U8g2 face header: glyph count, then bounding box height 8 at size 8. */
    assert(r8.face[0] > 0U && r8.face[10] == 8U);
    solar_os_font_face_release(&r8);
    solar_os_font_face_release(&r10);
    solar_os_font_face_release(&b10);

    /* A second pack covers its own ranges; loading the same pack again
     * replaces it instead of taking another slot. */
    assert(solar_os_font_pack_load("latin-ext") == ESP_OK);
    assert(solar_os_font_pack_load("latin-ext") == ESP_OK);
    assert(solar_os_font_pack_count() == 2U);
    assert(live_allocs == 2U);
    assert(has_fallback(u8g2_font_5x8_tf, 0x20ac));
    assert(has_fallback(u8g2_font_solar_os_default_i_16_tf, 0x0219));

    assert(solar_os_font_pack_unload("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_unload("cyrillic") == ESP_ERR_NOT_FOUND);
    assert(!has_fallback(u8g2_font_5x8_tf, 0x043f));
    unload_all();
    assert(!has_fallback(u8g2_font_5x8_tf, 0x20ac));
}

static void test_retained_faces_survive_reload_and_unload(void)
{
    psram_bytes = 8U * 1024U * 1024U;
    solar_os_font_face_ref_t old = {0}, current = {0};
    assert(solar_os_font_pack_load("cyrillic") == ESP_OK);
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f, &old));
    assert(!solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f, &old));
    assert(solar_os_font_pack_load("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_count() == 1 && live_allocs == 2);
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f, &current));
    assert(current.face != old.face && !memcmp(current.face, old.face, 23));
    assert(solar_os_font_pack_unload("cyrillic") == ESP_OK);
    assert(!solar_os_font_pack_count() && live_allocs == 2);
    assert(!has_fallback(u8g2_font_5x8_tf, 0x043f));
    assert(current.face[10] == 8 && old.face[10] == 8);
    solar_os_font_face_release(&old);
    assert(!old.face && !old.pack && live_allocs == 1);
    solar_os_font_face_release(&current);
    assert(!live_allocs);
}

static void test_reduced_load_without_psram(void)
{
    psram_bytes = 0U;
    assert(solar_os_font_pack_load("latin-ext") == ESP_OK);
    solar_os_font_pack_info_t info;
    assert(solar_os_font_pack_get(0, &info));
    assert(info.reduced);
    assert(info.bytes <= SOLAR_OS_FONT_PACK_REDUCED_BYTES);
    assert(info.faces_loaded > 0U && info.faces_loaded < info.faces_total);
    /* Regular faces come first, smallest first: 5x8 and size 10 load. */
    assert(has_fallback(u8g2_font_5x8_tf, 0x0105));
    assert(has_fallback(u8g2_font_solar_os_default_r_10_tf, 0x0105));
    /* Bold italic at the largest size does not fit. */
    assert(!has_fallback(u8g2_font_solar_os_default_bi_20_tf, 0x0105));
    unload_all();
}

static void test_selection(void)
{
    psram_bytes = 8U * 1024U * 1024U;
    char saved[64];
    assert(solar_os_font_pack_saved_selection(saved, sizeof(saved)) == ESP_OK);
    assert(saved[0] == '\0');

    assert(solar_os_font_pack_load("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_load("latin-ext") == ESP_OK);
    assert(solar_os_font_pack_save_selection() == ESP_OK);
    assert(strcmp(nvs_value, "cyrillic,latin-ext") == 0);
    unload_all();

    /* At boot the saved packs load again; a pack missing from the volume is
     * skipped and the selection stays saved. */
    snprintf(nvs_value, sizeof(nvs_value), "missing,cyrillic,latin-ext");
    solar_os_font_pack_restore_selection();
    assert(solar_os_font_pack_count() == 2U);
    assert(has_fallback(u8g2_font_5x8_tf, 0x043f));
    assert(strcmp(nvs_value, "missing,cyrillic,latin-ext") == 0);
    unload_all();
}

static void test_generation(void)
{
    const uint32_t before = solar_os_font_pack_generation();
    psram_bytes = 8U * 1024U * 1024U;
    assert(solar_os_font_pack_load("missing") == ESP_ERR_NOT_FOUND);
    assert(solar_os_font_pack_load("bad") == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_font_pack_generation() == before);
    assert(solar_os_font_pack_load("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_generation() == before + 1U);
    /* A reload must invalidate text even though the loaded count is unchanged. */
    assert(solar_os_font_pack_load("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_count() == 1U);
    assert(solar_os_font_pack_generation() == before + 2U);
    assert(solar_os_font_pack_load("broken") == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_font_pack_unload("missing") == ESP_ERR_NOT_FOUND);
    assert(solar_os_font_pack_generation() == before + 2U);
    assert(solar_os_font_pack_unload("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_generation() == before + 3U);
    assert(live_allocs == 0U);
}

static void collect_font_pack(const char *name, void *user)
{
    unsigned *seen = user;
    if (!strcmp(name, "cyrillic")) *seen |= 1U;
    if (!strcmp(name, "latin-ext")) *seen |= 2U;
}

static void test_native_font_packs(void)
{
    const solar_os_native_font_packs_api_v1_t *api = solar_os_native_font_packs_get_service(
        SOLAR_OS_NATIVE_FONT_PACKS_SERVICE, SOLAR_OS_NATIVE_FONT_PACKS_ABI, sizeof(*api));
    assert(api && !api->count());
    assert(!solar_os_native_font_packs_get_service(SOLAR_OS_NATIVE_FONT_PACKS_SERVICE, 2, 0));
    assert(!api->name_valid("../escape") && api->name_valid("latin-ext"));
    char path[300], expected[300];
    snprintf(expected, sizeof(expected), "%s/fonts", volume);
    assert(!api->directory(path, sizeof(path)) && !strcmp(path, expected));
    unsigned seen = 0;
    assert(!api->visit_files(collect_font_pack, &seen) && seen == 3);
    uint32_t generation = api->generation();
    assert(!api->load("cyrillic") && api->count() == 1 && api->generation() == generation + 1);
    assert(!api->load("cyrillic") && api->count() == 1 && api->generation() == generation + 2);
    solar_os_font_pack_info_t snapshot;
    assert(!api->info(0, &snapshot) && !strcmp(snapshot.name, "cyrillic") && snapshot.faces_loaded == 25);
    assert(!api->save_selection());
    char names[32];
    assert(api->saved_selection(names, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !names[0]);
    assert(!api->saved_selection(names, sizeof(names)) && !strcmp(names, "cyrillic"));
    assert(!api->unload("cyrillic") && api->generation() == generation + 3);
    assert(!strcmp(snapshot.name, "cyrillic") && snapshot.bytes && !live_allocs);
    assert(api->info(0, &snapshot) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !snapshot.name[0]);
    assert(!api->save_selection() && !api->saved_selection(names, sizeof(names)) && !names[0]);
}

int main(void)
{
    char tmpl[] = "/tmp/font_packs_test.XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    snprintf(volume, sizeof(volume), "%s", tmpl);
    char fonts[300];
    snprintf(fonts, sizeof(fonts), "%s/fonts", volume);
    assert(mkdir(fonts, 0700) == 0);
    copy_pack("cyrillic");
    copy_pack("latin-ext");

    test_names();
    test_bad_files();
    test_face_validation();
    test_full_load_with_psram();
    test_retained_faces_survive_reload_and_unload();
    test_reduced_load_without_psram();
    test_selection();
    test_generation();
    test_native_font_packs();

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", volume);
    assert(system(cmd) == 0);
    puts("font pack tests passed");
    return 0;
}
