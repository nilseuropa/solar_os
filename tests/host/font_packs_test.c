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
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f) != NULL);
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_r_10_tf, 0x0490) != NULL);
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_bi_20_tf, 0x0401) != NULL);
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_r_14_tf, 0x0400) == NULL);
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_r_14_tf, 0x0105) == NULL);
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_r_14_tf, 'A') == NULL);
    /* Faces differ per size and style. */
    const uint8_t *r8 = solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f);
    const uint8_t *r10 = solar_os_font_fallback(u8g2_font_solar_os_default_r_10_tf, 0x043f);
    const uint8_t *b10 = solar_os_font_fallback(u8g2_font_solar_os_default_b_10_tf, 0x043f);
    assert(r8 != r10 && r10 != b10);
    /* A U8g2 face header: glyph count, then bounding box height 8 at size 8. */
    assert(r8[0] > 0U && r8[10] == 8U);

    /* A second pack covers its own ranges; loading the same pack again
     * replaces it instead of taking another slot. */
    assert(solar_os_font_pack_load("latin-ext") == ESP_OK);
    assert(solar_os_font_pack_load("latin-ext") == ESP_OK);
    assert(solar_os_font_pack_count() == 2U);
    assert(live_allocs == 2U);
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x20ac) != NULL);
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_i_16_tf, 0x0219) != NULL);

    assert(solar_os_font_pack_unload("cyrillic") == ESP_OK);
    assert(solar_os_font_pack_unload("cyrillic") == ESP_ERR_NOT_FOUND);
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f) == NULL);
    unload_all();
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x20ac) == NULL);
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
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x0105) != NULL);
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_r_10_tf, 0x0105) != NULL);
    /* Bold italic at the largest size does not fit. */
    assert(solar_os_font_fallback(u8g2_font_solar_os_default_bi_20_tf, 0x0105) == NULL);
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
    assert(solar_os_font_fallback(u8g2_font_5x8_tf, 0x043f) != NULL);
    assert(strcmp(nvs_value, "missing,cyrillic,latin-ext") == 0);
    unload_all();
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
    test_full_load_with_psram();
    test_reduced_load_without_psram();
    test_selection();

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", volume);
    assert(system(cmd) == 0);
    puts("font pack tests passed");
    return 0;
}
