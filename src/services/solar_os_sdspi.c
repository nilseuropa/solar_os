#include "solar_os_sdspi.h"

#include <stdbool.h>
#include <string.h>

#include "sd_card.h"
#include "solar_os_board_caps.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
} solar_os_sdspi_device_t;

static solar_os_sdspi_device_t sdspi;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin)
{
    bool have_spi = false;
    bool have_cs = false;

    if (bindings == NULL || spi_bus == NULL || spi_bus_len == 0 || cs_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_bus[0] = '\0';
    *cs_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) {
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(spi_bus, binding->target, spi_bus_len);
            have_spi = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS) {
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            *cs_pin = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(spi_bus, binding->target, spi_bus_len);
                have_spi = true;
            }
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_spi || !have_cs ||
        !solar_os_expansion_spi_cs_allowed(spi_bus, *cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t solar_os_sdspi_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin = -1;
    solar_os_expansion_spi_bus_t bus;

    if (name == NULL || name[0] == '\0' || sdspi.active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (solar_os_board_has(SOLAR_OS_BOARD_CAP_SD)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = parse_bindings(bindings,
                                   binding_count,
                                   spi_bus,
                                   sizeof(spi_bus),
                                   &cs_pin);
    if (ret != ESP_OK || !solar_os_expansion_find_spi_bus(spi_bus, &bus, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = sd_card_configure_sdspi(bus.host, cs_pin);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = sd_card_init();
    if (ret != ESP_OK) {
        (void)sd_card_unmount();
        (void)sd_card_clear_sdspi_config();
        return ret;
    }

    memset(&sdspi, 0, sizeof(sdspi));
    sdspi.active = true;
    sdspi.cs_pin = cs_pin;
    strlcpy(sdspi.name, name, sizeof(sdspi.name));
    strlcpy(sdspi.spi_bus, spi_bus, sizeof(sdspi.spi_bus));
    return ESP_OK;
}

esp_err_t solar_os_sdspi_detach(const char *name)
{
    if (!sdspi.active || name == NULL || strcmp(sdspi.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (sd_card_has_mounts()) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)sd_card_unmount();
    const esp_err_t ret = sd_card_clear_sdspi_config();
    if (ret == ESP_OK) {
        memset(&sdspi, 0, sizeof(sdspi));
    }
    return ret;
}
