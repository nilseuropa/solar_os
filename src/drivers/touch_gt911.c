#include "touch_gt911.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "io_expander_ch422g.h"
#include "solar_os_board.h"
#include "solar_os_log.h"
#include "solar_os_touch.h"

#define GT911_ADDR_PRIMARY 0x5d
#define GT911_ADDR_SECONDARY 0x14
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS 0x814e
#define GT911_REG_POINT1 0x814f
#define GT911_STATUS_READY 0x80U

static const char *TAG = "touch_gt911";

static uint8_t gt911_addr;
static bool gt911_ready;
/* GT911 only raises the status "ready" flag when it has a fresh
 * frame; between frames the last state stays valid. */
static bool gt911_pressed;
static uint16_t gt911_x;
static uint16_t gt911_y;

static esp_err_t gt911_read_reg(uint16_t reg, uint8_t *data, size_t len)
{
    const uint8_t reg_be[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xffU)};
    return i2c_bus_transmit_receive(gt911_addr, reg_be, sizeof(reg_be), data, len);
}

static esp_err_t gt911_write_reg_byte(uint16_t reg, uint8_t value)
{
    const uint8_t frame[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xffU), value};
    return i2c_bus_transmit(gt911_addr, frame, sizeof(frame));
}

esp_err_t touch_gt911_init(void)
{
    if (gt911_ready) {
        return ESP_OK;
    }

    /* Reset (CH422G, high since expander init) released long ago by
     * the display bring-up; make sure of it and give the controller
     * time to finish booting on cold starts. */
    (void)io_expander_ch422g_set_pin(SOLAR_OS_BOARD_CH422G_PIN_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    const uint8_t addresses[2] = {GT911_ADDR_PRIMARY, GT911_ADDR_SECONDARY};
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < 2; i++) {
        ret = i2c_bus_probe(addresses[i]);
        if (ret == ESP_OK) {
            gt911_addr = addresses[i];
            break;
        }
    }
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "no touch controller at 0x%02x/0x%02x: %s",
                      GT911_ADDR_PRIMARY, GT911_ADDR_SECONDARY, esp_err_to_name(ret));
        return ret;
    }

    char product[5] = {0};
    (void)gt911_read_reg(GT911_REG_PRODUCT_ID, (uint8_t *)product, 4);

    gt911_ready = true;
    SOLAR_OS_LOGI(TAG, "ready at 0x%02x: product '%s'", gt911_addr, product);
    return ESP_OK;
}

bool touch_gt911_available(void)
{
    return gt911_ready;
}

esp_err_t touch_gt911_read(bool *pressed, uint16_t *x, uint16_t *y)
{
    if (pressed == NULL || x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pressed = false;
    *x = 0;
    *y = 0;
    if (!gt911_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0;
    esp_err_t ret = gt911_read_reg(GT911_REG_STATUS, &status, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    if ((status & GT911_STATUS_READY) != 0) {
        const uint8_t touches = status & 0x0fU;
        if (touches > 0) {
            /* Point 1: track id, x lo/hi, y lo/hi. */
            uint8_t point[5] = {0};
            ret = gt911_read_reg(GT911_REG_POINT1, point, sizeof(point));
            if (ret == ESP_OK) {
                const uint16_t raw_x = (uint16_t)(point[1] | (point[2] << 8));
                const uint16_t raw_y = (uint16_t)(point[3] | (point[4] << 8));
                if (!gt911_pressed) {
                    SOLAR_OS_LOGD(TAG, "touch down x=%u y=%u", raw_x, raw_y);
                }
                gt911_pressed = true;
                gt911_x = raw_x;
                gt911_y = raw_y;
            }
        } else {
            gt911_pressed = false;
        }
        (void)gt911_write_reg_byte(GT911_REG_STATUS, 0);
    }

    *pressed = gt911_pressed;
    *x = gt911_x;
    *y = gt911_y;
    return ESP_OK;
}

/* Board-agnostic facade (solar_os_touch.h). */
esp_err_t solar_os_touch_init(void)
{
    return touch_gt911_init();
}

bool solar_os_touch_available(void)
{
    return touch_gt911_available();
}

esp_err_t solar_os_touch_read(bool *pressed, uint16_t *x, uint16_t *y)
{
    return touch_gt911_read(pressed, x, y);
}
