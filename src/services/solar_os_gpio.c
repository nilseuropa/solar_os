#include "solar_os_gpio.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_pins.h"
#include "solar_os_resources.h"
#include "solar_os_stream.h"

#if SOLAR_OS_BOARD_HAS_GPIO
#include "driver/gpio.h"
#include "gpio_port.h"
#include "solar_os_board.h"
#endif

static esp_err_t gpio_stream_read(void *user,
                                  solar_os_stream_handle_t *handle,
                                  void *data,
                                  size_t len,
                                  uint32_t timeout_ms,
                                  size_t *read_len)
{
    (void)handle;
    (void)timeout_ms;
    if (data == NULL || len < 1U || read_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool level = false;
    const esp_err_t err = solar_os_gpio_read((int)(intptr_t)user, &level);
    if (err == ESP_OK) {
        ((uint8_t *)data)[0] = level ? 1U : 0U;
        *read_len = 1U;
    }
    return err;
}

static esp_err_t gpio_register_stream(int pin)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_EVENT,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_SHARED,
        },
        .read = gpio_stream_read,
        .user = (void *)(intptr_t)pin,
    };
    snprintf(driver.info.id, sizeof(driver.info.id), "gpio%d", pin);
    strlcpy(driver.info.provider, "gpio", sizeof(driver.info.provider));
    strlcpy(driver.info.device, driver.info.id, sizeof(driver.info.device));
    strlcpy(driver.info.format, "bool", sizeof(driver.info.format));
    strlcpy(driver.info.summary, "expansion GPIO state", sizeof(driver.info.summary));
    return solar_os_stream_register(&driver);
}

#if SOLAR_OS_BOARD_HAS_GPIO
typedef struct {
    int pin;
    solar_os_pin_policy_t policy;
    const char *role;
    bool configured;
    solar_os_gpio_mode_t mode;
    solar_os_gpio_pull_t pull;
    bool output_level;
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

static void gpio_resource_owner(int pin, char *owner, size_t owner_size)
{
    snprintf(owner, owner_size, "gpio:%d", pin);
}

static bool gpio_claim_is_ours(int pin, solar_os_resource_claim_t *claim)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_resource_claim_t existing;
    if (!solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &existing)) {
        return false;
    }
    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    gpio_resource_owner(pin, owner, sizeof(owner));
    if (claim != NULL) {
        *claim = existing;
    }
    return strcmp(existing.owner, owner) == 0;
#else
    (void)pin;
    (void)claim;
    return false;
#endif
}

static esp_err_t gpio_claim_direct(int pin, bool *claimed_here)
{
    if (claimed_here != NULL) {
        *claimed_here = false;
    }
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_resource_claim_t existing;
    if (solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &existing)) {
        return gpio_claim_is_ours(pin, NULL) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    gpio_resource_owner(pin, owner, sizeof(owner));
    const esp_err_t ret = solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN,
                                                  pin,
                                                  -1,
                                                  owner,
                                                  "direct-gpio");
    if (ret == ESP_OK && claimed_here != NULL) {
        *claimed_here = true;
    }
    return ret;
#else
    (void)pin;
    return ESP_OK;
#endif
}

static void gpio_release_claim(int pin)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    gpio_resource_owner(pin, owner, sizeof(owner));
    (void)solar_os_resource_release(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, owner);
#else
    (void)pin;
#endif
}
#endif

esp_err_t solar_os_gpio_init(void)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    return ESP_ERR_NOT_SUPPORTED;
#else
    size_t registered = 0U;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t info;
        if (!solar_os_gpio_get_pin_info(i, &info) || !info.runtime_allowed) {
            continue;
        }
        const esp_err_t err = gpio_register_stream(info.pin);
        if (err != ESP_OK) {
            for (size_t j = 0; j < i && registered > 0U; j++) {
                solar_os_gpio_pin_info_t prior;
                if (solar_os_gpio_get_pin_info(j, &prior) &&
                    prior.runtime_allowed) {
                    char id[SOLAR_OS_STREAM_ID_MAX];
                    snprintf(id, sizeof(id), "gpio%d", prior.pin);
                    (void)solar_os_stream_unregister(id);
                    registered--;
                }
            }
            return err;
        }
        registered++;
    }
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
    solar_os_resource_claim_t claim = {0};
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const bool claimed = solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN,
                                                       slot->pin,
                                                       -1,
                                                       &claim);
