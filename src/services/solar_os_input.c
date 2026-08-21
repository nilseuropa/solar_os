#include "solar_os_input.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "solar_os_keys.h"

#define INPUT_SOURCE_MAX 8U
#define INPUT_SOURCE_NAME_MAX 16U
#define INPUT_QUEUE_MAX 64U
#define INPUT_REPEAT_RATE_DEFAULT 15U
#define INPUT_REPEAT_DELAY_DEFAULT_MS 450U
#define INPUT_NVS_NAMESPACE "input"
#define INPUT_NVS_REPEAT_RATE_KEY "repeat_cps"
#define INPUT_NVS_REPEAT_DELAY_KEY "repeat_delay"
#define INPUT_NVS_LAYOUT_KEY "layout"
#define INPUT_LEGACY_NVS_NAMESPACE "blekbd"

#define INPUT_LATIN1_A_UMLAUT_UPPER ((uint8_t)0xc4)
#define INPUT_LATIN1_O_UMLAUT_UPPER ((uint8_t)0xd6)
#define INPUT_LATIN1_U_UMLAUT_UPPER ((uint8_t)0xdc)
#define INPUT_LATIN1_SHARP_S ((uint8_t)0xdf)
#define INPUT_LATIN1_A_UMLAUT_LOWER ((uint8_t)0xe4)
#define INPUT_LATIN1_O_UMLAUT_LOWER ((uint8_t)0xf6)
#define INPUT_LATIN1_U_UMLAUT_LOWER ((uint8_t)0xfc)

typedef struct {
    bool active;
    bool keyboard;
    bool ready;
    char name[INPUT_SOURCE_NAME_MAX];
} input_source_slot_t;

typedef struct {
    bool active;
    solar_os_input_key_event_t event;
} input_pressed_slot_t;

typedef struct {
    bool active;
    solar_os_input_source_t source;
    uint16_t physical_key;
    uint32_t next_ms;
} input_repeat_state_t;

static input_source_slot_t input_sources[INPUT_SOURCE_MAX];
static input_pressed_slot_t input_pressed[SOLAR_OS_INPUT_MAX_PRESSED_KEYS];
static solar_os_input_key_event_t input_queue[INPUT_QUEUE_MAX];
static size_t input_queue_head;
static size_t input_queue_count;
static uint16_t input_repeat_rate_cps = INPUT_REPEAT_RATE_DEFAULT;
static uint16_t input_repeat_delay_ms = INPUT_REPEAT_DELAY_DEFAULT_MS;
static solar_os_input_keyboard_layout_t input_keyboard_layout =
    SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US;
static input_repeat_state_t input_repeat;
static portMUX_TYPE input_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *const input_keyboard_layout_names[] = {
    [SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US] = "us",
    [SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE] = "de",
};

static uint32_t input_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool input_source_valid_locked(solar_os_input_source_t source)
{
    return source > SOLAR_OS_INPUT_SOURCE_INVALID &&
        source <= INPUT_SOURCE_MAX &&
        input_sources[source - 1U].active;
}

static bool input_repeat_config_valid(uint16_t rate_cps, uint16_t delay_ms)
{
    if (rate_cps > SOLAR_OS_INPUT_REPEAT_RATE_MAX ||
        (rate_cps != 0 && rate_cps < SOLAR_OS_INPUT_REPEAT_RATE_MIN)) {
        return false;
    }
    return delay_ms >= SOLAR_OS_INPUT_REPEAT_DELAY_MIN_MS &&
        delay_ms <= SOLAR_OS_INPUT_REPEAT_DELAY_MAX_MS;
}

static uint32_t input_repeat_interval_ms(uint16_t rate_cps)
{
    return rate_cps == 0 ? 0 : (1000U + rate_cps - 1U) / rate_cps;
}

static bool input_key_repeatable(uint8_t key)
{
    return key != 0 &&
        key != SOLAR_OS_KEY_ENTER &&
        key != SOLAR_OS_KEY_APP_EXIT &&
        key != SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE &&
        key != SOLAR_OS_KEY_ALT_PREFIX;
}

