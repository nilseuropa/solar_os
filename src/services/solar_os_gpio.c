#include "solar_os_gpio.h"

#include <stdint.h>
#include <string.h>

#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_GPIO
#include "driver/gpio.h"
#include "gpio_port.h"
#include "solar_os_board.h"
#endif

#if SOLAR_OS_BOARD_HAS_GPIO
typedef struct {
    int pin;
    bool runtime_allowed;
    const char *role;
    bool configured;
    solar_os_gpio_mode_t mode;
    solar_os_gpio_pull_t pull;
} gpio_slot_t;

static gpio_slot_t gpio_slots[] = SOLAR_OS_BOARD_GPIO_SLOTS;

static gpio_slot_t *find_slot(int pin)
{
    for (size_t i = 0; i < sizeof(gpio_slots) / sizeof(gpio_slots[0]); i++) {
        if (gpio_slots[i].pin == pin) {
            return &gpio_slots[i];
        }
    }
    return NULL;
}

static const gpio_slot_t *find_const_slot(int pin)
{
    return find_slot(pin);
}

static bool board_mask_contains(uint64_t mask, int pin)
{
    return pin >= 0 && pin < 64 && (mask & (1ULL << (uint32_t)pin)) != 0;
}

static gpio_port_mode_t to_port_mode(solar_os_gpio_mode_t mode)
{
    return mode == SOLAR_OS_GPIO_MODE_OUTPUT ? GPIO_PORT_MODE_OUTPUT : GPIO_PORT_MODE_INPUT;
}

static gpio_port_pull_t to_port_pull(solar_os_gpio_pull_t pull)
{
    switch (pull) {
    case SOLAR_OS_GPIO_PULL_UP:
        return GPIO_PORT_PULL_UP;
    case SOLAR_OS_GPIO_PULL_DOWN:
        return GPIO_PORT_PULL_DOWN;
    case SOLAR_OS_GPIO_PULL_NONE:
    default:
        return GPIO_PORT_PULL_NONE;
    }
}
#endif

esp_err_t solar_os_gpio_init(void)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    return ESP_OK;
#endif
}

size_t solar_os_gpio_pin_count(void)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    return 0;
#else
    return sizeof(gpio_slots) / sizeof(gpio_slots[0]);
#endif
}

bool solar_os_gpio_get_pin_info(size_t index, solar_os_gpio_pin_info_t *info)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)index;
    (void)info;
    return false;
#else
    if (info == NULL || index >= solar_os_gpio_pin_count()) {
        return false;
    }

    const gpio_slot_t *slot = &gpio_slots[index];
    bool level = false;
    const esp_err_t level_err = gpio_port_read((gpio_num_t)slot->pin, &level);

    *info = (solar_os_gpio_pin_info_t) {
        .pin = slot->pin,
        .runtime_allowed = slot->runtime_allowed,
        .role = slot->role,
        .configured = slot->configured,
        .mode = slot->mode,
        .pull = slot->pull,
        .level = level,
        .level_valid = level_err == ESP_OK,
    };
    return true;
#endif
}

bool solar_os_gpio_get_pin_info_by_pin(int pin, solar_os_gpio_pin_info_t *info)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)info;
    return false;
#else
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        if (gpio_slots[i].pin == pin) {
            return solar_os_gpio_get_pin_info(i, info);
        }
    }
    return false;
#endif
}

bool solar_os_gpio_is_runtime_allowed(int pin)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    return false;
#else
    const gpio_slot_t *slot = find_const_slot(pin);
    return slot != NULL &&
        slot->runtime_allowed &&
        board_mask_contains(SOLAR_OS_BOARD_USER_GPIO_MASK, pin);
#endif
}

esp_err_t solar_os_gpio_configure(int pin, solar_os_gpio_mode_t mode, solar_os_gpio_pull_t pull)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)mode;
    (void)pull;
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    if (slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (mode != SOLAR_OS_GPIO_MODE_INPUT && mode != SOLAR_OS_GPIO_MODE_OUTPUT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pull != SOLAR_OS_GPIO_PULL_NONE &&
        pull != SOLAR_OS_GPIO_PULL_UP &&
        pull != SOLAR_OS_GPIO_PULL_DOWN) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = gpio_port_configure((gpio_num_t)pin, to_port_mode(mode), to_port_pull(pull));
    if (err == ESP_OK) {
        slot->configured = true;
        slot->mode = mode;
        slot->pull = pull;
    }
    return err;
#endif
}

esp_err_t solar_os_gpio_read(int pin, bool *level)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    if (level == NULL || slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (!slot->configured) {
        const esp_err_t config_err =
            solar_os_gpio_configure(pin, SOLAR_OS_GPIO_MODE_INPUT, SOLAR_OS_GPIO_PULL_NONE);
        if (config_err != ESP_OK) {
            return config_err;
        }
    }

    return gpio_port_read((gpio_num_t)pin, level);
#endif
}

esp_err_t solar_os_gpio_write(int pin, bool level)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    if (slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }

    const esp_err_t prewrite_err = gpio_port_write((gpio_num_t)pin, level);
    if (prewrite_err != ESP_OK) {
        return prewrite_err;
    }

    if (!slot->configured || slot->mode != SOLAR_OS_GPIO_MODE_OUTPUT) {
        const esp_err_t config_err =
            solar_os_gpio_configure(pin, SOLAR_OS_GPIO_MODE_OUTPUT, SOLAR_OS_GPIO_PULL_NONE);
        if (config_err != ESP_OK) {
            return config_err;
        }
    }

    return gpio_port_write((gpio_num_t)pin, level);
#endif
}

const char *solar_os_gpio_mode_name(solar_os_gpio_mode_t mode)
{
    switch (mode) {
    case SOLAR_OS_GPIO_MODE_INPUT:
        return "input";
    case SOLAR_OS_GPIO_MODE_OUTPUT:
        return "output";
    default:
        return "unknown";
    }
}

const char *solar_os_gpio_pull_name(solar_os_gpio_pull_t pull)
{
    switch (pull) {
    case SOLAR_OS_GPIO_PULL_NONE:
        return "none";
    case SOLAR_OS_GPIO_PULL_UP:
        return "up";
    case SOLAR_OS_GPIO_PULL_DOWN:
        return "down";
    default:
        return "unknown";
    }
}

bool solar_os_gpio_parse_mode(const char *text, solar_os_gpio_mode_t *mode)
{
    if (text == NULL || mode == NULL) {
        return false;
    }
    if (strcmp(text, "in") == 0 || strcmp(text, "input") == 0) {
        *mode = SOLAR_OS_GPIO_MODE_INPUT;
        return true;
    }
    if (strcmp(text, "out") == 0 || strcmp(text, "output") == 0) {
        *mode = SOLAR_OS_GPIO_MODE_OUTPUT;
        return true;
    }
    return false;
}

bool solar_os_gpio_parse_pull(const char *text, solar_os_gpio_pull_t *pull)
{
    if (text == NULL || pull == NULL) {
        return false;
    }
    if (strcmp(text, "none") == 0 || strcmp(text, "off") == 0) {
        *pull = SOLAR_OS_GPIO_PULL_NONE;
        return true;
    }
    if (strcmp(text, "up") == 0 || strcmp(text, "pullup") == 0) {
        *pull = SOLAR_OS_GPIO_PULL_UP;
        return true;
    }
    if (strcmp(text, "down") == 0 || strcmp(text, "pulldown") == 0) {
        *pull = SOLAR_OS_GPIO_PULL_DOWN;
        return true;
    }
    return false;
}
