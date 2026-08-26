#include "solar_os_input.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "solar_os_keys.h"
#include "solar_os_memory.h"

#define INPUT_SOURCE_MAX 8U
#define INPUT_QUEUE_MAX 64U
#define INPUT_POINTER_QUEUE_MAX 32U
#define INPUT_AXIS_QUEUE_MAX 32U
#define INPUT_REPEAT_RATE_DEFAULT 15U
#define INPUT_REPEAT_DELAY_DEFAULT_MS 450U
#define INPUT_NVS_NAMESPACE "input"
#define INPUT_NVS_REPEAT_RATE_KEY "repeat_cps"
#define INPUT_NVS_REPEAT_DELAY_KEY "repeat_delay"
#define INPUT_NVS_LAYOUT_KEY "layout"
#define INPUT_NVS_CALIBRATION_VERSION 1U
#define INPUT_LEGACY_NVS_NAMESPACE "blekbd"
#define INPUT_POINTER_CAPABILITIES \
    (SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE | SOLAR_OS_INPUT_CAP_POINTER_RELATIVE)

#define INPUT_LATIN1_A_UMLAUT_UPPER ((uint8_t)0xc4)
#define INPUT_LATIN1_O_UMLAUT_UPPER ((uint8_t)0xd6)
#define INPUT_LATIN1_U_UMLAUT_UPPER ((uint8_t)0xdc)
#define INPUT_LATIN1_SHARP_S ((uint8_t)0xdf)
#define INPUT_LATIN1_A_UMLAUT_LOWER ((uint8_t)0xe4)
#define INPUT_LATIN1_O_UMLAUT_LOWER ((uint8_t)0xf6)
#define INPUT_LATIN1_U_UMLAUT_LOWER ((uint8_t)0xfc)

typedef struct {
    uint32_t key_events;
    uint32_t pointer_events;
    uint32_t axis_events;
    bool has_key;
    bool has_pointer;
    bool has_axis;
    solar_os_input_key_event_t last_key;
    struct {
        solar_os_input_pointer_event_t event;
        int16_t raw_x;
        int16_t raw_y;
        int16_t raw_delta_x;
        int16_t raw_delta_y;
    } last_pointer;
    solar_os_input_axis_event_t last_axis;
    bool calibration_enabled;
    solar_os_input_pointer_calibration_t calibration;
} input_source_diagnostics_state_t;

typedef struct {
    bool active;
    solar_os_input_source_class_t source_class;
    uint32_t capabilities;
    bool ready;
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
} input_source_slot_t;

typedef struct {
    uint16_t version;
    char source_name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    solar_os_input_pointer_calibration_t calibration;
} input_calibration_record_t;

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
static EXT_RAM_BSS_ATTR input_source_diagnostics_state_t
    input_diagnostics[INPUT_SOURCE_MAX];
static input_pressed_slot_t input_pressed[SOLAR_OS_INPUT_MAX_PRESSED_KEYS];
static solar_os_input_key_event_t input_queue[INPUT_QUEUE_MAX];
static size_t input_queue_head;
static size_t input_queue_count;
static solar_os_input_pointer_event_t *input_pointer_queue;
static size_t input_pointer_queue_head;
static size_t input_pointer_queue_count;
static solar_os_input_axis_event_t *input_axis_queue;
static size_t input_axis_queue_head;
static size_t input_axis_queue_count;
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

