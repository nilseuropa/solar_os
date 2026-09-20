#include "solar_os_shell_expansion_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static esp_err_t command_append(char *command,
                                size_t command_len,
                                const char *format,
                                ...)
{
    if (command == NULL || command_len == 0U || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t used = strlen(command);
    if (used >= command_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(&command[used],
                                  command_len - used,
                                  format,
                                  args);
    va_end(args);
    return written < 0 || (size_t)written >= command_len - used ?
        ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t command_append_token(char *command,
                                      size_t command_len,
                                      const char *token)
{
    if (command == NULL || command_len == 0U ||
        token == NULL || token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    bool quote = false;
    size_t escaped_len = 0U;
    for (const char *cursor = token; *cursor != '\0'; cursor++) {
        if (iscntrl((unsigned char)*cursor)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (isspace((unsigned char)*cursor) || *cursor == '"' ||
            *cursor == '\\' || *cursor == '\'' || *cursor == '|' ||
            *cursor == '&' || *cursor == ';' || *cursor == '<' ||
            *cursor == '>') {
            quote = true;
        }
        escaped_len += (*cursor == '"' || *cursor == '\\') ? 2U : 1U;
    }

    esp_err_t ret = command_append(command,
                                   command_len,
                                   command[0] == '\0' ? "" : " ");
    if (ret != ESP_OK) {
        return ret;
    }
    if (!quote) {
        return command_append(command, command_len, "%s", token);
    }
    if (strlen(command) + escaped_len + 3U > command_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    ret = command_append(command, command_len, "\"");
    for (const char *cursor = token; ret == ESP_OK && *cursor != '\0'; cursor++) {
        ret = command_append(command,
                             command_len,
                             *cursor == '"' || *cursor == '\\' ? "\\%c" : "%c",
                             *cursor);
    }
    return ret == ESP_OK ? command_append(command, command_len, "\"") : ret;
}

static bool binding_matches_spec(
    const solar_os_expansion_binding_t *binding,
    const solar_os_expansion_binding_spec_t *spec)
{
    if (binding == NULL || spec == NULL || binding->kind != spec->kind) {
        return false;
    }
    if (spec->role != NULL) {
        return strcmp(binding->role, spec->role) == 0;
    }
    if (spec->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS) {
        return binding->role[0] == '\0';
    }
    switch (spec->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return strcmp(binding->role, spec->key) == 0;
    default:
        return true;
    }
}

static const char *binding_spec_key(
    const solar_os_expansion_driver_t *driver,
    const solar_os_expansion_binding_t *binding)
{
    if (driver == NULL || binding == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < driver->binding_spec_count; i++) {
        if (binding_matches_spec(binding, &driver->binding_specs[i])) {
            return driver->binding_specs[i].key;
        }
    }
    return NULL;
}

static bool manual_gpio_role(const char *role)
{
    static const char * const roles[] = {
        "gpio", "bck", "din", "rck", "mclk", "ws", "dout", "pa",
        "pos", "neg", "amp", "irq", "reset", "data", "dc", "busy",
    };
    for (size_t i = 0U; i < sizeof(roles) / sizeof(roles[0]); i++) {
        if (strcmp(role, roles[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t binding_key(const solar_os_expansion_driver_t *driver,
                             const solar_os_expansion_binding_t *binding,
                             char *key,
                             size_t key_len)
{
    const char *spec_key = binding_spec_key(driver, binding);
    if (spec_key != NULL) {
        return strlcpy(key, spec_key, key_len) < key_len ?
            ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    const char *fixed = NULL;
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS: fixed = "i2c"; break;
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS: fixed = "spi"; break;
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT: fixed = "uart"; break;
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS: fixed = "ps2"; break;
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT: fixed = "i2s"; break;
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        fixed = binding->role[0] != '\0' ? binding->role : "addr";
        break;
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS: fixed = "cs"; break;
    case SOLAR_OS_EXPANSION_BINDING_ADC: fixed = "adc"; break;
    case SOLAR_OS_EXPANSION_BINDING_PWM: fixed = "pwm"; break;
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        fixed = binding->role;
        break;
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        if (manual_gpio_role(binding->role)) {
            fixed = binding->role;
        } else if (binding->role[0] == '\0') {
            fixed = "gpio";
        } else {
            const int written = snprintf(key,
                                         key_len,
                                         "key:%s",
                                         binding->role);
            return written < 0 || (size_t)written >= key_len ?
                ESP_ERR_INVALID_SIZE : ESP_OK;
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fixed == NULL || fixed[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return strlcpy(key, fixed, key_len) < key_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t binding_token(const solar_os_expansion_driver_t *driver,
                               const solar_os_expansion_binding_t *binding,
                               char *token,
                               size_t token_len)
{
    char key[SOLAR_OS_EXPANSION_ROLE_MAX + 5U];
    esp_err_t ret = binding_key(driver, binding, key, sizeof(key));
    if (ret != ESP_OK) {
        return ret;
    }

    int written = -1;
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        written = snprintf(token, token_len, "%s=%s", key, binding->target);
        break;
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
        written = binding->target[0] == '\0' ?
            snprintf(token, token_len, "%s=gpio%d", key, binding->value) :
            snprintf(token,
                     token_len,
                     "%s=%s:%d",
                     key,
                     binding->target,
                     binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
        written = snprintf(token, token_len, "%s=i2s%d", key, binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        written = snprintf(token, token_len, "%s=0x%02x", key, binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        written = snprintf(token, token_len, "%s=gpio%d", key, binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        written = snprintf(token, token_len, "%s=%d", key, binding->value);
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    return written < 0 || (size_t)written >= token_len ?
        ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t solar_os_shell_expansion_binding_manifest_field(
    const solar_os_expansion_driver_t *driver,
    const solar_os_expansion_binding_t *binding,
    char *key,
    size_t key_len,
    char *value,
    size_t value_len,
    bool *string_value)
{
    if (driver == NULL || binding == NULL || key == NULL || key_len == 0U ||
        value == NULL || value_len == 0U || string_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = binding_key(driver, binding, key, key_len);
    if (ret != ESP_OK) {
        return ret;
    }

    int written = -1;
    *string_value = false;
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        *string_value = true;
        written = snprintf(value, value_len, "%s", binding->target);
        break;
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
        if (binding->target[0] == '\0') {
            written = snprintf(value, value_len, "%d", binding->value);
        } else {
            *string_value = true;
            written = snprintf(value,
                               value_len,
                               "%s:%d",
                               binding->target,
                               binding->value);
        }
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        written = snprintf(value, value_len, "%d", binding->value);
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    return written < 0 || (size_t)written >= value_len ?
        ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t solar_os_shell_expansion_attach_command(
    const solar_os_expansion_driver_t *driver,
    const solar_os_expansion_device_t *device,
    char *command,
    size_t command_len)
{
    if (driver == NULL || device == NULL || command == NULL ||
        command_len == 0U || strcmp(driver->name, device->driver) != 0 ||
        device->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    command[0] = '\0';
    const char * const prefix[] = {
        "expansion", "attach", device->driver, device->name,
    };
    for (size_t i = 0U; i < sizeof(prefix) / sizeof(prefix[0]); i++) {
        const esp_err_t ret = command_append_token(command,
                                                   command_len,
                                                   prefix[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    for (size_t i = 0U; i < device->binding_count; i++) {
        char token[SOLAR_OS_EXPANSION_ROLE_MAX +
                   SOLAR_OS_EXPANSION_TARGET_MAX + 24U];
        esp_err_t ret = binding_token(driver,
                                      &device->bindings[i],
                                      token,
                                      sizeof(token));
        if (ret == ESP_OK) {
            ret = command_append_token(command, command_len, token);
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}
