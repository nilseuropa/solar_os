#include "gt911.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "solar_os_buses.h"

#define GT911_REG_PRODUCT_ID 0x8140U
#define GT911_REG_STATUS 0x814eU
#define GT911_REG_POINTS 0x814fU

static const char *TAG = "gt911";
static bool ready;
static char bus_name[SOLAR_OS_BUS_NAME_MAX];
static uint8_t device_address;

static esp_err_t transfer(uint16_t reg, uint8_t *data, size_t len)
{
    const uint8_t tx[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return solar_os_bus_i2c_transmit_receive(bus_name, device_address,
                                             tx, sizeof(tx), data, len);
}

static esp_err_t write_u8(uint16_t reg, uint8_t value)
{
    const uint8_t tx[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    return solar_os_bus_i2c_transmit(bus_name, device_address, tx, sizeof(tx));
}

esp_err_t gt911_init(const char *i2c_bus, uint8_t address, int irq_pin)
{
    if (!i2c_bus || !i2c_bus[0] ||
        (address != GT911_ADDRESS && address != GT911_ALTERNATE_ADDRESS) ||
        irq_pin < 0 || irq_pin >= 64) return ESP_ERR_INVALID_ARG;
    if (ready) return strcmp(bus_name, i2c_bus) == 0 &&
        device_address == address ? ESP_OK : ESP_ERR_INVALID_STATE;

    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "irq config failed");
    strlcpy(bus_name, i2c_bus, sizeof(bus_name));
    device_address = address;
    uint8_t product[4] = {0};
    esp_err_t err = transfer(GT911_REG_PRODUCT_ID, product, sizeof(product));
    if (err != ESP_OK && address == GT911_ADDRESS) {
        device_address = GT911_ALTERNATE_ADDRESS;
        err = transfer(GT911_REG_PRODUCT_ID, product, sizeof(product));
    }
    if (err != ESP_OK) {
        bus_name[0] = '\0';
        device_address = 0;
        return err;
    }
    ready = true;
    return ESP_OK;
}

esp_err_t gt911_read(gt911_sample_t *sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    if (!ready) return ESP_ERR_INVALID_STATE;
    memset(sample, 0, sizeof(*sample));
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(transfer(GT911_REG_STATUS, &status, 1), TAG, "status read failed");
    if (!(status & 0x80U)) return ESP_OK;
    const uint8_t count = status & 0x0fU;
    if (count) {
        uint8_t point[8];
        ESP_RETURN_ON_ERROR(transfer(GT911_REG_POINTS, point, sizeof(point)), TAG,
                            "point read failed");
        sample->touched = true;
        sample->id = point[0] & 0x0fU;
        sample->x = (uint16_t)point[1] | ((uint16_t)point[2] << 8);
        sample->y = (uint16_t)point[3] | ((uint16_t)point[4] << 8);
    }
    return write_u8(GT911_REG_STATUS, 0);
}

void gt911_deinit(void)
{
    ready = false;
    bus_name[0] = '\0';
    device_address = 0;
}
