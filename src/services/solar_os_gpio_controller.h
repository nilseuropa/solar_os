#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_GPIO_CONTROLLER_NAME_MAX 16

typedef enum {
    SOLAR_OS_GPIO_LINE_MODE_INPUT,
    SOLAR_OS_GPIO_LINE_MODE_OUTPUT,
} solar_os_gpio_line_mode_t;

typedef enum {
    SOLAR_OS_GPIO_LINE_PULL_NONE,
    SOLAR_OS_GPIO_LINE_PULL_UP,
    SOLAR_OS_GPIO_LINE_PULL_DOWN,
} solar_os_gpio_line_pull_t;

typedef struct {
    char controller[SOLAR_OS_GPIO_CONTROLLER_NAME_MAX];
    uint8_t line;
} solar_os_gpio_line_ref_t;

typedef struct {
    esp_err_t (*configure)(void *ctx,
                           uint8_t line,
                           solar_os_gpio_line_mode_t mode,
                           solar_os_gpio_line_pull_t pull);
    esp_err_t (*read)(void *ctx, uint8_t line, bool *level);
    esp_err_t (*write)(void *ctx, uint8_t line, bool level);
} solar_os_gpio_controller_ops_t;

typedef struct {
    const char *name;
    uint8_t line_count;
    const solar_os_gpio_controller_ops_t *ops;
    void *ctx;
} solar_os_gpio_controller_registration_t;

typedef struct {
    uint8_t id;
    uint8_t line_count;
    char name[SOLAR_OS_GPIO_CONTROLLER_NAME_MAX];
} solar_os_gpio_controller_info_t;

esp_err_t solar_os_gpio_controller_register(
    const solar_os_gpio_controller_registration_t *registration);
esp_err_t solar_os_gpio_controller_unregister(const char *name);
size_t solar_os_gpio_controller_count(void);
bool solar_os_gpio_controller_get(size_t index, solar_os_gpio_controller_info_t *info);
bool solar_os_gpio_controller_find(const char *name, solar_os_gpio_controller_info_t *info);

bool solar_os_gpio_line_parse(const char *text, solar_os_gpio_line_ref_t *line);
esp_err_t solar_os_gpio_line_configure(const solar_os_gpio_line_ref_t *line,
                                       solar_os_gpio_line_mode_t mode,
                                       solar_os_gpio_line_pull_t pull);
esp_err_t solar_os_gpio_line_read(const solar_os_gpio_line_ref_t *line, bool *level);
esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *line, bool level);
