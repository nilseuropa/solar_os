/* Use the real input service with storage that survives input reinitialization. */
#define main input_service_tests
#define nvs_open fixture_nvs_open
#define nvs_get_u16 fixture_nvs_get_u16
#define nvs_set_u16 fixture_nvs_set_u16
#define nvs_get_blob fixture_nvs_get_blob
#define nvs_set_blob fixture_nvs_set_blob
#define nvs_erase_key fixture_nvs_erase_key
#define nvs_commit fixture_nvs_commit
#include "input_test.c"
#undef main
#undef nvs_open
#undef nvs_get_u16
#undef nvs_set_u16
#undef nvs_get_blob
#undef nvs_set_blob
#undef nvs_erase_key
#undef nvs_commit

static uint8_t saved_layouts[8];
static size_t saved_count;
static uint16_t saved_layout;
static bool saved_single;
static unsigned writes;
static esp_err_t commit_error;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{ (void)mode; *handle = strcmp(name, "blekbd") == 0 ? 2U : 1U; return ESP_OK; }
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value)
{
    if (strcmp(key, "layout") != 0) return ESP_ERR_NVS_NOT_FOUND;
    if (handle == 2U) { *value = SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE; return ESP_OK; }
    if (!saved_single) return ESP_ERR_NVS_NOT_FOUND;
    *value = saved_layout;
    return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value)
{
    assert(handle == 1U && strcmp(key, "layout") == 0);
    saved_layout = value; saved_single = true; writes++;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *length)
{
    (void)handle;
    if (strcmp(key, "layouts") != 0 || saved_count == 0) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < saved_count) return ESP_ERR_INVALID_SIZE;
    memcpy(data, saved_layouts, saved_count); *length = saved_count;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(handle == 1U && strcmp(key, "layouts") == 0 && length <= sizeof(saved_layouts));
    memcpy(saved_layouts, data, length); saved_count = length; writes++;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1U && strcmp(key, "layouts") == 0);
    if (!saved_count) return ESP_ERR_NVS_NOT_FOUND;
    saved_count = 0; writes++;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1U); writes++; return commit_error; }

static void drain(void)
{
    solar_os_input_key_event_t events[64];
    while (solar_os_input_read_events(events, 64)) {}
}
static void space(solar_os_input_source_t keyboard, uint8_t mods, solar_os_input_key_action_t action)
{
    assert(solar_os_input_write_key(keyboard, 0x2c, 0x2c, 0, mods, action) == ESP_OK);
}
static void toggle(solar_os_input_source_t keyboard, uint8_t mods)
{
    space(keyboard, mods, SOLAR_OS_INPUT_KEY_PRESS);
    space(keyboard, 0, SOLAR_OS_INPUT_KEY_RELEASE);
}
static void expect_list(const char *expected)
{
    char text[SOLAR_OS_INPUT_KEYBOARD_LAYOUT_LIST_MAX];
    assert(solar_os_input_keyboard_layout_list(text, sizeof(text)) == ESP_OK);
    assert(strcmp(text, expected) == 0);
}