static const char *const input_source_class_names[] = {
    [SOLAR_OS_INPUT_SOURCE_OTHER] = "other",
    [SOLAR_OS_INPUT_SOURCE_KEYBOARD] = "keyboard",
    [SOLAR_OS_INPUT_SOURCE_TOUCH] = "touch",
    [SOLAR_OS_INPUT_SOURCE_MOUSE] = "mouse",
    [SOLAR_OS_INPUT_SOURCE_JOYSTICK] = "joystick",
    [SOLAR_OS_INPUT_SOURCE_DPAD] = "dpad",
    [SOLAR_OS_INPUT_SOURCE_BUTTONS] = "buttons",
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

static uint32_t input_source_name_hash(const char *name)
{
    uint32_t hash = 2166136261U;
    for (const unsigned char *cursor = (const unsigned char *)name;
         *cursor != '\0'; cursor++) {
        hash ^= *cursor;
        hash *= 16777619U;
    }
    return hash;
}

static void input_calibration_key(const char *name, char key[12])
{
    (void)snprintf(key, 12, "cal%08lx", (unsigned long)input_source_name_hash(name));
}

static bool input_calibration_valid(
    const solar_os_input_pointer_calibration_t *calibration)
{
    return calibration != NULL &&
        calibration->min_x < calibration->max_x &&
        calibration->min_y < calibration->max_y &&
        calibration->width > 0 && calibration->width <= 32768U &&
        calibration->height > 0 && calibration->height <= 32768U;
}

static esp_err_t input_calibration_load(
    const char *name,
    solar_os_input_pointer_calibration_t *calibration)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    char key[12];
    input_calibration_key(name, key);
    input_calibration_record_t record = {0};
    size_t length = sizeof(record);
    err = nvs_get_blob(nvs, key, &record, &length);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(record) ||
        record.version != INPUT_NVS_CALIBRATION_VERSION ||
        record.source_name[SOLAR_OS_INPUT_SOURCE_NAME_MAX - 1U] != '\0' ||
        strcmp(record.source_name, name) != 0 ||
        !input_calibration_valid(&record.calibration)) {
        return ESP_ERR_INVALID_STATE;
    }
    *calibration = record.calibration;
    return ESP_OK;
}

