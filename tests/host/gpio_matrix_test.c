#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/services/solar_os_gpio_matrix.c"
#include "../../src/services/solar_os_gpio_matrix_driver.c"

struct fake_keyboard { keyboard_btn_config_t config; keyboard_btn_cb_config_t cb; bool started; };
static unsigned opened, closed, released, event_count, created, deleted, resets;
static unsigned fail_stage;
static bool alive[256];
static char names[256][SOLAR_OS_INPUT_SOURCE_NAME_MAX];
static solar_os_input_key_event_t events[512];
static TickType_t ticks;
static const char *file_content;
static bool ownership_ok = true;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1U ? length : size - 1U; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return fail_stage == 1 ? NULL : malloc(size); }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
bool solar_os_input_source_get_info(solar_os_input_source_t source, solar_os_input_source_info_t *info)
{
    if (!alive[source]) return false;
    memset(info, 0, sizeof(*info)); info->source = source;
    info->capabilities = SOLAR_OS_INPUT_CAP_KEY_EVENTS;
    strlcpy(info->name, names[source], sizeof(info->name)); return true;
}
bool solar_os_input_source_find(const char *name, solar_os_input_source_info_t *info)
{
    for (unsigned i = 1; i <= opened; i++)
        if (alive[i] && strcmp(name, names[i]) == 0) return solar_os_input_source_get_info(i, info);
    return false;
}
esp_err_t solar_os_input_keyboard_source_open(const char *name, bool ready, solar_os_input_source_t *source)
{
    assert(ready);
    if (fail_stage == 2) return ESP_ERR_NO_MEM;
    *source = ++opened; alive[*source] = true;
    strlcpy(names[*source], name, sizeof(names[*source])); return ESP_OK;
}
void solar_os_input_source_close(solar_os_input_source_t source)
{ assert(alive[source]); alive[source] = false; closed++; }
void solar_os_input_source_release_all(solar_os_input_source_t source)
{ assert(alive[source]); released++; }
esp_err_t solar_os_input_keyboard_source_set_ready(solar_os_input_source_t source, bool ready)
{ assert(alive[source]); (void)ready; return ESP_OK; }
void solar_os_input_composition_set_modifiers(solar_os_input_source_t source, uint8_t mods)
{ assert(alive[source]); (void)mods; }
uint8_t solar_os_input_composition_apply(uint8_t key) { return key; }
uint8_t solar_os_input_translate_hid_usage(uint16_t usage, uint8_t mods, bool caps)
{
    (void)caps;
    if (usage >= 4U && usage <= 29U) return (mods & SOLAR_OS_INPUT_MOD_SHIFT ? 'A' : 'a') + usage - 4U;
    return 0;
}
esp_err_t solar_os_input_write_key(solar_os_input_source_t source, uint16_t physical,
    uint16_t usage, uint8_t key, uint8_t mods, solar_os_input_key_action_t action)
{
    assert(alive[source] && event_count < 512U);
    if (fail_stage == 6) return ESP_ERR_NO_MEM;
    events[event_count++] = (solar_os_input_key_event_t){
        .source=source, .physical_key=physical, .usage=usage, .key=key, .modifiers=mods, .action=action,
    };
    return ESP_OK;
}
esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char key)
{ assert(alive[source]); (void)key; return ESP_OK; }
esp_err_t solar_os_storage_read_file(const char *path, void *data, size_t len, size_t *read_len)
{
    (void)path; assert(file_content != NULL); *read_len = strlen(file_content);
    assert(*read_len <= len); memcpy(data, file_content, *read_len); return ESP_OK;
}
bool solar_os_resource_find_claim(solar_os_resource_kind_t kind, int pin, int secondary,
    solar_os_resource_claim_t *claim)
{
    assert(kind == SOLAR_OS_RESOURCE_GPIO_PIN && pin >= 0 && secondary == -1);
    if (!ownership_ok) return false;
    memset(claim, 0, sizeof(*claim)); strlcpy(claim->owner, "keys", sizeof(claim->owner)); return true;
}
esp_err_t gpio_reset_pin(gpio_num_t pin) { assert(pin >= 0); resets++; return ESP_OK; }
esp_err_t keyboard_button_create(keyboard_btn_config_t *config, keyboard_btn_handle_t *scanner)
{
    assert(config->start_paused && !config->enable_power_save && config->active_level == 0);
    assert(config->ticks_interval == 1000 && config->debounce_ticks >= 1);
    if (fail_stage == 3) return ESP_ERR_NO_MEM;
    *scanner = calloc(1, sizeof(**scanner)); assert(*scanner);
    (*scanner)->config = *config; created++; return ESP_OK;
}
esp_err_t keyboard_button_register_cb(keyboard_btn_handle_t scanner, keyboard_btn_cb_config_t cb, void *handle)
{ (void)handle; assert(!scanner->started); if (fail_stage == 4) return ESP_ERR_NO_MEM; scanner->cb = cb; return ESP_OK; }
esp_err_t solar_os_keyboard_button_start(keyboard_btn_handle_t scanner)
{ assert(scanner->cb.callback); if (fail_stage == 5) return ESP_FAIL; scanner->started = true; return ESP_OK; }
esp_err_t keyboard_button_delete(keyboard_btn_handle_t scanner)
{
    /* The final callback must not deadlock against detach or publish input. */
    if (scanner->started) scanner->cb.callback(scanner, (keyboard_btn_report_t){0}, scanner->cb.user_data);
    free(scanner); deleted++; return ESP_OK;
}

