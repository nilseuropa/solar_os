#include "solar_os_sdmmc.h"

#include <stdbool.h>
#include <string.h>

#include "sd_card.h"
#include "solar_os_board_caps.h"
#include "solar_os_storage.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
} solar_os_sdmmc_device_t;

static solar_os_sdmmc_device_t sdmmc;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                int pins[6], int *power, int *active)
{
    static const char *const roles[] = {"clk", "cmd", "d0", "d1", "d2", "d3"};
    bool present[6] = {false};
    bool have_power = false, have_active = false;
    if (bindings == NULL || pins == NULL || power == NULL || active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *power = -1;
    *active = 1;
    for (size_t i = 0; i < 6; i++) {
        pins[i] = -1;
    }

    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO && !have_power &&
            strcmp(bindings[i].role, "power") == 0) {
            *power = bindings[i].value;
            have_power = true;
            continue;
        }
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !have_active &&
            strcmp(bindings[i].role, "active") == 0 &&
            bindings[i].value >= 0 && bindings[i].value <= 1) {
            *active = bindings[i].value;
            have_active = true;
            continue;
        }
        if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_GPIO) {
            return ESP_ERR_INVALID_ARG;
        }
        size_t role = 0;
        while (role < 6 && strcmp(bindings[i].role, roles[role]) != 0) {
            role++;
        }
        if (role == 6 || present[role]) {
            return ESP_ERR_INVALID_ARG;
        }
        pins[role] = bindings[i].value;
        present[role] = true;
    }

    if ((!have_power && have_active) || !present[0] || !present[1] || !present[2] ||
        (present[3] != present[4]) || (present[3] != present[5])) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t solar_os_sdmmc_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    int pins[6];
    int power, active;
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (sdmmc.active) {
        return strcmp(sdmmc.name, name) == 0 ?
            solar_os_storage_mount() : ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = parse_bindings(bindings, binding_count, pins, &power, &active);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = sd_card_configure_sdmmc(pins[0], pins[1], pins[2], pins[3], pins[4], pins[5]);
    if (ret != ESP_OK) {
        return ret;
    }

    if (power >= 0) {
        ret = sd_card_configure_sdmmc_power(power, active);
        if (ret != ESP_OK) {
            (void)sd_card_clear_sdmmc_config();
            return ret;
        }
    }

    /* Built-in cards mount during normal storage initialization. */
    if (!solar_os_board_has(SOLAR_OS_BOARD_CAP_SD)) {
        ret = sd_card_init();
        if (ret != ESP_OK) {
            (void)sd_card_unmount();
            (void)sd_card_clear_sdmmc_config();
            return ret;
        }
    }

    memset(&sdmmc, 0, sizeof(sdmmc));
    sdmmc.active = true;
    strlcpy(sdmmc.name, name, sizeof(sdmmc.name));
    return ESP_OK;
}

esp_err_t solar_os_sdmmc_detach(const char *name)
{
    if (!sdmmc.active || name == NULL || strcmp(sdmmc.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (sd_card_has_mounts()) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)sd_card_unmount();
    const esp_err_t ret = sd_card_clear_sdmmc_config();
    if (ret == ESP_OK) {
        memset(&sdmmc, 0, sizeof(sdmmc));
    }
    return ret;
}
