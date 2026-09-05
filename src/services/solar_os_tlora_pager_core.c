#include "solar_os_tlora_pager_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "solar_os_buses.h"

/*
 * Board bring-up for the LilyGO T-LoRa-Pager: an XL9555
 * (PCA9535/TCA9535-compatible) I2C GPIO expander that gates power and reset
 * to the keyboard, LoRa radio, GNSS, NFC, haptic driver and SD card. Nothing
 * behind those rails will probe successfully until this device attaches, so
 * it is marked `early` and must be listed first among this board's fixed
 * devices.
 *
 * The bit map below is taken from LilyGO's own reference firmware
 * (arduino-esp32 variants/lilygo_tlora_pager/pins_arduino.h), not reverse
 * engineered.
 *
 * The display backlight (an AW9364 single-wire pulse dimmer on GPIO42) is
 * deliberately NOT handled here: driving its enable pin HIGH from LOW is
 * exactly equivalent to a plain digital "on" at full brightness, so it is
 * wired instead through the st7796 display driver's ordinary non-PWM
 * backlight binding (`bl=42, active=1, pwm=0` in the board manifest). That
 * gives on/off backlight control via the existing, already-tested display
 * brightness path for free, without a second driver contending for the
 * same GPIO. True 16-step dimming is not implemented in this port.
 */

#define XL9555_REG_OUTPUT_PORT0 0x02U
#define XL9555_REG_OUTPUT_PORT1 0x03U
#define XL9555_REG_CONFIG_PORT0 0x06U
#define XL9555_REG_CONFIG_PORT1 0x07U

/* Port0 bit0..7: DRV_EN, AMP_EN, KB_RST, LORA_EN, GPS_EN, NFC_EN, (NC), GPS_RST */
#define XL9555_PORT0_OUTPUT_VALUE 0xBFU
#define XL9555_PORT0_CONFIG_VALUE 0x40U /* bit6 (NC) left as input; the rest are outputs */

/* Port1 bit0..7 (global bit8..15): KB_EN, GPIO_EN, SD_DET, SD_PULLEN, SD_EN, (NC x3) */
#define XL9555_PORT1_OUTPUT_VALUE 0x13U
#define XL9555_PORT1_CONFIG_VALUE 0xECU /* SD_DET/SD_PULLEN stay inputs; bits 5-7 unused stay inputs */

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
} solar_os_tlora_pager_core_device_t;

static const char *TAG = "tlora-pager-core";
static solar_os_tlora_pager_core_device_t core_device;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0U;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address || binding->value != SOLAR_OS_TLORA_PAGER_CORE_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t xl9555_power_sequence(const char *i2c_bus, uint8_t address)
{
    const uint8_t port0_out = XL9555_PORT0_OUTPUT_VALUE;
    const uint8_t port1_out = XL9555_PORT1_OUTPUT_VALUE;
    const uint8_t port0_cfg = XL9555_PORT0_CONFIG_VALUE;
    const uint8_t port1_cfg = XL9555_PORT1_CONFIG_VALUE;

    /* Pre-load the output latches before switching the pins to output mode,
     * so nothing glitches low the instant it becomes a driven output. */
    ESP_RETURN_ON_ERROR(
        solar_os_bus_i2c_write_reg(i2c_bus, address, XL9555_REG_OUTPUT_PORT0, &port0_out, 1),
        TAG, "XL9555 output0 write failed");
    ESP_RETURN_ON_ERROR(
        solar_os_bus_i2c_write_reg(i2c_bus, address, XL9555_REG_OUTPUT_PORT1, &port1_out, 1),
        TAG, "XL9555 output1 write failed");
    ESP_RETURN_ON_ERROR(
        solar_os_bus_i2c_write_reg(i2c_bus, address, XL9555_REG_CONFIG_PORT0, &port0_cfg, 1),
        TAG, "XL9555 config0 write failed");
    ESP_RETURN_ON_ERROR(
        solar_os_bus_i2c_write_reg(i2c_bus, address, XL9555_REG_CONFIG_PORT1, &port1_cfg, 1),
        TAG, "XL9555 config1 write failed");
    return ESP_OK;
}

static void clear_device(solar_os_tlora_pager_core_device_t *device)
{
    if (device == NULL) {
        return;
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_tlora_pager_core_attach(const char *name,
                                           const solar_os_expansion_binding_t *bindings,
                                           size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (core_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, i2c_bus, sizeof(i2c_bus), &address),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG,
                        "XL9555 not found");
    ESP_RETURN_ON_ERROR(xl9555_power_sequence(i2c_bus, address),
                        TAG,
                        "XL9555 power sequence failed");

    clear_device(&core_device);
    core_device.active = true;
    core_device.address = address;
    strlcpy(core_device.name, name, sizeof(core_device.name));
    strlcpy(core_device.i2c_bus, i2c_bus, sizeof(core_device.i2c_bus));

    ESP_LOGI(TAG,
             "%s attached on %s address 0x%02x: keyboard, LoRa, GNSS, NFC, "
             "haptic and SD power rails released",
             name,
             i2c_bus,
             address);
    return ESP_OK;
}

esp_err_t solar_os_tlora_pager_core_detach(const char *name)
{
    if (!core_device.active || name == NULL || strcmp(core_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Board-origin device: leave power/reset rails as-is on detach so we
     * don't yank power out from under peripherals that are still attached
     * and depending on this device having run once at boot. */
    clear_device(&core_device);
    return ESP_OK;
}