static input_pressed_slot_t *input_find_pressed_locked(solar_os_input_source_t source,
                                                        uint16_t physical_key)
{
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (input_pressed[i].active &&
            input_pressed[i].event.source == source &&
            input_pressed[i].event.physical_key == physical_key) {
            return &input_pressed[i];
        }
    }
    return NULL;
}

static input_pressed_slot_t *input_alloc_pressed_locked(void)
{
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (!input_pressed[i].active) {
            return &input_pressed[i];
        }
    }
    return NULL;
}

static bool input_queue_push_locked(const solar_os_input_key_event_t *event)
{
    if (event == NULL || input_queue_count >= INPUT_QUEUE_MAX) {
        return false;
    }
    const size_t index = (input_queue_head + input_queue_count) % INPUT_QUEUE_MAX;
    input_queue[index] = *event;
    input_queue_count++;
    return true;
}

static void input_repeat_stop_locked(solar_os_input_source_t source,
                                     uint16_t physical_key)
{
    if (input_repeat.active &&
        input_repeat.source == source &&
        input_repeat.physical_key == physical_key) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
}

static void input_repeat_start_locked(const solar_os_input_key_event_t *event)
{
    if (event == NULL) {
        return;
    }
    if (!input_key_repeatable(event->key)) {
        if (event->key != 0) {
            memset(&input_repeat, 0, sizeof(input_repeat));
        }
        return;
    }
    if (input_repeat_rate_cps == 0) {
        return;
    }
    input_repeat = (input_repeat_state_t) {
        .active = true,
        .source = event->source,
        .physical_key = event->physical_key,
        .next_ms = input_now_ms() + input_repeat_delay_ms,
    };
}

static void input_queue_repeat_if_due_locked(void)
{
    if (!input_repeat.active || input_repeat_rate_cps == 0 ||
        (int32_t)(input_now_ms() - input_repeat.next_ms) < 0) {
        return;
    }

    input_pressed_slot_t *pressed =
        input_find_pressed_locked(input_repeat.source, input_repeat.physical_key);
    if (pressed == NULL) {
        memset(&input_repeat, 0, sizeof(input_repeat));
        return;
    }

    solar_os_input_key_event_t event = pressed->event;
    event.action = SOLAR_OS_INPUT_KEY_REPEAT;
    if (input_queue_push_locked(&event)) {
        input_repeat.next_ms = input_now_ms() +
            input_repeat_interval_ms(input_repeat_rate_cps);
    }
}

static esp_err_t input_load_repeat_namespace(const char *namespace_name,
                                             uint16_t *rate_cps,
                                             uint16_t *delay_ms)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u16(nvs, INPUT_NVS_REPEAT_RATE_KEY, rate_cps);
    if (err == ESP_OK) {
        err = nvs_get_u16(nvs, INPUT_NVS_REPEAT_DELAY_KEY, delay_ms);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t input_load_layout_namespace(const char *namespace_name,
                                             uint16_t *layout)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u16(nvs, INPUT_NVS_LAYOUT_KEY, layout);
    nvs_close(nvs);
    return err;
}

esp_err_t solar_os_input_init(void)
{
    uint16_t rate_cps = INPUT_REPEAT_RATE_DEFAULT;
    uint16_t delay_ms = INPUT_REPEAT_DELAY_DEFAULT_MS;
    esp_err_t repeat_err = input_load_repeat_namespace(INPUT_NVS_NAMESPACE,
                                                       &rate_cps,
                                                       &delay_ms);
    if (repeat_err == ESP_ERR_NVS_NOT_FOUND) {
        repeat_err = input_load_repeat_namespace(INPUT_LEGACY_NVS_NAMESPACE,
                                                 &rate_cps,
                                                 &delay_ms);
    }
    if (repeat_err == ESP_ERR_NVS_NOT_FOUND) {
        repeat_err = ESP_OK;
    }
    if (repeat_err == ESP_OK && !input_repeat_config_valid(rate_cps, delay_ms)) {
        repeat_err = ESP_ERR_INVALID_ARG;
    }

    uint16_t layout_value = SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US;
    esp_err_t layout_err = input_load_layout_namespace(INPUT_NVS_NAMESPACE,
                                                       &layout_value);
    if (layout_err == ESP_ERR_NVS_NOT_FOUND) {
        layout_err = input_load_layout_namespace(INPUT_LEGACY_NVS_NAMESPACE,
                                                 &layout_value);
    }
    if (layout_err == ESP_ERR_NVS_NOT_FOUND) {
        layout_err = ESP_OK;
    }
    if (layout_err == ESP_OK &&
        layout_value >= sizeof(input_keyboard_layout_names) /
            sizeof(input_keyboard_layout_names[0])) {
        layout_err = ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&input_lock);
    if (repeat_err == ESP_OK) {
        input_repeat_rate_cps = rate_cps;
        input_repeat_delay_ms = delay_ms;
    }
    if (layout_err == ESP_OK) {
        input_keyboard_layout = (solar_os_input_keyboard_layout_t)layout_value;
    }
    portEXIT_CRITICAL(&input_lock);
    return repeat_err != ESP_OK ? repeat_err : layout_err;
}

