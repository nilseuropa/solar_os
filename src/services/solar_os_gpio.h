#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "solar_os_pin_types.h"

#define SOLAR_OS_GPIO_OWNER_MAX 24

typedef enum {
    SOLAR_OS_GPIO_MODE_INPUT,
    SOLAR_OS_GPIO_MODE_OUTPUT,
} solar_os_gpio_mode_t;

typedef enum {
    SOLAR_OS_GPIO_PULL_NONE,
    SOLAR_OS_GPIO_PULL_UP,
    SOLAR_OS_GPIO_PULL_DOWN,
} solar_os_gpio_pull_t;

typedef struct {
    int pin;
    bool expansion;
    bool runtime_allowed;
    bool available;
    bool claimed;
    char owner[SOLAR_OS_GPIO_OWNER_MAX];
    solar_os_pin_policy_t policy;
    const char *role;
    bool configured;
    solar_os_gpio_mode_t mode;
    solar_os_gpio_pull_t pull;
    bool level;
    bool level_valid;
} solar_os_gpio_pin_info_t;

esp_err_t solar_os_gpio_init(void);
size_t solar_os_gpio_pin_count(void);
bool solar_os_gpio_get_pin_info(size_t index, solar_os_gpio_pin_info_t *info);
bool solar_os_gpio_get_pin_info_by_pin(int pin, solar_os_gpio_pin_info_t *info);
bool solar_os_gpio_is_runtime_allowed(int pin);
esp_err_t solar_os_gpio_configure(int pin, solar_os_gpio_mode_t mode, solar_os_gpio_pull_t pull);
esp_err_t solar_os_gpio_read(int pin, bool *level);
esp_err_t solar_os_gpio_write(int pin, bool level);
esp_err_t solar_os_gpio_release(int pin);
const char *solar_os_gpio_mode_name(solar_os_gpio_mode_t mode);
const char *solar_os_gpio_pull_name(solar_os_gpio_pull_t pull);
bool solar_os_gpio_parse_mode(const char *text, solar_os_gpio_mode_t *mode);
bool solar_os_gpio_parse_pull(const char *text, solar_os_gpio_pull_t *pull);