static esp_err_t input_calibration_store(
    const char *name,
    const solar_os_input_pointer_calibration_t *calibration)
{
    input_calibration_record_t record = {
        .version = INPUT_NVS_CALIBRATION_VERSION,
        .calibration = *calibration,
    };
    strlcpy(record.source_name, name, sizeof(record.source_name));
    char key[12];
    input_calibration_key(name, key);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, key, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t input_calibration_erase(const char *name)
{
    char key[12];
    input_calibration_key(name, key);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static int16_t input_calibrate_coordinate(int32_t value,
                                          int16_t minimum,
                                          int16_t maximum,
                                          uint16_t size)
{
    if (value <= minimum) {
        return 0;
    }
    if (value >= maximum) {
        return (int16_t)(size - 1U);
    }
    const int32_t range = (int32_t)maximum - minimum;
    const int64_t scaled = (int64_t)(value - minimum) * (size - 1U);
    return (int16_t)((scaled + range / 2) / range);
}

static void input_apply_calibration_locked(
    const input_source_diagnostics_state_t *diagnostics,
    solar_os_input_pointer_event_t *event)
{
    if (diagnostics == NULL || event == NULL ||
        event->mode != SOLAR_OS_INPUT_POINTER_ABSOLUTE ||
        !diagnostics->calibration_enabled) {
        return;
    }
    const solar_os_input_pointer_calibration_t *calibration =
        &diagnostics->calibration;
    const int32_t previous_x = (int32_t)event->x - event->delta_x;
    const int32_t previous_y = (int32_t)event->y - event->delta_y;
    const int16_t calibrated_x = input_calibrate_coordinate(
        event->x, calibration->min_x, calibration->max_x, calibration->width);
    const int16_t calibrated_y = input_calibrate_coordinate(
        event->y, calibration->min_y, calibration->max_y, calibration->height);
    const int16_t previous_calibrated_x = input_calibrate_coordinate(
        previous_x, calibration->min_x, calibration->max_x, calibration->width);
    const int16_t previous_calibrated_y = input_calibrate_coordinate(
        previous_y, calibration->min_y, calibration->max_y, calibration->height);
    event->x = calibrated_x;
    event->y = calibrated_y;
    event->delta_x = calibrated_x - previous_calibrated_x;
    event->delta_y = calibrated_y - previous_calibrated_y;
}

static void input_increment_counter(uint32_t *counter)
{
    if (*counter != UINT32_MAX) {
        (*counter)++;
    }
}

static void input_record_key_locked(const solar_os_input_key_event_t *event)
{
    input_source_diagnostics_state_t *diagnostics =
        &input_diagnostics[event->source - 1U];
    input_increment_counter(&diagnostics->key_events);
    diagnostics->has_key = true;
    diagnostics->last_key = *event;
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
        key != '\t' &&
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

static bool input_pointer_queue_push_locked(const solar_os_input_pointer_event_t *event)
{
    if (event == NULL || input_pointer_queue == NULL ||
        input_pointer_queue_count >= INPUT_POINTER_QUEUE_MAX) {
        return false;
    }
    const size_t index =
        (input_pointer_queue_head + input_pointer_queue_count) % INPUT_POINTER_QUEUE_MAX;
    input_pointer_queue[index] = *event;
    input_pointer_queue_count++;
    return true;
}

static bool input_axis_queue_push_locked(const solar_os_input_axis_event_t *event)
{
    if (event == NULL || input_axis_queue == NULL) {
        return false;
    }
    for (size_t i = 0; i < input_axis_queue_count; i++) {
        const size_t index = (input_axis_queue_head + i) % INPUT_AXIS_QUEUE_MAX;
        if (input_axis_queue[index].source == event->source &&
            input_axis_queue[index].axis == event->axis) {
            input_axis_queue[index].value = event->value;
            input_axis_queue[index].delta += event->delta;
            return true;
        }
    }
    if (input_axis_queue_count >= INPUT_AXIS_QUEUE_MAX) {
        return false;
    }
    const size_t index =
        (input_axis_queue_head + input_axis_queue_count) % INPUT_AXIS_QUEUE_MAX;
    input_axis_queue[index] = *event;
    input_axis_queue_count++;
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
        input_record_key_locked(&event);
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

static bool input_source_open_args_valid(const char *name,
                                         solar_os_input_source_class_t source_class,
                                         uint32_t capabilities,
                                         const solar_os_input_source_t *source)
{
    return name != NULL && name[0] != '\0' && source != NULL &&
        strlen(name) < SOLAR_OS_INPUT_SOURCE_NAME_MAX &&
        source_class >= SOLAR_OS_INPUT_SOURCE_OTHER &&
        source_class < SOLAR_OS_INPUT_SOURCE_CLASS_COUNT &&
        capabilities != 0;
}

static esp_err_t input_source_open_locked(const char *name,
                                          solar_os_input_source_class_t source_class,
                                          uint32_t capabilities,
                                          bool ready,
                                          solar_os_input_source_t *source)
{
    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (input_sources[i].active && strcmp(input_sources[i].name, name) == 0) {
            if (input_sources[i].source_class != source_class ||
                input_sources[i].capabilities != capabilities) {
                return ESP_ERR_INVALID_STATE;
            }
            input_sources[i].ready = ready;
            *source = (solar_os_input_source_t)(i + 1U);
            return ESP_OK;
        }
    }

    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (input_sources[i].active) {
            continue;
        }
        input_sources[i].active = true;
        input_sources[i].source_class = source_class;
        input_sources[i].capabilities = capabilities;
        input_sources[i].ready = ready;
        strlcpy(input_sources[i].name, name, sizeof(input_sources[i].name));
        memset(&input_diagnostics[i], 0, sizeof(input_diagnostics[i]));
        *source = (solar_os_input_source_t)(i + 1U);
        return ESP_OK;
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_input_source_open_typed(const char *name,
                                           solar_os_input_source_class_t source_class,
                                           uint32_t capabilities,
                                           bool ready,
                                           solar_os_input_source_t *source)
{
    if (!input_source_open_args_valid(name, source_class, capabilities, source)) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_input_pointer_calibration_t calibration = {0};
    const bool calibration_enabled =
        (capabilities & SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) != 0 &&
        input_calibration_load(name, &calibration) == ESP_OK;

    solar_os_input_pointer_event_t *pointer_candidate = NULL;
    solar_os_input_axis_event_t *axis_candidate = NULL;
    for (;;) {
        portENTER_CRITICAL(&input_lock);
        const bool needs_pointer_queue =
            (capabilities & INPUT_POINTER_CAPABILITIES) != 0;
        const bool needs_axis_queue =
            (capabilities & SOLAR_OS_INPUT_CAP_AXIS_EVENTS) != 0;
        const bool pointer_ready = !needs_pointer_queue ||
            input_pointer_queue != NULL || pointer_candidate != NULL;
        const bool axis_ready = !needs_axis_queue ||
            input_axis_queue != NULL || axis_candidate != NULL;
        if (pointer_ready && axis_ready) {
            const esp_err_t result = input_source_open_locked(name,
                                                              source_class,
                                                              capabilities,
                                                              ready,
                                                              source);
            if (result == ESP_OK && needs_pointer_queue &&
                input_pointer_queue == NULL) {
                input_pointer_queue = pointer_candidate;
                pointer_candidate = NULL;
            }
            if (result == ESP_OK && needs_axis_queue && input_axis_queue == NULL) {
                input_axis_queue = axis_candidate;
                axis_candidate = NULL;
            }
            if (result == ESP_OK &&
                (capabilities & SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) != 0) {
                input_diagnostics[*source - 1U].calibration_enabled =
                    calibration_enabled;
                input_diagnostics[*source - 1U].calibration = calibration;
            }
            portEXIT_CRITICAL(&input_lock);
            solar_os_memory_free(pointer_candidate);
            solar_os_memory_free(axis_candidate);
            return result;
        }
        portEXIT_CRITICAL(&input_lock);

        if (!pointer_ready) {
            pointer_candidate = solar_os_memory_calloc(
                INPUT_POINTER_QUEUE_MAX,
                sizeof(*pointer_candidate),
                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                "input-pointer");
            if (pointer_candidate == NULL) {
                solar_os_memory_free(axis_candidate);
                return ESP_ERR_NO_MEM;
            }
        }
        if (!axis_ready) {
            axis_candidate = solar_os_memory_calloc(
                INPUT_AXIS_QUEUE_MAX,
                sizeof(*axis_candidate),
                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                "input-axis");
            if (axis_candidate == NULL) {
                solar_os_memory_free(pointer_candidate);
                return ESP_ERR_NO_MEM;
            }
        }
    }
}

esp_err_t solar_os_input_source_open(const char *name, solar_os_input_source_t *source)
{
    return solar_os_input_source_open_typed(name,
                                            SOLAR_OS_INPUT_SOURCE_OTHER,
                                            SOLAR_OS_INPUT_CAP_KEY_EVENTS,
                                            true,
                                            source);
}

esp_err_t solar_os_input_key_source_open(const char *name,
                                         solar_os_input_source_class_t source_class,
                                         solar_os_input_source_t *source)
{
    return solar_os_input_source_open_typed(name,
                                            source_class,
                                            SOLAR_OS_INPUT_CAP_KEY_EVENTS,
                                            true,
                                            source);
}

esp_err_t solar_os_input_pointer_source_open(const char *name,
                                             solar_os_input_source_t *source)
{
    return solar_os_input_source_open_typed(
        name,
        SOLAR_OS_INPUT_SOURCE_OTHER,
        INPUT_POINTER_CAPABILITIES | SOLAR_OS_INPUT_CAP_POINTER_BUTTONS,
        true,
        source);
}

esp_err_t solar_os_input_touch_source_open(const char *name,
                                           solar_os_input_source_t *source)
{
    return solar_os_input_source_open_typed(
        name,
        SOLAR_OS_INPUT_SOURCE_TOUCH,
        SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE | SOLAR_OS_INPUT_CAP_POINTER_BUTTONS,
        true,
        source);
}

esp_err_t solar_os_input_mouse_source_open(const char *name,
                                           solar_os_input_source_t *source)
{
    return solar_os_input_source_open_typed(
        name,
        SOLAR_OS_INPUT_SOURCE_MOUSE,
        SOLAR_OS_INPUT_CAP_POINTER_RELATIVE |
            SOLAR_OS_INPUT_CAP_POINTER_BUTTONS,
        true,
        source);
}

esp_err_t solar_os_input_joystick_source_open(const char *name,
                                              solar_os_input_source_t *source)
{
    return solar_os_input_source_open_typed(name,
                                            SOLAR_OS_INPUT_SOURCE_JOYSTICK,
                                            SOLAR_OS_INPUT_CAP_AXIS_EVENTS,
                                            true,
                                            source);
}

esp_err_t solar_os_input_keyboard_source_open(const char *name,
                                              bool ready,
                                              solar_os_input_source_t *source)
{
    return solar_os_input_source_open_typed(name,
                                            SOLAR_OS_INPUT_SOURCE_KEYBOARD,
                                            SOLAR_OS_INPUT_CAP_KEY_EVENTS,
                                            ready,
                                            source);
}

esp_err_t solar_os_input_source_set_ready(solar_os_input_source_t source,
                                          bool ready)
{
    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source)) {
        result = ESP_ERR_INVALID_ARG;
    } else {
        input_sources[source - 1U].ready = ready;
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_keyboard_source_set_ready(solar_os_input_source_t source,
                                                   bool ready)
{
    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        input_sources[source - 1U].source_class != SOLAR_OS_INPUT_SOURCE_KEYBOARD) {
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
        if (input_sources[i].active &&
            input_sources[i].source_class == SOLAR_OS_INPUT_SOURCE_KEYBOARD &&
            input_sources[i].ready) {
            count++;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

size_t solar_os_input_source_count(void)
{
    size_t count = 0;
    portENTER_CRITICAL(&input_lock);
    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (input_sources[i].active) {
            count++;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

bool solar_os_input_source_get(size_t index, solar_os_input_source_info_t *info)
{
    if (info == NULL) {
        return false;
    }

    bool found = false;
    portENTER_CRITICAL(&input_lock);
    size_t current = 0;
    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (!input_sources[i].active) {
            continue;
        }
        if (current++ != index) {
            continue;
        }
        *info = (solar_os_input_source_info_t) {
            .source = (solar_os_input_source_t)(i + 1U),
            .source_class = input_sources[i].source_class,
            .capabilities = input_sources[i].capabilities,
            .ready = input_sources[i].ready,
        };
        strlcpy(info->name, input_sources[i].name, sizeof(info->name));
        found = true;
        break;
    }
    portEXIT_CRITICAL(&input_lock);
    return found;
}

bool solar_os_input_source_find(const char *name, solar_os_input_source_info_t *info)
{
    if (name == NULL || info == NULL) {
        return false;
    }
    bool found = false;
    portENTER_CRITICAL(&input_lock);
    for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
        if (!input_sources[i].active || strcmp(input_sources[i].name, name) != 0) {
            continue;
        }
        *info = (solar_os_input_source_info_t) {
            .source = (solar_os_input_source_t)(i + 1U),
            .source_class = input_sources[i].source_class,
            .capabilities = input_sources[i].capabilities,
            .ready = input_sources[i].ready,
        };
        strlcpy(info->name, input_sources[i].name, sizeof(info->name));
        found = true;
        break;
    }
    portEXIT_CRITICAL(&input_lock);
    return found;
}

bool solar_os_input_source_get_diagnostics(
    solar_os_input_source_t source,
    solar_os_input_source_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) {
        return false;
    }
    bool found = false;
    portENTER_CRITICAL(&input_lock);
    if (input_source_valid_locked(source)) {
        const input_source_diagnostics_state_t *state =
            &input_diagnostics[source - 1U];
        *diagnostics = (solar_os_input_source_diagnostics_t) {
            .key_events = state->key_events,
            .pointer_events = state->pointer_events,
            .axis_events = state->axis_events,
            .has_key = state->has_key,
            .has_pointer = state->has_pointer,
            .has_axis = state->has_axis,
            .last_key = state->last_key,
            .last_pointer = state->last_pointer.event,
            .last_axis = state->last_axis,
            .calibration_enabled = state->calibration_enabled,
            .calibration = state->calibration,
        };
        diagnostics->last_pointer_raw = state->last_pointer.event;
        diagnostics->last_pointer_raw.x = state->last_pointer.raw_x;
        diagnostics->last_pointer_raw.y = state->last_pointer.raw_y;
        diagnostics->last_pointer_raw.delta_x = state->last_pointer.raw_delta_x;
        diagnostics->last_pointer_raw.delta_y = state->last_pointer.raw_delta_y;
        found = true;
    }
    portEXIT_CRITICAL(&input_lock);
    return found;
}

const char *solar_os_input_source_class_name(solar_os_input_source_class_t source_class)
{
    if (source_class < SOLAR_OS_INPUT_SOURCE_OTHER ||
        source_class >= SOLAR_OS_INPUT_SOURCE_CLASS_COUNT) {
        return "invalid";
    }
    return input_source_class_names[source_class];
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
        if (input_queue_push_locked(&release)) {
            input_record_key_locked(&release);
        }
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

    solar_os_input_pointer_event_t *pointer_queue_to_free = NULL;
    solar_os_input_axis_event_t *axis_queue_to_free = NULL;
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
    const size_t pointer_event_count = input_pointer_queue_count;
    for (size_t i = 0; i < pointer_event_count; i++) {
        const solar_os_input_pointer_event_t event =
            input_pointer_queue[input_pointer_queue_head];
        input_pointer_queue_head =
            (input_pointer_queue_head + 1U) % INPUT_POINTER_QUEUE_MAX;
        input_pointer_queue_count--;
        if (event.source != source) {
            (void)input_pointer_queue_push_locked(&event);
        }
    }
    if (input_pointer_queue_count == 0) {
        input_pointer_queue_head = 0;
    }
    const size_t axis_event_count = input_axis_queue_count;
    for (size_t i = 0; i < axis_event_count; i++) {
        const solar_os_input_axis_event_t event =
            input_axis_queue[input_axis_queue_head];
        input_axis_queue_head =
            (input_axis_queue_head + 1U) % INPUT_AXIS_QUEUE_MAX;
        input_axis_queue_count--;
        if (event.source != source) {
            (void)input_axis_queue_push_locked(&event);
        }
    }
    if (input_axis_queue_count == 0) {
        input_axis_queue_head = 0;
    }
    for (size_t i = 0; i < SOLAR_OS_INPUT_MAX_PRESSED_KEYS; i++) {
        if (input_pressed[i].active && input_pressed[i].event.source == source) {
            memset(&input_pressed[i], 0, sizeof(input_pressed[i]));
        }
    }
    if (input_repeat.active && input_repeat.source == source) {
        memset(&input_repeat, 0, sizeof(input_repeat));
    }
    const bool pointer_source =
        (input_sources[source - 1U].capabilities & INPUT_POINTER_CAPABILITIES) != 0;
    const bool axis_source =
        (input_sources[source - 1U].capabilities &
         SOLAR_OS_INPUT_CAP_AXIS_EVENTS) != 0;
    memset(&input_sources[source - 1U], 0, sizeof(input_sources[source - 1U]));
    memset(&input_diagnostics[source - 1U], 0, sizeof(input_diagnostics[source - 1U]));
    if (pointer_source) {
        bool another_pointer_source = false;
        for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
            if (input_sources[i].active &&
                (input_sources[i].capabilities & INPUT_POINTER_CAPABILITIES) != 0) {
                another_pointer_source = true;
                break;
            }
        }
        if (!another_pointer_source) {
            pointer_queue_to_free = input_pointer_queue;
            input_pointer_queue = NULL;
            input_pointer_queue_head = 0;
            input_pointer_queue_count = 0;
        }
    }
    if (axis_source) {
        bool another_axis_source = false;
        for (size_t i = 0; i < INPUT_SOURCE_MAX; i++) {
            if (input_sources[i].active &&
                (input_sources[i].capabilities &
                 SOLAR_OS_INPUT_CAP_AXIS_EVENTS) != 0) {
                another_axis_source = true;
                break;
            }
        }
        if (!another_axis_source) {
            axis_queue_to_free = input_axis_queue;
            input_axis_queue = NULL;
            input_axis_queue_head = 0;
            input_axis_queue_count = 0;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    solar_os_memory_free(pointer_queue_to_free);
    solar_os_memory_free(axis_queue_to_free);
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
    if (!input_source_valid_locked(source) ||
        (input_sources[source - 1U].capabilities &
         SOLAR_OS_INPUT_CAP_KEY_EVENTS) == 0) {
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
            } else {
                input_record_key_locked(&pressed->event);
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
            } else {
                input_record_key_locked(&release);
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
            } else {
                input_record_key_locked(&repeat);
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
    if (!input_source_valid_locked(source) ||
        (input_sources[source - 1U].capabilities &
         SOLAR_OS_INPUT_CAP_KEY_EVENTS) == 0) {
        result = ESP_ERR_INVALID_STATE;
    } else if (input_queue_count > INPUT_QUEUE_MAX - 2U) {
        result = ESP_ERR_NO_MEM;
    } else {
        (void)input_queue_push_locked(&press);
        (void)input_queue_push_locked(&release);
        input_record_key_locked(&press);
        input_record_key_locked(&release);
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_write_pointer(solar_os_input_source_t source,
                                       const solar_os_input_pointer_event_t *event)
{
    if (event == NULL || event->mode > SOLAR_OS_INPUT_POINTER_RELATIVE ||
        event->action > SOLAR_OS_INPUT_POINTER_RELEASE ||
        event->target[SOLAR_OS_INPUT_POINTER_TARGET_MAX - 1U] != '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        (event->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE &&
         (input_sources[source - 1U].capabilities &
          SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) == 0) ||
        (event->mode == SOLAR_OS_INPUT_POINTER_RELATIVE &&
         (input_sources[source - 1U].capabilities &
          SOLAR_OS_INPUT_CAP_POINTER_RELATIVE) == 0) ||
        input_pointer_queue == NULL) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        input_source_diagnostics_state_t *diagnostics =
            &input_diagnostics[source - 1U];
        solar_os_input_pointer_event_t raw = *event;
        raw.source = source;
        solar_os_input_pointer_event_t queued = raw;
        input_apply_calibration_locked(diagnostics, &queued);
        queued.source = source;
        if (!input_pointer_queue_push_locked(&queued)) {
            result = ESP_ERR_NO_MEM;
        } else {
            input_increment_counter(&diagnostics->pointer_events);
            diagnostics->has_pointer = true;
            diagnostics->last_pointer.event = queued;
            diagnostics->last_pointer.raw_x = raw.x;
            diagnostics->last_pointer.raw_y = raw.y;
            diagnostics->last_pointer.raw_delta_x = raw.delta_x;
            diagnostics->last_pointer.raw_delta_y = raw.delta_y;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_write_axis(solar_os_input_source_t source,
                                    const solar_os_input_axis_event_t *event)
{
    if (event == NULL || event->axis < SOLAR_OS_INPUT_AXIS_X ||
        event->axis >= SOLAR_OS_INPUT_AXIS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        (input_sources[source - 1U].capabilities &
         SOLAR_OS_INPUT_CAP_AXIS_EVENTS) == 0 ||
        input_axis_queue == NULL) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        solar_os_input_axis_event_t queued = *event;
        queued.source = source;
        if (!input_axis_queue_push_locked(&queued)) {
            result = ESP_ERR_NO_MEM;
        } else {
            input_source_diagnostics_state_t *diagnostics =
                &input_diagnostics[source - 1U];
            input_increment_counter(&diagnostics->axis_events);
            diagnostics->has_axis = true;
            diagnostics->last_axis = queued;
        }
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_pointer_calibration_get(
    solar_os_input_source_t source,
    bool *enabled,
    solar_os_input_pointer_calibration_t *calibration)
{
    if (enabled == NULL || calibration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        (input_sources[source - 1U].capabilities &
         SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) == 0) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        *enabled = input_diagnostics[source - 1U].calibration_enabled;
        *calibration = input_diagnostics[source - 1U].calibration;
    }
    portEXIT_CRITICAL(&input_lock);
    return result;
}

esp_err_t solar_os_input_pointer_calibration_set(
    solar_os_input_source_t source,
    const solar_os_input_pointer_calibration_t *calibration)
{
    if (!input_calibration_valid(calibration)) {
        return ESP_ERR_INVALID_ARG;
    }
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        (input_sources[source - 1U].capabilities &
         SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) == 0) {
        portEXIT_CRITICAL(&input_lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(name, input_sources[source - 1U].name, sizeof(name));
    portEXIT_CRITICAL(&input_lock);

    esp_err_t err = input_calibration_store(name, calibration);
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        strcmp(input_sources[source - 1U].name, name) != 0) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        input_diagnostics[source - 1U].calibration_enabled = true;
        input_diagnostics[source - 1U].calibration = *calibration;
    }
    portEXIT_CRITICAL(&input_lock);
    return err;
}

esp_err_t solar_os_input_pointer_calibration_reset(solar_os_input_source_t source)
{
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        (input_sources[source - 1U].capabilities &
         SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE) == 0) {
        portEXIT_CRITICAL(&input_lock);
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(name, input_sources[source - 1U].name, sizeof(name));
    portEXIT_CRITICAL(&input_lock);

    esp_err_t err = input_calibration_erase(name);
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&input_lock);
    if (!input_source_valid_locked(source) ||
        strcmp(input_sources[source - 1U].name, name) != 0) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        input_diagnostics[source - 1U].calibration_enabled = false;
        memset(&input_diagnostics[source - 1U].calibration,
               0,
               sizeof(input_diagnostics[source - 1U].calibration));
    }
    portEXIT_CRITICAL(&input_lock);
    return err;
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

size_t solar_os_input_read_pointer_events(solar_os_input_pointer_event_t *events,
                                          size_t event_count)
{
    if (events == NULL || event_count == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    size_t count = 0;
    while (input_pointer_queue != NULL && count < event_count &&
           input_pointer_queue_count > 0) {
        events[count++] = input_pointer_queue[input_pointer_queue_head];
        input_pointer_queue_head =
            (input_pointer_queue_head + 1U) % INPUT_POINTER_QUEUE_MAX;
        input_pointer_queue_count--;
    }
    if (input_pointer_queue_count == 0) {
        input_pointer_queue_head = 0;
    }
    portEXIT_CRITICAL(&input_lock);
    return count;
}

size_t solar_os_input_read_axis_events(solar_os_input_axis_event_t *events,
                                       size_t event_count)
{
    if (events == NULL || event_count == 0) {
        return 0;
    }

    portENTER_CRITICAL(&input_lock);
    size_t count = 0;
    while (input_axis_queue != NULL && count < event_count &&
           input_axis_queue_count > 0) {
        events[count++] = input_axis_queue[input_axis_queue_head];
        input_axis_queue_head =
            (input_axis_queue_head + 1U) % INPUT_AXIS_QUEUE_MAX;
        input_axis_queue_count--;
    }
    if (input_axis_queue_count == 0) {
        input_axis_queue_head = 0;
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