int main(void)
{
    /* Old BLE settings still migrate; the new list takes precedence at boot. */
    assert(solar_os_input_init() == ESP_OK);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE);
    expect_list("de");
    assert(solar_os_input_set_keyboard_layout_list("us,ua") == ESP_OK);
    expect_list("us,ua");
    const unsigned saved_writes = writes;
    const char *invalid[] = {NULL, "", ",us", "us,", "us,,ua", "us,us", "US,ua", "xx", "us, ua", "us,de,ua,fr,ro,us"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(solar_os_input_set_keyboard_layout_list(invalid[i]) == ESP_ERR_INVALID_ARG);
    assert(writes == saved_writes);
    expect_list("us,ua");
    char tiny[2];
    assert(solar_os_input_keyboard_layout_list(tiny, sizeof(tiny)) == ESP_ERR_INVALID_SIZE);

    solar_os_input_source_t keyboard;
    assert(solar_os_input_keyboard_source_open("switch-test", true, &keyboard) == ESP_OK);
    space(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL, SOLAR_OS_INPUT_KEY_PRESS);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);
    /* Duplicate presses, hardware repeats and software repeats cannot cycle. */
    space(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL, SOLAR_OS_INPUT_KEY_PRESS);
    space(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL, SOLAR_OS_INPUT_KEY_REPEAT);
    now_us += 2000000;
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);
    const uint8_t letter = solar_os_input_translate_hid_usage(0x04, 0, false);
    assert(solar_os_input_write_key(keyboard, 4, 4, letter, 0, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    space(keyboard, 0, SOLAR_OS_INPUT_KEY_RELEASE);
    assert(solar_os_input_write_key(keyboard, 4, 4, letter, 0, SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    solar_os_input_key_event_t events[8];
    assert(solar_os_input_read_events(events, 8) == 5);
    assert(events[0].layout_switch && events[0].key == 0);
    assert(events[1].layout_switch && events[1].action == SOLAR_OS_INPUT_KEY_REPEAT);
    assert(solar_os_input_resolve_layout_key(&events[2]).codepoint == 0x0444);
    assert(events[3].layout_switch && events[3].modifiers == 0 && events[3].key == 0);
    assert(writes == saved_writes);
    toggle(keyboard, SOLAR_OS_INPUT_MOD_RIGHT_CTRL);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US);
    drain();
    const uint8_t other_mods[] = {0, SOLAR_OS_INPUT_MOD_LEFT_CTRL | SOLAR_OS_INPUT_MOD_LEFT_SHIFT,
        SOLAR_OS_INPUT_MOD_LEFT_CTRL | SOLAR_OS_INPUT_MOD_RIGHT_ALT,
        SOLAR_OS_INPUT_MOD_LEFT_CTRL | SOLAR_OS_INPUT_MOD_LEFT_GUI};
    for (size_t i = 0; i < sizeof(other_mods); i++) {
        toggle(keyboard, other_mods[i]);
        assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US);
        drain();
    }
    toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    char chars[4];
    assert(solar_os_input_read_chars(chars, sizeof(chars)) == 0);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);

    /* Held keys retain their original layout across the switching barrier. */
    assert(solar_os_input_set_keyboard_layout_list("de,us") == ESP_OK);
    const uint8_t dead = solar_os_input_translate_hid_usage(0x35, 0, false);
    assert(solar_os_input_write_key(keyboard, 0x35, 0x35, dead, 0, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    assert(solar_os_input_write_key(keyboard, 0x35, 0x35, dead, 0, SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    assert(solar_os_input_read_events(events, 8) == 4);
    assert(solar_os_input_resolve_layout_key(&events[0]).dead != 0);
    (void)solar_os_input_resolve_layout_key(&events[3]);
    assert(events[0].key == '^' && events[3].key == '^');

    /* Set-key sources may report modifier changes while Space remains held. */
    assert(solar_os_input_set_keyboard_layout_list("us,ua") == ESP_OK);
    assert(solar_os_input_set_key(keyboard, 44, 44, ' ', SOLAR_OS_INPUT_MOD_LEFT_CTRL) == ESP_OK);
    assert(solar_os_input_set_key(keyboard, 44, 44, ' ', SOLAR_OS_INPUT_MOD_LEFT_CTRL) == ESP_OK);
    assert(solar_os_input_set_key(keyboard, 44, 44, ' ', 0) == ESP_OK);
    assert(solar_os_input_set_key(keyboard, 44, 44, 0, 0) == ESP_OK);
    assert(solar_os_input_read_events(events, 8) == 2);
    assert(events[0].layout_switch && events[1].layout_switch);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);
    const unsigned before_boot = writes;
    assert(solar_os_input_init() == ESP_OK);
    assert(writes == before_boot);
    expect_list("us,ua");
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US);

    int owner;
    assert(solar_os_input_capture_keyboard_with_shortcuts("switch-test", &owner) == ESP_OK);
    toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);
    assert(solar_os_input_read_events(events, 8) == 2 && events[0].layout_switch);
    bool reset;
    assert(!solar_os_input_read_captured_key(&owner, &events[0], &reset));
    solar_os_input_release_keyboard(&owner);
    assert(solar_os_input_capture_keyboard("switch-test", &owner) == ESP_OK);
    toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);
    assert(solar_os_input_read_events(events, 8) == 0);
    assert(solar_os_input_read_captured_key(&owner, &events[0], &reset) && !reset && !events[0].layout_switch);
    assert(solar_os_input_read_captured_key(&owner, &events[0], &reset) && !reset);
    solar_os_input_release_keyboard(&owner);

    /* An on-screen keyboard can use its configurable alternate chord while
     * the configured language list reserves Ctrl+Space for layout switching. */
    solar_os_input_route_config_t route = {
        .target = "display0", .native_width = 320, .native_height = 240,
        .scale = 1, .x = 0, .y = 120, .width = 320, .height = 120,
    };
    assert(solar_os_input_parse_key_chord("CTRL+ALT+SPACE", &route.trigger_key, &route.trigger_modifiers));
    assert(solar_os_input_route_claim(&owner, &route) == ESP_OK);
    const uint8_t alternate = solar_os_input_translate_hid_usage(44, route.trigger_modifiers, false);
    assert(solar_os_input_write_key(keyboard, 44, 44, alternate, route.trigger_modifiers,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    assert(solar_os_input_write_key(keyboard, 44, 44, alternate, 0, SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);
    assert(solar_os_input_read_events(events, 8) == 0);
    solar_os_input_route_event_t routed;
    assert(solar_os_input_route_read(&owner, &routed, &reset) && !reset);
    assert(routed.type == SOLAR_OS_INPUT_ROUTE_KEY && !routed.data.key.layout_switch);
    assert(solar_os_input_route_read(&owner, &routed, &reset) && !reset);
    route.trigger_modifiers = SOLAR_OS_INPUT_MOD_LEFT_CTRL;
    assert(solar_os_input_route_update(&owner, &route) == ESP_OK);
    toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US);
    assert(solar_os_input_read_events(events, 8) == 2 && events[0].layout_switch);
    assert(!solar_os_input_route_read(&owner, &routed, &reset));
    solar_os_input_route_release(&owner);

    /* A single layout removes the saved list and releases the shortcut. */
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR) == ESP_OK);
    expect_list("fr");
    toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR);
    assert(solar_os_input_read_events(events, 8) == 2 && !events[0].layout_switch);
    assert(solar_os_input_init() == ESP_OK);
    expect_list("fr");

    assert(solar_os_input_set_keyboard_layout_list("us,de,ua,fr,ro") == ESP_OK);
    expect_list("us,de,ua,fr,ro");
    const unsigned before_cycles = writes;
    for (unsigned i = 0; i < 10; i++) {
        toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
        assert((unsigned)solar_os_input_keyboard_layout() == (i + 1U) % 5U);
        drain();
    }
    assert(writes == before_cycles);
    assert(solar_os_input_write_key_tap(keyboard, 0, 44, ' ', SOLAR_OS_INPUT_MOD_LEFT_CTRL) == ESP_OK);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE);
    assert(solar_os_input_read_events(events, 8) == 2);
    assert(events[0].layout_switch && events[1].layout_switch);
    /* Detaching while the shortcut is held must not leave it held or cycle again. */
    space(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL, SOLAR_OS_INPUT_KEY_PRESS);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_UA);
    solar_os_input_source_close(keyboard);
    drain();
    assert(solar_os_input_keyboard_source_open("switch-test", true, &keyboard) == ESP_OK);
    toggle(keyboard, SOLAR_OS_INPUT_MOD_LEFT_CTRL);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR);
    drain();
    /* A full queue cannot switch silently without delivering the composition barrier. */
    for (unsigned i = 0; i < 32; i++) assert(solar_os_input_write_char(keyboard, 'x') == ESP_OK);
    assert(solar_os_input_write_key(keyboard, 44, 44, 0, SOLAR_OS_INPUT_MOD_LEFT_CTRL,
                                    SOLAR_OS_INPUT_KEY_PRESS) == ESP_ERR_NO_MEM);
    assert(solar_os_input_keyboard_layout() == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR);
    solar_os_input_source_release_all(keyboard);
    drain();
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_FR) == ESP_OK);
    commit_error = ESP_FAIL;
    assert(solar_os_input_set_keyboard_layout_list("us,ua") == ESP_FAIL);
    expect_list("fr");
    solar_os_input_source_close(keyboard);
    puts("layout list persistence, Ctrl+Space, queued text, held keys, capture and write failures passed");
    return 0;
}
