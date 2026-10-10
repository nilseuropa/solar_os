#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/task.h"
#include "solar_os_input_keymap.h"
#include "solar_os_memory.h"

typedef struct {
    bool locked, stopped, close_on_lock;
    esp_err_t prepare_error;
    unsigned prepares;
    solar_os_input_keymap_handle_t handle;
} owner_t;
static owner_t owner, second;
static solar_os_input_key_event_t events[64];
static unsigned event_count, released, chars, allocations, fail_allocation;
static uint8_t last_char, modifiers;
static TickType_t ticks;
static const char *contents;
static size_t content_length;
static esp_err_t read_error;
static bool close_during_read;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size != 0U) { size_t n = len < size - 1U ? len : size - 1U; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag != NULL);
    return ++allocations == fail_allocation ? NULL : malloc(size);
}
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
bool solar_os_input_source_get_info(solar_os_input_source_t source, solar_os_input_source_info_t *info)
{
    if (source < 1U || source > 10U) return false;
    memset(info, 0, sizeof(*info)); info->source = source;
    info->capabilities = SOLAR_OS_INPUT_CAP_KEY_EVENTS;
    info->ready = true; info->source_class = SOLAR_OS_INPUT_SOURCE_KEYBOARD;
    snprintf(info->name, sizeof(info->name), "source%u", source);
    return true;
}
bool solar_os_input_source_find(const char *name, solar_os_input_source_info_t *info)
{
    for (unsigned i = 1; i <= 10; i++) {
        solar_os_input_source_info_t item;
        solar_os_input_source_get_info((solar_os_input_source_t)i, &item);
        if (strcmp(name, item.name) == 0) { *info = item; return true; }
    }
    return false;
}
void solar_os_input_source_release_all(solar_os_input_source_t source) { assert(source != 0U); released++; }
void solar_os_input_composition_set_modifiers(solar_os_input_source_t source, uint8_t mods)
{ assert(source != 0U); modifiers = mods; }
uint8_t solar_os_input_composition_apply(uint8_t key) { return key; }
uint8_t solar_os_input_translate_hid_usage(uint16_t usage, uint8_t mods, bool caps)
{
    if (usage == 0x2cU) return ' ';
    if (usage < 4U || usage > 29U) return 0;
    return (uint8_t)((((mods & SOLAR_OS_INPUT_MOD_SHIFT) != 0) != caps ? 'A' : 'a') + usage - 4U);
}
esp_err_t solar_os_input_write_key(solar_os_input_source_t source, uint16_t physical,
    uint16_t usage, uint8_t key, uint8_t mods, solar_os_input_key_action_t action)
{
    assert(event_count < 64U);
    events[event_count++] = (solar_os_input_key_event_t) {
        .source=source, .physical_key=physical, .usage=usage, .key=key, .modifiers=mods, .action=action,
    };
    return ESP_OK;
}
esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char key)
{ assert(source != 0U); last_char = (uint8_t)key; chars++; return ESP_OK; }

static esp_err_t lock(void *context)
{
    owner_t *o = context;
    if (o->stopped) return ESP_ERR_INVALID_STATE;
    assert(!o->locked); o->locked = true;
    if (o->close_on_lock) {
        o->close_on_lock = false;
        assert(solar_os_input_keymap_unregister(o->handle) == ESP_ERR_TIMEOUT);
    }
    return ESP_OK;
}
static void unlock(void *context) { owner_t *o = context; assert(o->locked); o->locked = false; }
static esp_err_t prepare(void *context)
{ owner_t *o = context; assert(o->locked); o->prepares++; return o->prepare_error; }
esp_err_t solar_os_storage_read_file(const char *path, void *buffer, size_t len, size_t *read_len)
{
    assert(strcmp(path, "/map.json") == 0);
    if (close_during_read) {
        close_during_read = false;
        assert(solar_os_input_keymap_unregister(owner.handle) == ESP_ERR_TIMEOUT);
    }
    if (read_error != ESP_OK) return read_error;
    *read_len = content_length < len ? content_length : len;
    memcpy(buffer, contents, *read_len);
    return ESP_OK;
}
static void file(const char *text) { contents = text; content_length = strlen(text); }
static void process(owner_t *o, uint16_t physical, bool pressed, bool expected)
{
    assert(lock(o) == ESP_OK);
    bool published = !expected;
    assert(solar_os_input_keymap_process(o->handle, physical, pressed, &published) == ESP_OK);
    assert(published == expected); unlock(o);
}
static void usage(unsigned expected)
{
    solar_os_input_keymap_t map;
    assert(solar_os_input_keymap_get("source1", false, &map) == ESP_OK);
    assert(map.keys[0][1].usage == expected);
}

