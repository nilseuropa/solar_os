/* Exercise layer taps through the real keymap and input services. */
#define main input_service_tests
#include "input_test.c"
#undef main

#include "freertos/task.h"
#include "solar_os_input_keymap.h"

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    assert(strcmp(tag, "input-keymap") == 0);
    return malloc(size);
}

TickType_t xTaskGetTickCount(void) { return (TickType_t)(now_us / 1000); }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
void solar_os_input_composition_set_modifiers(solar_os_input_source_t source, uint8_t mods)
{ assert(source != SOLAR_OS_INPUT_SOURCE_INVALID); (void)mods; }
uint8_t solar_os_input_composition_apply(uint8_t key) { return key; }
esp_err_t solar_os_storage_read_file(const char *path, void *data, size_t len, size_t *read_len)
{ (void)path; (void)data; (void)len; (void)read_len; return ESP_ERR_NOT_SUPPORTED; }

static bool locked;
static esp_err_t lock(void *context)
{ assert(context == &locked && !locked); locked = true; return ESP_OK; }
static void unlock(void *context)
{ assert(context == &locked && locked); locked = false; }

static void process(solar_os_input_keymap_handle_t handle, uint16_t physical, bool pressed)
{
    bool published = false;
    assert(lock(&locked) == ESP_OK);
    assert(solar_os_input_keymap_process(handle, physical, pressed, &published) == ESP_OK);
    assert(published);
    unlock(&locked);
}

static void tap(solar_os_input_keymap_handle_t handle, uint16_t physical)
{ process(handle, physical, true); process(handle, physical, false); }

static void check_tap(solar_os_input_keyboard_layout_t layout, uint16_t usage,
    uint8_t override, uint8_t mods, bool caps, uint16_t codepoint, uint8_t byte, bool dead)
{
    assert(solar_os_input_set_keyboard_layout(layout) == ESP_OK);
    solar_os_input_source_t keyboard;
    assert(solar_os_input_keyboard_source_open("layout-tap", true, &keyboard) == ESP_OK);
    solar_os_input_keymap_t map = {.slot_count = 5};
    for (unsigned i = 1; i <= map.slot_count; i++) map.physical[i] = (uint16_t)i;
    map.keys[0][1] = (solar_os_input_keymap_key_t){
        .usage = usage, .key = override, .flags = SOLAR_OS_INPUT_KEYMAP_LAYER_TAP,
    };
    map.keys[0][2].usage = 0xe6; /* AltGr */
    map.keys[0][3].usage = 0xe1; /* Shift */
    map.keys[0][4].usage = 0x39; /* Caps Lock */
    map.keys[0][5].usage = 0x08; /* E */
    map.keys[1][5].key = '!';
    const solar_os_input_keymap_ops_t ops = {.lock = lock, .unlock = unlock, .context = &locked};
    solar_os_input_keymap_handle_t handle;
    assert(solar_os_input_keymap_register(keyboard, &map, &ops, &handle) == ESP_OK);
    assert(solar_os_input_keymap_activate(handle) == ESP_OK);
    if (caps) tap(handle, 4);
    if (mods & SOLAR_OS_INPUT_MOD_RIGHT_ALT) process(handle, 2, true);
    if (mods & SOLAR_OS_INPUT_MOD_LEFT_SHIFT) process(handle, 3, true);
    solar_os_input_key_event_t events[8];
    (void)solar_os_input_read_events(events, 8);

    tap(handle, 1);
    assert(solar_os_input_read_events(events, 8) == 4);
    solar_os_input_key_event_t press = events[2], release = events[3];
    assert(press.source == keyboard && press.physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE);
    assert(press.usage == usage);
    assert(press.action == SOLAR_OS_INPUT_KEY_PRESS && press.modifiers == mods);
    assert(release.physical_key == press.physical_key && release.usage == usage);
    assert(release.action == SOLAR_OS_INPUT_KEY_RELEASE);
    const solar_os_input_text_t text = solar_os_input_resolve_layout_key(&press);
    assert(text.codepoint == codepoint && (text.dead != 0) == dead && press.key == byte);
    if (dead) {
        solar_os_input_compose_t compose = {0};
        uint16_t typed[2];
        assert(solar_os_input_compose_feed(&compose, text, typed) == 0);
        assert(solar_os_input_compose_feed(&compose,
            (solar_os_input_text_t){.codepoint = 'e'}, typed) == 1 && typed[0] == 0x00ea);
    }

    /* Byte readers keep the legacy spacing/fallback character, never a marker. */
    tap(handle, 1);
    char chars[8] = {0};
    const size_t count = solar_os_input_read_source_chars(keyboard, chars, sizeof(chars));
    assert(count == (byte != 0 ? 1U : 0U));
    if (byte != 0) assert((uint8_t)chars[0] == byte);

    /* Using the symbol layer suppresses its tap character. */
    if (mods & SOLAR_OS_INPUT_MOD_RIGHT_ALT) process(handle, 2, false);
    if (mods & SOLAR_OS_INPUT_MOD_LEFT_SHIFT) process(handle, 3, false);
    (void)solar_os_input_read_events(events, 8);
    process(handle, 1, true);
    tap(handle, 5);
    process(handle, 1, false);
    assert(solar_os_input_read_source_chars(keyboard, chars, sizeof(chars)) == 1);
    assert(chars[0] == '!');
    assert(solar_os_input_keymap_unregister(handle) == ESP_OK);
    solar_os_input_source_close(keyboard);
    (void)solar_os_input_read_events(events, 8);
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    check_tap(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x35, 0, 0, false, '^', '^', true);
    check_tap(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA, 0x04, 0, 0, false, 0x0444, 0, false);
    check_tap(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x08, 0,
        SOLAR_OS_INPUT_MOD_RIGHT_ALT, false, 0x20ac, 0, false);
    check_tap(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x20, 0,
        SOLAR_OS_INPUT_MOD_LEFT_SHIFT, false, 0x00a7, 0, false);
    check_tap(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0x2d, 0, 0, true, 0x1e9e, 0xdf, false);
    check_tap(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US, 0x2c, 0, 0, false, ' ', ' ', false);
    check_tap(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE, 0, 'x', 0, false, 'x', 'x', false);
    puts("source keymap layout tap tests: ok");
    return 0;
}