static esp_err_t input_source_open(const char *name,
                                   bool keyboard,
                                   bool ready,
                                   solar_os_input_source_t *source)
{
    if (name == NULL || name[0] == '\0' || source == NULL ||
        strlen(name) >= INPUT_SOURCE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&input_lock);
    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (input_sources[i].active && strcmp(input_sources[i].name, name) == 0) {
            if (input_sources[i].keyboard == keyboard) {
                input_sources[i].ready = ready;
                *source = (solar_os_input_source_t)(i + 1U);
                result = ESP_OK;
            } else {
                result = ESP_ERR_INVALID_STATE;
            }
            break;
        }
    }
    if (result != ESP_OK) {
        for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
            if (input_sources[i].active) {
                continue;
            }
            input_sources[i].active = true;
            input_sources[i].keyboard = keyboard;
            input_sources[i].ready = ready;
            strlcpy(input_sources[i].name, name, sizeof(input_sources[i].name));
            *source = (solar_os_input_source_t)(i + 1U);
            result = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_source_open(const char *name, solar_os_input_source_t *source)
{
    return input_source_open(name, false, true, source);
}

esp_err_t solar_os_input_keyboard_source_open(const char *name,
                                              bool ready,
                                              solar_os_input_source_t *source)
{
    return input_source_open(name, true, ready, source);
}

esp_err_t solar_os_input_keyboard_source_set_ready(solar_os_input_source_t source,
                                                   bool ready)
{
    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) || !input_sources[source - 1U].keyboard) {
        result = ESP_ERR_INVALID_ARG;
    } else {
        input_sources[source - 1U].ready = ready;
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

size_t solar_os_input_keyboard_count(void)
{
    size_t count = 0;
    portENTER_CRITICAL(&input_lock);
    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (input_sources[i].active && input_sources[i].keyboard && input_sources[i].ready) {
            count++;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

void solar_os_input_source_release_all(solar_os_input_source_t source)
{
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        portEXIT_CRITICAL(&input_lock);
        return;
    }

    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (!input_pressed[i].active || input_pressed[i].event.source != source) {
            continue;
        }
        solar_os_input_key_event_t release = input_pressed[i].event;
        release.action = SOLAR_OS_INPUT_KEY_RELEASE;
        (void)input_queue_push_locked(&release);
        memset(&input_pressed[i], 0, sizeof(input_pressed[i]));
    }
    if (input_repeat.active && input_repeat.source == source) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
    portEXIT_CRITICAL(&input_lock);
}

void solar_os_input_source_close(solar_os_input_source_t source)
{
    if (source == SOLAR_OS_INPUT_SOURCE_INVALID || source > INPUT_SOURCE_MAX) {
        return;
    }

    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        portEXIT_CRITICAL(&input_lock);
        return;
    }

    solar_os_input_key_event_t retained[INPUT_QUEUE_MAX];
    size_t kept = 0;
    for (size_t i = 0; i < input_queue_count; i++) {
        const size_t read_index = (input_queue_head + i) % INPUT_QUEUE_MAX;
        if (input_queue[read_index].source != source) {
            retained[kept++] = input_queue[read_index];
        }
    }
    memcpy(input_queue, retained, kept * sizeof(retained[0]));
    input_queue_head = 0;
    input_queue_count = kept;
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (input_pressed[i].active && input_pressed[i].event.source == source) {
            memset(&input_pressed[i], 0, sizeof(input_pressed[i]));
        }
    }
    if (input_repeat.active && input_repeat.source == source) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
    memset(&input_sources[source - 1U], 0, sizeof(input_sources[source - 1U]));
    portEXIT_CRITICAL(&input_lock);
}