static solar_os_expansion_binding_t bindings[24];
static unsigned binding_count;
static void binding(const char *role, int value, bool parameter)
{
    solar_os_expansion_binding_t *b = &bindings[binding_count++];
    memset(b, 0, sizeof(*b)); b->kind = parameter ? SOLAR_OS_EXPANSION_BINDING_PARAMETER : SOLAR_OS_EXPANSION_BINDING_GPIO;
    strlcpy(b->role, role, sizeof(b->role)); b->value = value;
}
static void geometry(unsigned rows, unsigned cols)
{
    binding_count = 0;
    for (unsigned i = 0; i < rows + cols; i++) {
        char role[8]; snprintf(role, sizeof(role), "%s%u", i < rows ? "row" : "col", i < rows ? i : i - rows);
        binding(role, (int)i + 1, false);
    }
}
static void snapshot(uint64_t mask)
{
    matrix_device_t *dev = find_device("keys"); assert(dev && dev->scanner->started);
    keyboard_btn_data_t keys[80]; unsigned count = 0;
    for (unsigned i = 0; i < dev->rows * dev->cols; i++) {
        if (i < 64 && (mask & (1ULL << i))) {
            unsigned row = i / dev->cols, col = i % dev->cols;
            keys[count++] = (keyboard_btn_data_t){
                .output_index = dev->columns_drive ? col : row,
                .input_index = dev->columns_drive ? row : col,
            };
        }
    }
    dev->scanner->cb.callback(dev->scanner, (keyboard_btn_report_t){.key_pressed_num=count, .key_data=keys}, dev);
}

int main(void)
{
    geometry(2, 2);
    ownership_ok = false;
    assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_ERR_INVALID_STATE);
    assert(!created && !resets); ownership_ok = true;
    bindings[1].value = bindings[0].value;
    assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_ERR_INVALID_ARG);
    geometry(2, 2); strcpy(bindings[1].role, "row2");
    assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_ERR_INVALID_ARG);
    geometry(2, 2); binding("drive", 2, true);
    assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_ERR_INVALID_ARG);
    geometry(2, 2);
    for (fail_stage = 1; fail_stage <= 5; fail_stage++) {
        assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) != ESP_OK);
        assert(!find_device("keys") && opened == closed && created == deleted);
    }
    fail_stage = 0;
    assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_OK);
    assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_ERR_INVALID_STATE);
    solar_os_input_keymap_t map;
    assert(solar_os_input_keymap_get("keys", false, &map) == ESP_OK);
    assert(map.rows == 2 && map.cols == 2 && map.first == 1 && map.stride == 2 && map.slot_count == 4);
    snapshot(1); assert(event_count == 1 && events[0].physical_key == 1 && events[0].key == 0);
    snapshot(1); assert(event_count == 1);
    snapshot(0); assert(event_count == 2 && events[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    file_content = "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":225},{\"row\":0,\"col\":1,\"usage\":4}]}";
    assert(solar_os_input_keymap_load("keys", "/map.json") == ESP_OK);
    snapshot(3); assert(events[event_count - 1].key == 'A');
    snapshot(0);
    /* Modifier in the last matrix slot must precede a first-slot letter. */
    file_content = "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":4},{\"row\":1,\"col\":1,\"usage\":225}]}";
    assert(solar_os_input_keymap_load("keys", "/map.json") == ESP_OK);
    snapshot(9); assert(events[event_count - 1].key == 'A');
    snapshot(0);
    /* A same-scan Fn press selects symbols before the letter is decoded. */
    file_content = "{\"schema\":1,\"keys\":[{\"row\":0,\"col\":0,\"usage\":4},{\"row\":1,\"col\":1,\"usage\":44,\"layer_tap\":true}],\"symbols\":[{\"row\":0,\"col\":0,\"key\":33}]}";
    assert(solar_os_input_keymap_load("keys", "/map.json") == ESP_OK);
    snapshot(9); assert(events[event_count - 1].key == '!');
    snapshot(0);
    snapshot(3);
    unsigned before = event_count;
    assert(solar_os_input_keymap_reset("keys") == ESP_OK);
    snapshot(7); assert(event_count == before + 1 && events[event_count - 1].physical_key == 3);
    snapshot(0);
    snapshot(15); before = event_count; /* diode-less rectangle: all new keys blocked */
    assert(!find_device("keys")->held[0] && !find_device("keys")->held[3]);
    snapshot(1); assert(event_count == before + 1);
    snapshot(15); assert(event_count == before + 1); /* retain the real, existing hold */
    snapshot(0); assert(events[event_count - 1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    fail_stage = 6; snapshot(1); fail_stage = 0;
    before = event_count; snapshot(3); assert(event_count == before + 1 && events[event_count - 1].physical_key == 2);
    assert(solar_os_gpio_matrix_detach("keys") == ESP_OK);
    assert(solar_os_gpio_matrix_detach("keys") == ESP_ERR_NOT_FOUND);
    assert(opened == closed && created == deleted);
    geometry(2, 2); binding("drive", 1, true); binding("diodes", 1, true); binding("debounce-ms", 25, true);
    assert(solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_OK);
    assert(find_device("keys")->scanner->config.output_gpios[0] == 3);
    snapshot(4); assert(events[event_count - 1].physical_key == 3);
    snapshot(15); assert(find_device("keys")->held[0] && find_device("keys")->held[3]);
    assert(solar_os_gpio_matrix_detach("keys") == ESP_OK);
    geometry(8, 10);
    assert(binding_count == 18 && solar_os_gpio_matrix_attach("keys", bindings, binding_count) == ESP_OK);
    assert(solar_os_input_keymap_get("keys", false, &map) == ESP_OK && map.slot_count == 80);
    assert(solar_os_gpio_matrix_detach("keys") == ESP_OK);
    assert(opened == closed && created == deleted && released > 0);
    puts("GPIO matrix: ownership, geometry, cleanup, snapshots, keymaps, modifiers, ghosting and scan direction PASS");
    return 0;
}