#else
    const bool claimed = false;
#endif
    const bool ours = claimed && gpio_claim_is_ours(slot->pin, NULL);
    bool level = false;
    esp_err_t level_err = ESP_ERR_INVALID_STATE;
    if (slot->configured && (!claimed || ours)) {
        if (slot->mode == SOLAR_OS_GPIO_MODE_OUTPUT) {
            /* Output-only GPIOs do not enable the input path on ESP-IDF. */
            level = slot->output_level;
            level_err = ESP_OK;
        } else {
            level_err = gpio_port_read((gpio_num_t)slot->pin, &level);
        }
    }

    *info = (solar_os_gpio_pin_info_t) {
        .pin = slot->pin,
        .expansion = solar_os_pin_is_expansion(slot->pin),
        .runtime_allowed = solar_os_pin_is_direct_gpio(slot->pin),
        .available = solar_os_pin_is_direct_gpio(slot->pin) && (!claimed || ours),
        .claimed = claimed,
        .policy = slot->policy,
        .role = slot->role,
        .configured = slot->configured,
        .mode = slot->mode,
        .pull = slot->pull,
        .level = level,
        .level_valid = level_err == ESP_OK,
    };
    if (claimed) {
        strlcpy(info->owner, claim.owner, sizeof(info->owner));
    }
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
    return find_const_slot(pin) != NULL && solar_os_pin_is_direct_gpio(pin);
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

    bool claimed_here = false;
    esp_err_t err = gpio_claim_direct(pin, &claimed_here);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_port_configure((gpio_num_t)pin, to_port_mode(mode), to_port_pull(pull));
    if (err == ESP_OK) {
        slot->configured = true;
        slot->mode = mode;
        slot->pull = pull;
    }
    if (err != ESP_OK && claimed_here) {
        gpio_release_claim(pin);
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
    } else {
        const esp_err_t claim_err = gpio_claim_direct(pin, NULL);
        if (claim_err != ESP_OK) return claim_err;
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

    if (!slot->configured || slot->mode != SOLAR_OS_GPIO_MODE_OUTPUT) {
        const esp_err_t config_err =
            solar_os_gpio_configure(pin, SOLAR_OS_GPIO_MODE_OUTPUT, SOLAR_OS_GPIO_PULL_NONE);
        if (config_err != ESP_OK) {
            return config_err;
        }
    } else {
        const esp_err_t claim_err = gpio_claim_direct(pin, NULL);
        if (claim_err != ESP_OK) return claim_err;
    }

    const esp_err_t err = gpio_port_write((gpio_num_t)pin, level);
    if (err == ESP_OK) {
        slot->output_level = level;
    }
    return err;
#endif
}

