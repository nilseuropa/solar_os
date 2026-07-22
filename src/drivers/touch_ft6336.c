#include "touch_ft6336.h"

#include <stdbool.h>
#include <stdint.h>

#include "i2c_bus.h"
#include "solar_os_log.h"
#include "solar_os_touch.h"

#define FT6336_ADDR 0x38
#define FT6336_REG_TD_STATUS 0x02
#define FT6336_REG_CHIP_ID 0xA3
#define FT6336_REG_FIRMWARE_ID 0xA6

static const char *TAG = "touch_ft6336";

static bool ft6336_ready;

esp_err_t touch_ft6336_init(void)
{
    if (ft6336_ready) {
        return ESP_OK;
    }

    /* The i2c_bus_* helpers take the bus lock themselves. */
    esp_err_t ret = i2c_bus_probe(FT6336_ADDR);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "no touch controller at 0x%02x: %s",
                      FT6336_ADDR, esp_err_to_name(ret));
        return ret;
    }

    uint8_t chip_id = 0;
    uint8_t firmware_id = 0;
    (void)i2c_bus_read_reg(FT6336_ADDR, FT6336_REG_CHIP_ID, &chip_id, 1);
    (void)i2c_bus_read_reg(FT6336_ADDR, FT6336_REG_FIRMWARE_ID, &firmware_id, 1);

    ft6336_ready = true;
    SOLAR_OS_LOGI(TAG, "ready: chip 0x%02x firmware 0x%02x", chip_id, firmware_id);
    return ESP_OK;
}

bool touch_ft6336_available(void)
{
    return ft6336_ready;
}

esp_err_t touch_ft6336_read(bool *pressed, uint16_t *x, uint16_t *y)
{
    if (pressed == NULL || x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pressed = false;
    *x = 0;
    *y = 0;
    if (!ft6336_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* TD_STATUS + point 1: XH, XL, YH, YL. */
    uint8_t buf[5] = {0};
    const esp_err_t ret = i2c_bus_read_reg(FT6336_ADDR, FT6336_REG_TD_STATUS,
                                           buf, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }

    const uint8_t touches = buf[0] & 0x0fU;
    if (touches == 0 || touches > 2) {
        return ESP_OK;
    }

    *pressed = true;
    *x = (uint16_t)(((buf[1] & 0x0fU) << 8) | buf[2]);
    *y = (uint16_t)(((buf[3] & 0x0fU) << 8) | buf[4]);
    return ESP_OK;
}

/* Board-agnostic facade (solar_os_touch.h). */
esp_err_t solar_os_touch_init(void)
{
    return touch_ft6336_init();
}

bool solar_os_touch_available(void)
{
    return touch_ft6336_available();
}

esp_err_t solar_os_touch_read(bool *pressed, uint16_t *x, uint16_t *y)
{
    return touch_ft6336_read(pressed, x, y);
}