int main(void)
{
    solar_os_input_keymap_t map = {.slot_count=3};
    map.physical[1] = 50000; map.keys[0][1].usage = 4;
    map.physical[2] = 900; map.keys[0][2].usage = 0xe1;
    map.physical[3] = 60000; map.keys[0][3].usage = 0x2c;
    map.keys[0][3].flags = SOLAR_OS_INPUT_KEYMAP_LAYER_TAP;
    map.keys[1][1].key = '!';
    solar_os_input_keymap_ops_t ops = {.lock=lock, .unlock=unlock, .prepare=prepare, .context=&owner};
    solar_os_input_keymap_info_t info;
    assert(solar_os_input_keymap_info("source1", &info) == ESP_OK && info.capabilities == 0U);
    assert(solar_os_input_keymap_get("source1", false, &map) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_input_keymap_get("missing", false, &map) == ESP_ERR_NOT_FOUND);
    fail_allocation = allocations + 1U;
    assert(solar_os_input_keymap_register(1, &map, &ops, &owner.handle) == ESP_ERR_NO_MEM);
    fail_allocation = 0;
    assert(solar_os_input_keymap_register(1, &map, &ops, &owner.handle) == ESP_OK);
    assert(solar_os_input_keymap_get("source1", false, &map) == ESP_ERR_INVALID_STATE);
    assert(solar_os_input_keymap_activate(owner.handle) == ESP_OK);
    solar_os_input_keymap_handle_t duplicate;
    assert(solar_os_input_keymap_register(1, &map, &ops, &duplicate) == ESP_ERR_INVALID_STATE);
    assert(solar_os_input_keymap_info("source1", &info) == ESP_OK && info.key_count == 3 && info.rows == 0);
    assert((info.capabilities & SOLAR_OS_INPUT_KEYMAP_TAP_HOLD) != 0U);
    assert(solar_os_input_keymap_info("source2", &info) == ESP_OK && info.capabilities == 0U);
    process(&owner, 50000, true, true); process(&owner, 50000, true, false);
    assert(events[0].physical_key == 50000 && events[0].key == 'a');
    process(&owner, 50000, false, true); process(&owner, 50000, false, false);
    process(&owner, 900, true, true); process(&owner, 50000, true, true);
    assert(events[event_count-1U].key == 'A' && modifiers == SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    ops.context = &second;
    assert(solar_os_input_keymap_register(3, &map, &ops, &second.handle) == ESP_OK);
    assert(solar_os_input_keymap_activate(second.handle) == ESP_OK);
    process(&second, 50000, true, true); assert(events[event_count-1U].key == 'a');
    solar_os_input_keymap_release_all(owner.handle);
    process(&owner, 60000, true, true); process(&owner, 60000, false, true);
    assert(chars == 1 && last_char == ' ');
    process(&owner, 60000, true, true); process(&owner, 50000, true, true);
    assert(events[event_count-1U].key == '!');
    process(&owner, 60000, false, true); process(&owner, 50000, false, true);
    assert(chars == 1 && events[event_count-1U].key == '!');
    solar_os_input_keymap_t custom = map;
    custom.physical[1] = 1234;
    unsigned prior_released = released;
    assert(solar_os_input_keymap_set("source1", &custom) == ESP_ERR_INVALID_ARG && released == prior_released);
    custom = map; custom.keys[0][1].usage = 5;
    assert(solar_os_input_keymap_set("source1", &custom) == ESP_OK); usage(5);
    owner.prepare_error = ESP_FAIL;
    assert(solar_os_input_keymap_reset("source1") == ESP_FAIL); usage(5);
    owner.prepare_error = ESP_OK;
    assert(solar_os_input_keymap_reset("source1") == ESP_OK); usage(4);
    assert(solar_os_input_keymap_load("source1", "relative") == ESP_ERR_INVALID_ARG);
    file("{\"schema\":1,\"keys\":[{\"physical\":50000,\"usage\":5}]}");
    assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_OK); usage(5);
    for (unsigned i = 1; i <= 3; i++) {
        fail_allocation = allocations + i;
        assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_ERR_NO_MEM); usage(5);
    }
    fail_allocation = 0;
    file("{\"schema\":1,\"keys\":[]}");
    assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_OK); usage(4);
    read_error = ESP_FAIL;
    assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_FAIL);
    read_error = ESP_OK; file("{}");
    assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_ERR_INVALID_ARG); usage(4);
    static const char nul[] = "{\"schema\":1,\"keys\":[]}\0trailing";
    contents=nul; content_length=sizeof(nul)-1U;
    assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_ERR_INVALID_ARG);
    char large[16385]; memset(large, ' ', sizeof(large)); contents=large; content_length=sizeof(large);
    assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_ERR_INVALID_SIZE);
    owner.stopped = true;
    assert(solar_os_input_keymap_reset("source1") == ESP_ERR_INVALID_STATE);
    owner.stopped = false; owner.close_on_lock = true;
    assert(solar_os_input_keymap_get("source1", false, &custom) == ESP_ERR_INVALID_STATE);
    assert(solar_os_input_keymap_unregister(owner.handle) == ESP_OK);
    solar_os_input_keymap_handle_t stale = owner.handle;
    ops.context = &owner;
    assert(solar_os_input_keymap_register(1, &map, &ops, &owner.handle) == ESP_OK && owner.handle != stale);
    assert(solar_os_input_keymap_activate(owner.handle) == ESP_OK);
    assert(solar_os_input_keymap_process(stale, 50000, true, NULL) == ESP_ERR_INVALID_STATE);
    file("{\"schema\":1,\"keys\":[{\"physical\":50000,\"usage\":5}]}");
    close_during_read = true;
    assert(solar_os_input_keymap_load("source1", "/map.json") == ESP_ERR_INVALID_STATE);
    assert(solar_os_input_keymap_unregister(owner.handle) == ESP_OK);
    assert(solar_os_input_keymap_unregister(second.handle) == ESP_OK);
    owner_t owners[9] = {0};
    for (unsigned i = 0; i < 8U; i++) {
        ops.context = &owners[i];
        assert(solar_os_input_keymap_register((solar_os_input_source_t)(i+1U), &map,
            &ops, &owners[i].handle) == ESP_OK);
        assert(solar_os_input_keymap_activate(owners[i].handle) == ESP_OK);
    }
    ops.context = &owners[8];
    assert(solar_os_input_keymap_register(9, &map, &ops, &owners[8].handle) == ESP_ERR_NO_MEM);
    for (unsigned i = 0; i < 8U; i++) {
        owners[i].stopped = true;
        assert(solar_os_input_keymap_unregister(owners[i].handle) == ESP_OK);
    }
    puts("source keymap service tests: ok");
    return 0;
}