esp_err_t solar_os_gpio_release(int pin)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    if (slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_resource_claim_t claim;
    if (solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim) &&
        !gpio_claim_is_ours(pin, NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    const esp_err_t ret = gpio_reset_pin((gpio_num_t)pin);
    if (ret == ESP_OK) {
        slot->configured = false;
        slot->mode = SOLAR_OS_GPIO_MODE_INPUT;
        slot->pull = SOLAR_OS_GPIO_PULL_NONE;
        gpio_release_claim(pin);
    }
    return ret;
#endif
}

esp_err_t solar_os_gpio_claim_pins(const int *pins,
                                   const char *const *labels,
                                   size_t count,
                                   const char *owner,
                                   solar_os_resource_conflict_t *conflict)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pins;
    (void)labels;
    (void)count;
    (void)owner;
    (void)conflict;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (pins == NULL || labels == NULL || count == 0 ||
        count > SOLAR_OS_RESOURCE_BUNDLE_MAX || owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_resource_request_t requests[SOLAR_OS_RESOURCE_BUNDLE_MAX];
    for (size_t i = 0; i < count; i++) {
        if (!solar_os_gpio_is_runtime_allowed(pins[i]) ||
            labels[i] == NULL || labels[i][0] == '\0') {
            return ESP_ERR_NOT_ALLOWED;
        }
        requests[i] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = pins[i],
            .secondary = -1,
            .label = labels[i],
        };
    }
    return solar_os_resource_claim_bundle(requests, count, owner, conflict);
#endif
}

static esp_err_t gpio_check_owned(int pin, const char *owner)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)owner;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (find_slot(pin) == NULL || owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    solar_os_resource_claim_t claim;
    if (!solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim)) {
        return ESP_ERR_NOT_FOUND;
    }
    return strcmp(claim.owner, owner) == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t solar_os_gpio_configure_owned(int pin,
                                        solar_os_gpio_mode_t mode,
                                        solar_os_gpio_pull_t pull,
                                        const char *owner)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)mode;
    (void)pull;
    (void)owner;
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    esp_err_t err = gpio_check_owned(pin, owner);
    if (err != ESP_OK) {
        return err;
    }
    if ((mode != SOLAR_OS_GPIO_MODE_INPUT && mode != SOLAR_OS_GPIO_MODE_OUTPUT) ||
        (pull != SOLAR_OS_GPIO_PULL_NONE &&
         pull != SOLAR_OS_GPIO_PULL_UP &&
         pull != SOLAR_OS_GPIO_PULL_DOWN)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = gpio_port_configure((gpio_num_t)pin, to_port_mode(mode), to_port_pull(pull));
    if (err == ESP_OK) {
        slot->configured = true;
        slot->mode = mode;
        slot->pull = pull;
    }
    return err;
#endif
}

esp_err_t solar_os_gpio_read_owned(int pin, const char *owner, bool *level)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)owner;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = gpio_check_owned(pin, owner);
    if (err != ESP_OK) {
        return err;
    }
    if (slot == NULL || !slot->configured || slot->mode != SOLAR_OS_GPIO_MODE_INPUT) {
        return ESP_ERR_INVALID_STATE;
    }
    return gpio_port_read((gpio_num_t)pin, level);
#endif
}

esp_err_t solar_os_gpio_write_owned(int pin, const char *owner, bool level)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)owner;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    esp_err_t error = gpio_check_owned(pin, owner);
    if (error != ESP_OK) return error;
    if (slot == NULL || !slot->configured || slot->mode != SOLAR_OS_GPIO_MODE_OUTPUT) return ESP_ERR_INVALID_STATE;
    error = gpio_port_write((gpio_num_t)pin, level);
    if (error == ESP_OK) slot->output_level = level;
    return error;
#endif
}

esp_err_t solar_os_gpio_release_owned(int pin, const char *owner)
{
#if !SOLAR_OS_BOARD_HAS_GPIO
    (void)pin;
    (void)owner;
    return ESP_ERR_NOT_SUPPORTED;
#else
    gpio_slot_t *slot = find_slot(pin);
    const esp_err_t owned_err = gpio_check_owned(pin, owner);
    if (owned_err != ESP_OK) {
        return owned_err;
    }

    const esp_err_t reset_err = gpio_reset_pin((gpio_num_t)pin);
    if (reset_err != ESP_OK) {
        return reset_err;
    }
    slot->configured = false;
    slot->mode = SOLAR_OS_GPIO_MODE_INPUT;
    slot->pull = SOLAR_OS_GPIO_PULL_NONE;
    return solar_os_resource_release(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, owner);
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
