#include "solar_os_gpio_keys_job.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "solar_os_gpio.h"
#include "solar_os_input.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_shell.h"

#define GPIO_KEYS_MAX SOLAR_OS_RESOURCE_BUNDLE_MAX
#define GPIO_KEYS_DEBOUNCE_MS 25U
#define GPIO_KEYS_POLL_MS 10U
#define GPIO_KEYS_LINE_MAX 96U

typedef struct {
    int pin;
    uint8_t key;
    char label[SOLAR_OS_RESOURCE_LABEL_MAX];
    bool last_raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
} gpio_key_mapping_t;

typedef struct {
    bool running;
    size_t mapping_count;
    gpio_key_mapping_t mappings[GPIO_KEYS_MAX];
    solar_os_input_source_t input_source;
    uint32_t presses;
    uint32_t dropped;
    uint32_t read_errors;
    char owner[SOLAR_OS_JOB_OWNER_MAX];
} gpio_keys_state_t;

static const char *TAG = "solar_os_gpio_keys";
static gpio_keys_state_t gpio_keys;

static char *trim(char *text)
{
    while (text != NULL && isspace((unsigned char)*text)) {
        text++;
    }
    if (text == NULL || *text == '\0') {
        return text;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static bool parse_pin(const char *text, int *pin)
{
    if (text == NULL || pin == NULL) {
        return false;
    }
    if (strncasecmp(text, "gpio", 4) == 0) {
        text += 4;
    } else if (strncasecmp(text, "io", 2) == 0) {
        text += 2;
    }
    if (*text == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > INT32_MAX) {
        return false;
    }
    *pin = (int)value;
    return true;
}

static void format_key_label(uint8_t key, char *label, size_t label_len)
{
    const char *name = solar_os_key_name(key);
    if (name != NULL) {
        snprintf(label, label_len, "key:%s", name);
    } else if (isprint(key)) {
        snprintf(label, label_len, "key:%c", (char)key);
    } else {
        snprintf(label, label_len, "key:0x%02x", (unsigned)key);
    }
}

static bool add_mapping(gpio_key_mapping_t *mappings,
                        size_t *count,
                        const char *pin_text,
                        const char *key_text)
{
    if (mappings == NULL || count == NULL || *count >= GPIO_KEYS_MAX) {
        return false;
    }

    int pin = -1;
    uint8_t key = 0;
    if (!parse_pin(pin_text, &pin) ||
        !solar_os_gpio_is_runtime_allowed(pin) ||
        !solar_os_key_parse(key_text, &key)) {
        return false;
    }
    for (size_t i = 0; i < *count; i++) {
        if (mappings[i].pin == pin) {
            return false;
        }
    }

    gpio_key_mapping_t *mapping = &mappings[(*count)++];
    memset(mapping, 0, sizeof(*mapping));
    mapping->pin = pin;
    mapping->key = key;
    format_key_label(key, mapping->label, sizeof(mapping->label));
    return true;
}

static bool parse_mapping_text(char *text,
                               gpio_key_mapping_t *mappings,
                               size_t *count)
{
    char *mapping = trim(text);
    if (mapping == NULL || mapping[0] == '\0') {
        return false;
    }

    char *key_text = strchr(mapping, ':');
    if (key_text != NULL) {
        *key_text++ = '\0';
    } else {
        key_text = mapping;
        while (*key_text != '\0' && !isspace((unsigned char)*key_text)) {
            key_text++;
        }
        if (*key_text == '\0') {
            return false;
        }
        *key_text++ = '\0';
    }

    const char *pin_text = trim(mapping);
    key_text = trim(key_text);
    if (pin_text == NULL || pin_text[0] == '\0' ||
        key_text == NULL || key_text[0] == '\0') {
        return false;
    }
    for (char *p = key_text; *p != '\0'; p++) {
        if (isspace((unsigned char)*p)) {
            return false;
        }
    }
    return add_mapping(mappings, count, pin_text, key_text);
}

static esp_err_t load_config(solar_os_context_t *ctx,
                             const char *path_arg,
                             gpio_key_mapping_t *mappings,
                             size_t *count,
                             char *resolved,
                             size_t resolved_len)
{
    esp_err_t err = solar_os_shell_resolve_path(ctx, path_arg, resolved, resolved_len);
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(resolved, "r");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[GPIO_KEYS_LINE_MAX];
    size_t line_number = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        char *comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }
        char *mapping = trim(line);
        if (mapping == NULL || mapping[0] == '\0') {
            continue;
        }
        if (!parse_mapping_text(mapping, mappings, count)) {
            SOLAR_OS_LOGW(TAG, "invalid mapping in %s:%u", resolved, (unsigned)line_number);
            err = ESP_ERR_INVALID_ARG;
            break;
        }
    }
    if (err == ESP_OK && ferror(file)) {
        err = ESP_FAIL;
    }
    fclose(file);
    if (err == ESP_OK && *count == 0) {
        err = ESP_ERR_INVALID_ARG;
    }
    return err;
}

static void gpio_keys_cleanup(void)
{
    if (gpio_keys.input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(gpio_keys.input_source);
    }
    for (size_t i = 0; i < gpio_keys.mapping_count; i++) {
        (void)solar_os_gpio_release_owned(gpio_keys.mappings[i].pin, gpio_keys.owner);
    }
    memset(&gpio_keys, 0, sizeof(gpio_keys));
}