esp_err_t solar_os_input_write_key(solar_os_input_source_t source,
                                   uint16_t physical_key,
                                   uint16_t usage,
                                   uint8_t key,
                                   uint8_t modifiers,
                                   solar_os_input_key_action_t action)
{
    if (physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE ||
        action > SOLAR_OS_INPUT_KEY_REPEAT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        result = ESP_ERR_INVALID_STATE;
    } else if (action == SOLAR_OS_INPUT_KEY_PRESS) {
        input_pressed_slot_t *pressed = input_find_pressed_locked(source, physical_key);
        if (pressed == NULL) {
            pressed = input_alloc_pressed_locked();
        }
        if (pressed == NULL) {
            result = ESP_ERR_NO_MEM;
        } else if (!pressed->active) {
            pressed->active = true;
            pressed->event = (solar_os_input_key_event_t) {
                .source = source,
                .physical_key = physical_key,
                .usage = usage,
                .key = key,
                .modifiers = modifiers,
                .action = SOLAR_OS_INPUT_KEY_PRESS,
            };
            if (!input_queue_push_locked(&pressed->event)) {
                result = ESP_ERR_NO_MEM;
            }
            input_repeat_start_locked(&pressed->event);
        }
    } else if (action == SOLAR_OS_INPUT_KEY_RELEASE) {
        input_pressed_slot_t *pressed = input_find_pressed_locked(source, physical_key);
        if (pressed != NULL) {
            solar_os_input_key_event_t release = pressed->event;
            release.action = SOLAR_OS_INPUT_KEY_RELEASE;
            release.modifiers = modifiers;
            memset(pressed, 0, sizeof(*pressed));
            input_repeat_stop_locked(source, physical_key);
            if (!input_queue_push_locked(&release)) {
                result = ESP_ERR_NO_MEM;
            }
        }
    } else {
        input_pressed_slot_t *pressed = input_find_pressed_locked(source, physical_key);
        if (pressed == NULL) {
            result = ESP_ERR_NOT_FOUND;
        } else {
            solar_os_input_key_event_t repeat = pressed->event;
            repeat.action = SOLAR_OS_INPUT_KEY_REPEAT;
            repeat.modifiers = modifiers;
            if (!input_queue_push_locked(&repeat)) {
                result = ESP_ERR_NO_MEM;
            }
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_write_char(solar_os_input_source_t source, char ch)
{
    solar_os_input_key_event_t press = {
        .source = source,
        .physical_key = SOLAR_OS_INPUT_PHYSICAL_NONE,
        .usage = SOLAR_OS_INPUT_USAGE_NONE,
        .key = (uint8_t)ch,
        .modifiers = 0,
        .action = SOLAR_OS_INPUT_KEY_PRESS,
    };
    solar_os_input_key_event_t release = press;
    release.action = SOLAR_OS_INPUT_KEY_RELEASE;

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        result = ESP_ERR_INVALID_STATE;
    } else if (input_queue_count > INPUT_QUEUE_MAX - 2U) {
        result = ESP_ERR_NO_MEM;
    } else {
        (void)input_queue_push_locked(&press);
        (void)input_queue_push_locked(&release);
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

size_t solar_os_input_read_events(solar_os_input_key_event_t *events, size_t event_count)
{
    if (events == NULL || event_count == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    input_queue_repeat_if_due_locked();
    size_t count = 0;
    while (count < event_count && input_queue_count > 0) {
        events[count++] = input_queue[input_queue_head];
        input_queue_head = (input_queue_head + 1U) % INPUT_QUEUE_MAX;
        input_queue_count--;
    }
    if (input_queue_count == 0) {
        input_queue_head = 0;
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

static size_t input_read_chars_for_source(solar_os_input_source_t source,
                                          bool filter_source,
                                          char *buffer,
                                          size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    input_queue_repeat_if_due_locked();
    solar_os_input_key_event_t retained[INPUT_QUEUE_MAX];
    size_t kept = 0;
    size_t count = 0;
    for (size_t i = 0; i < input_queue_count; i++) {
        const size_t read_index = (input_queue_head + i) % INPUT_QUEUE_MAX;
        const solar_os_input_key_event_t event = input_queue[read_index];
        const bool selected = !filter_source || event.source == source;
        const bool emits_char =
            (event.action == SOLAR_OS_INPUT_KEY_PRESS ||
             event.action == SOLAR_OS_INPUT_KEY_REPEAT) &&
            event.key != 0;
        const bool emits_alt_prefix = emits_char &&
            ((((event.modifiers & SOLAR_OS_INPUT_MOD_ALT) != 0) && event.key == '\t') ||
             (((event.modifiers & SOLAR_OS_INPUT_MOD_LEFT_ALT) != 0) &&
              event.key != SOLAR_OS_KEY_APP_EXIT));
        const size_t needed = emits_char ? (emits_alt_prefix ? 2U : 1U) : 0U;
        if (!selected || count + needed > buffer_len) {
            retained[kept++] = event;
            continue;
        }
        if (emits_alt_prefix) {
            buffer[count++] = (char)SOLAR_OS_KEY_ALT_PREFIX;
        }
        if (emits_char) {
            buffer[count++] = (char)event.key;
        }
    }
    memcpy(input_queue, retained, kept * sizeof(retained[0]));
    input_queue_head = 0;
    input_queue_count = kept;
    portEXIT_CRITICAL(&input_lock);
    return count;
}

size_t solar_os_input_read_chars(char *buffer, size_t buffer_len)
{
    return input_read_chars_for_source(SOLAR_OS_INPUT_SOURCE_INVALID,
                                       false,
                                       buffer,
                                       buffer_len);
}

size_t solar_os_input_read_source_chars(solar_os_input_source_t source,
                                        char *buffer,
                                        size_t buffer_len)
{
    return input_read_chars_for_source(source, true, buffer, buffer_len);
}

size_t solar_os_input_get_pressed(solar_os_input_key_event_t *keys, size_t key_count)
{
    if (keys == NULL || key_count == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS && count < key_count; i++) {
        if (input_pressed[i].active) {
            keys[count++] = input_pressed[i].event;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

solar_os_input_keyboard_layout_t solar_os_input_keyboard_layout(void)
{
    portENTER_CRITICAL(&input_lock);
    const solar_os_input_keyboard_layout_t layout = input_keyboard_layout;
    portEXIT_CRITICAL(&input_lock);
    return layout;
}

esp_err_t solar_os_input_set_keyboard_layout(solar_os_input_keyboard_layout_t layout)
{
    if ((size_t)layout >= sizeof(input_keyboard_layout_names) /
            sizeof(input_keyboard_layout_names[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&input_lock);
    input_keyboard_layout = layout;
    portEXIT_CRITICAL(&input_lock);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, INPUT_NVS_LAYOUT_KEY, (uint16_t)layout);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

const char *solar_os_input_keyboard_layout_name(solar_os_input_keyboard_layout_t layout)
{
    if ((size_t)layout >= sizeof(input_keyboard_layout_names) /
            sizeof(input_keyboard_layout_names[0])) {
        return "unknown";
    }
    return input_keyboard_layout_names[layout];
}

bool solar_os_input_parse_keyboard_layout(const char *name,
                                          solar_os_input_keyboard_layout_t *layout)
{
    if (name == NULL || layout == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(input_keyboard_layout_names) /
             sizeof(input_keyboard_layout_names[0]); i++) {
        if (strcmp(name, input_keyboard_layout_names[i]) == 0) {
            *layout = (solar_os_input_keyboard_layout_t)i;
            return true;
        }
    }
    return false;
}

static uint8_t input_shifted_digit(uint16_t usage)
{
    static const uint8_t shifted[] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
    return shifted[usage - 0x1eU];
}

static uint8_t input_unshifted_digit(uint16_t usage)
{
    return usage == 0x27U ? '0' : (uint8_t)('1' + usage - 0x1eU);
}

static uint8_t input_usage_to_us(uint16_t usage, bool shift, bool caps_lock)
{
    if (usage >= 0x04U && usage <= 0x1dU) {
        const bool upper = shift ^ caps_lock;
        return (uint8_t)((upper ? 'A' : 'a') + usage - 0x04U);
    }
    if (usage >= 0x1eU && usage <= 0x27U) {
        return shift ? input_shifted_digit(usage) : input_unshifted_digit(usage);
    }
    switch (usage) {
    case 0x28: return '\n';
    case 0x29: return SOLAR_OS_KEY_ESCAPE;
    case 0x2a: return '\b';
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x32: return shift ? '~' : '#';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}

static uint8_t input_usage_to_de(uint16_t usage,
                                 uint8_t modifiers,
                                 bool caps_lock)
{
    const bool shift = (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0;
    const bool altgr = (modifiers & SOLAR_OS_INPUT_MOD_RIGHT_ALT) != 0;
    if (altgr) {
        switch (usage) {
        case 0x2b: return '\t';
        case 0x14: return '@';
        case 0x24: return '{';
        case 0x25: return '[';
        case 0x26: return ']';
        case 0x27: return '}';
        case 0x2d: return '\\';
        case 0x30: return '~';
        case 0x64: return '|';
        default: return 0;
        }
    }
    if (usage >= 0x04U && usage <= 0x1dU) {
        uint8_t base = (uint8_t)('a' + usage - 0x04U);
        if (base == 'y') {
            base = 'z';
        } else if (base == 'z') {
            base = 'y';
        }
        return (shift ^ caps_lock) ? (uint8_t)toupper(base) : base;
    }
    if (usage >= 0x1eU && usage <= 0x27U) {
        static const uint8_t shifted[] = {'!', '"', '#', '$', '%', '&', '/', '(', ')', '='};
        return shift ? shifted[usage - 0x1eU] : input_unshifted_digit(usage);
    }
    switch (usage) {
    case 0x28: return '\n';
    case 0x29: return SOLAR_OS_KEY_ESCAPE;
    case 0x2a: return '\b';
    case 0x2b: return '\t';
    case 0x2c: return ' ';
    case 0x2d: return shift ? '?' : INPUT_LATIN1_SHARP_S;
    case 0x2e: return shift ? '`' : 0;
    case 0x2f: return shift ? INPUT_LATIN1_U_UMLAUT_UPPER : INPUT_LATIN1_U_UMLAUT_LOWER;
    case 0x30: return shift ? '*' : '+';
    case 0x31:
    case 0x32: return shift ? '\'' : '#';
    case 0x33: return shift ? INPUT_LATIN1_O_UMLAUT_UPPER : INPUT_LATIN1_O_UMLAUT_LOWER;
    case 0x34: return shift ? INPUT_LATIN1_A_UMLAUT_UPPER : INPUT_LATIN1_A_UMLAUT_LOWER;
    case 0x35: return shift ? 0 : '^';
    case 0x36: return shift ? ';' : ',';
    case 0x37: return shift ? ':' : '.';
    case 0x38: return shift ? '_' : '-';
    case 0x64: return shift ? '>' : '<';
    default: return 0;
    }
}

static uint8_t input_usage_to_function_key(uint16_t usage)
{
    if (usage >= 0x3aU && usage <= 0x45U) {
        return (uint8_t)(SOLAR_OS_KEY_F1 + usage - 0x3aU);
    }
    return 0;
}

static uint8_t input_usage_to_nav_key(uint16_t usage, uint8_t modifiers)
{
    const bool ctrl = (modifiers & SOLAR_OS_INPUT_MOD_CTRL) != 0;
    const bool shift = (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0;
    switch (usage) {
    case 0x4a:
        return ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_HOME : SOLAR_OS_KEY_CTRL_HOME) :
            (shift ? SOLAR_OS_KEY_SHIFT_HOME : SOLAR_OS_KEY_HOME);
    case 0x4b: return shift ? SOLAR_OS_KEY_SHIFT_PAGE_UP : SOLAR_OS_KEY_PAGE_UP;
    case 0x4c: return SOLAR_OS_KEY_DELETE;
    case 0x4d:
        return ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_END : SOLAR_OS_KEY_CTRL_END) :
            (shift ? SOLAR_OS_KEY_SHIFT_END : SOLAR_OS_KEY_END);
    case 0x4e: return shift ? SOLAR_OS_KEY_SHIFT_PAGE_DOWN : SOLAR_OS_KEY_PAGE_DOWN;
    case 0x4f:
        return ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_RIGHT : SOLAR_OS_KEY_CTRL_RIGHT) :
            (shift ? SOLAR_OS_KEY_SHIFT_RIGHT : SOLAR_OS_KEY_RIGHT);
    case 0x50:
        return ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_LEFT : SOLAR_OS_KEY_CTRL_LEFT) :
            (shift ? SOLAR_OS_KEY_SHIFT_LEFT : SOLAR_OS_KEY_LEFT);
    case 0x51:
        return ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_DOWN : SOLAR_OS_KEY_CTRL_DOWN) :
            (shift ? SOLAR_OS_KEY_SHIFT_DOWN : SOLAR_OS_KEY_DOWN);
    case 0x52:
        return ctrl ? (shift ? SOLAR_OS_KEY_CTRL_SHIFT_UP : SOLAR_OS_KEY_CTRL_UP) :
            (shift ? SOLAR_OS_KEY_SHIFT_UP : SOLAR_OS_KEY_UP);
    default: return 0;
    }
}

static uint8_t input_usage_to_control(uint16_t usage,
                                      solar_os_input_keyboard_layout_t layout)
{
    if (usage >= 0x04U && usage <= 0x1dU) {
        uint8_t base = (uint8_t)('a' + usage - 0x04U);
        if (layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) {
            if (base == 'y') {
                base = 'z';
            } else if (base == 'z') {
                base = 'y';
            }
        }
        return (uint8_t)(base - 'a' + 1U);
    }
    switch (usage) {
    case 0x23: return 0x1e;
    case 0x2d: return 0x1f;
    case 0x30: return 0x1d;
    case 0x31: return 0x1c;
    default: return 0;
    }
}

uint8_t solar_os_input_translate_hid_usage(uint16_t usage,
                                           uint8_t modifiers,
                                           bool caps_lock)
{
    const solar_os_input_keyboard_layout_t layout = solar_os_input_keyboard_layout();
    const bool ctrl = (modifiers & SOLAR_OS_INPUT_MOD_CTRL) != 0;
    const bool alt = (modifiers & SOLAR_OS_INPUT_MOD_ALT) != 0;

    if (ctrl && alt && usage == 0x4cU) {
        return SOLAR_OS_KEY_APP_EXIT;
    }
    if (ctrl && !alt) {
        if (layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US && usage == 0x30U) {
            return SOLAR_OS_KEY_APP_EXIT;
        }
        if (usage == 0x2eU ||
            (layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE && usage == 0x30U)) {
            return SOLAR_OS_KEY_CTRL_PLUS;
        }
        if (layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE && usage == 0x38U) {
            return 0x1f;
        }
    }

    uint8_t key = input_usage_to_function_key(usage);
    if (key == 0) {
        key = input_usage_to_nav_key(usage, modifiers);
    }
    if (key == 0 && ctrl) {
        key = input_usage_to_control(usage, layout);
    }
    if (key != 0) {
        return key;
    }
    if (layout == SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE) {
        return input_usage_to_de(usage, modifiers, caps_lock);
    }
    return input_usage_to_us(usage,
                             (modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0,
                             caps_lock);
}

void solar_os_input_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms)
{
    portENTER_CRITICAL(&input_lock);
    if (rate_cps != NULL) {
        *rate_cps = input_repeat_rate_cps;
    }
    if (delay_ms != NULL) {
        *delay_ms = input_repeat_delay_ms;
    }
    portEXIT_CRITICAL(&input_lock);
}

esp_err_t solar_os_input_set_repeat(uint16_t rate_cps, uint16_t delay_ms)
{
    if (delay_ms == 0) {
        solar_os_input_get_repeat(NULL, &delay_ms);
    }
    if (!input_repeat_config_valid(rate_cps, delay_ms)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&input_lock);
    input_repeat_rate_cps = rate_cps;
    input_repeat_delay_ms = delay_ms;
    if (rate_cps == 0) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
    portEXIT_CRITICAL(&input_lock);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u16(nvs, INPUT_NVS_REPEAT_RATE_KEY, rate_cps);
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, INPUT_NVS_REPEAT_DELAY_KEY, delay_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
