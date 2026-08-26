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
#include "solar_os_expansion.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_shell.h"

#define GPIO_KEYS_MAX SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX
#define GPIO_KEYS_LINE_MAX 96U

typedef struct {
    int pin;
    uint8_t key;
    char label[SOLAR_OS_RESOURCE_LABEL_MAX];
} gpio_key_mapping_t;

typedef struct {
    bool running;
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

#define GPIO_KEYS_JOB_DEVICE "gpio-keys-job"

static void gpio_keys_cleanup(void)
{
    if (gpio_keys.running) {
        (void)solar_os_expansion_detach(GPIO_KEYS_JOB_DEVICE);
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

    solar_os_expansion_binding_t bindings[GPIO_KEYS_MAX] = {0};
    for (size_t i = 0; i < mapping_count; i++) {
        bindings[i].kind = SOLAR_OS_EXPANSION_BINDING_GPIO;
        bindings[i].value = mappings[i].pin;
        const char *key_name = solar_os_key_name(mappings[i].key);
        if (key_name != NULL) {
            strlcpy(bindings[i].role, key_name, sizeof(bindings[i].role));
        } else if (isprint(mappings[i].key)) {
            bindings[i].role[0] = (char)mappings[i].key;
            bindings[i].role[1] = '\0';
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    const esp_err_t err = solar_os_expansion_attach("gpio-keys",
                                                    GPIO_KEYS_JOB_DEVICE,
                                                    bindings,
                                                    mapping_count);
    gpio_keys.running = err == ESP_OK;
    return err;
}

static void gpio_keys_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    gpio_keys_cleanup();
}

const solar_os_job_t solar_os_gpio_keys_job = {
    .name = "gpio-keys",
    .summary = "attach pull-up GPIO keyboard buttons",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = gpio_keys_start,
    .stop = gpio_keys_stop,
};