static esp_err_t gpio_keys_start(solar_os_context_t *ctx, int argc, char **argv)
{
    gpio_key_mapping_t mappings[GPIO_KEYS_MAX] = {0};
    size_t mapping_count = 0;
    char config_path[SOLAR_OS_APP_ARG_LEN] = "";

    if (argc < 2 || argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(argv[1], "--config") == 0) {
        if (argc != 3) {
            return ESP_ERR_INVALID_ARG;
        }
        const esp_err_t config_err = load_config(ctx,
                                                 argv[2],
                                                 mappings,
                                                 &mapping_count,
                                                 config_path,
                                                 sizeof(config_path));
        if (config_err != ESP_OK) {
            return config_err;
        }
    } else {
        for (int i = 1; i < argc; i++) {
            char mapping_text[SOLAR_OS_APP_ARG_LEN];
            strlcpy(mapping_text, argv[i], sizeof(mapping_text));
            if (!parse_mapping_text(mapping_text, mappings, &mapping_count)) {
                SOLAR_OS_LOGW(TAG, "invalid mapping: %s", argv[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    memset(&gpio_keys, 0, sizeof(gpio_keys));
    gpio_keys.mapping_count = mapping_count;
    memcpy(gpio_keys.mappings, mappings, mapping_count * sizeof(mappings[0]));
    esp_err_t err = solar_os_jobs_owner_name("gpio-keys",
                                             gpio_keys.owner,
                                             sizeof(gpio_keys.owner));
    if (err != ESP_OK) {
        gpio_keys_cleanup();
        return err;
    }

    int pins[GPIO_KEYS_MAX];
    const char *labels[GPIO_KEYS_MAX];
    for (size_t i = 0; i < mapping_count; i++) {
        pins[i] = gpio_keys.mappings[i].pin;
        labels[i] = gpio_keys.mappings[i].label;
    }
    solar_os_resource_conflict_t conflict;
    err = solar_os_gpio_claim_pins(pins,
                                   labels,
                                   mapping_count,
                                   gpio_keys.owner,
                                   &conflict);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            SOLAR_OS_LOGW(TAG,
                          "GPIO%d is owned by %s",
                          pins[conflict.request_index],
                          conflict.existing.owner);
        }
        gpio_keys_cleanup();
        return err;
    }

    for (size_t i = 0; i < mapping_count; i++) {
        gpio_key_mapping_t *mapping = &gpio_keys.mappings[i];
        err = solar_os_gpio_configure_owned(mapping->pin,
                                            SOLAR_OS_GPIO_MODE_INPUT,
                                            SOLAR_OS_GPIO_PULL_UP,
                                            gpio_keys.owner);
        bool level = true;
        if (err == ESP_OK) {
            err = solar_os_gpio_read_owned(mapping->pin, gpio_keys.owner, &level);
        }
        if (err != ESP_OK) {
            gpio_keys_cleanup();
            return err;
        }
        const bool pressed = !level;
        mapping->last_raw_pressed = pressed;
        mapping->stable_pressed = pressed;
        mapping->raw_changed_ms = 0;
    }

    err = solar_os_input_key_source_open("gpio-keys",
                                         SOLAR_OS_INPUT_SOURCE_KEYBOARD,
                                         &gpio_keys.input_source);
    if (err != ESP_OK) {
        gpio_keys_cleanup();
        return err;
    }

    char detail[24];
    snprintf(detail, sizeof(detail), "%u pins", (unsigned)mapping_count);
    (void)solar_os_jobs_note_resource("gpio-keys",
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "gpio-keys",
                                      detail);
    if (config_path[0] != '\0') {
        (void)solar_os_jobs_note_resource("gpio-keys",
                                          SOLAR_OS_JOB_RESOURCE_FILE,
                                          config_path,
                                          "config");
    }
    gpio_keys.running = true;
    SOLAR_OS_LOGI(TAG, "%u press-only GPIO keys ready", (unsigned)mapping_count);
    return ESP_OK;
}

static void gpio_keys_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (gpio_keys.running) {
        SOLAR_OS_LOGI(TAG,
                      "stopped: presses=%u dropped=%u read_errors=%u",
                      (unsigned)gpio_keys.presses,
                      (unsigned)gpio_keys.dropped,
                      (unsigned)gpio_keys.read_errors);
    }
    gpio_keys_cleanup();
}

static bool gpio_keys_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (!gpio_keys.running || event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }

    const uint32_t now_ms = event->data.tick_ms;
    for (size_t i = 0; i < gpio_keys.mapping_count; i++) {
        gpio_key_mapping_t *mapping = &gpio_keys.mappings[i];
        bool level = true;
        if (solar_os_gpio_read_owned(mapping->pin, gpio_keys.owner, &level) != ESP_OK) {
            gpio_keys.read_errors++;
            continue;
        }
        const bool pressed = !level;
        if (pressed != mapping->last_raw_pressed) {
            mapping->last_raw_pressed = pressed;
            mapping->raw_changed_ms = now_ms;
            continue;
        }
        if (pressed == mapping->stable_pressed ||
            (uint32_t)(now_ms - mapping->raw_changed_ms) < GPIO_KEYS_DEBOUNCE_MS) {
            continue;
        }

        mapping->stable_pressed = pressed;
        if (solar_os_input_write_key(gpio_keys.input_source,
                                     (uint16_t)mapping->pin + 1U,
                                     SOLAR_OS_INPUT_USAGE_NONE,
                                     mapping->key,
                                     0,
                                     pressed ? SOLAR_OS_INPUT_KEY_PRESS :
                                         SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK) {
            if (!pressed) {
                continue;
            }
            gpio_keys.presses++;
        } else {
            gpio_keys.dropped++;
        }
    }
    return false;
}

const solar_os_job_t solar_os_gpio_keys_job = {
    .name = "gpio-keys",
    .summary = "map pull-up GPIO presses to keyboard input",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = gpio_keys_start,
    .stop = gpio_keys_stop,
    .event = gpio_keys_event,
    .tick_interval_ms = GPIO_KEYS_POLL_MS,
    .tick_deadline_ms = 2U,
};
