#include "solar_os_font_packs.h"

#include <dirent.h>
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
static const uint8_t pack_sizes[SOLAR_OS_FONT_SIZE_COUNT] = {10, 12, 14, 16, 18, 20};

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
    pack_lock_give();
    return ESP_OK;
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
