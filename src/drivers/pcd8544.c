#include "pcd8544.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "u8x8.h"

#define PCD8544_SPI_CLOCK_HZ 4000000
#define PCD8544_TILE_HEIGHT 6U

static const char *TAG = "pcd8544";

static pcd8544_t *pcd8544_from_u8x8(u8x8_t *u8x8)
{
    if (u8x8 == NULL) {
        return NULL;
    }
    return (pcd8544_t *)((uint8_t *)u8x8 -
                         offsetof(pcd8544_t, u8g2) -
                         offsetof(u8g2_t, u8x8));
}

static void pcd8544_set_error(pcd8544_t *display, esp_err_t err)
{
    if (display != NULL && err != ESP_OK && display->last_error == ESP_OK) {
        display->last_error = err;
    }
}

static bool pcd8544_gpio_set_checked(pcd8544_t *display, int pin, uint32_t level)
{
    const esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
    pcd8544_set_error(display, err);
    return err == ESP_OK;
}

static uint8_t pcd8544_u8x8_byte_cb(u8x8_t *u8x8,
                                    uint8_t message,
                                    uint8_t arg_int,
                                    void *arg_ptr)
{
    pcd8544_t *display = pcd8544_from_u8x8(u8x8);
    if (display == NULL || display->last_error != ESP_OK) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_BYTE_INIT:
        return pcd8544_gpio_set_checked(display, display->cs_pin, 1) &&
            pcd8544_gpio_set_checked(display, display->dc_pin, 0);
    case U8X8_MSG_BYTE_SET_DC:
        return pcd8544_gpio_set_checked(display, display->dc_pin, arg_int != 0);
    case U8X8_MSG_BYTE_START_TRANSFER:
        return pcd8544_gpio_set_checked(display, display->cs_pin, 0);
    case U8X8_MSG_BYTE_END_TRANSFER:
        return pcd8544_gpio_set_checked(display, display->cs_pin, 1);
    case U8X8_MSG_BYTE_SEND: {
        if (arg_int == 0) {
            return 1;
        }
        if (display->spi == NULL || arg_ptr == NULL) {
            pcd8544_set_error(display, ESP_ERR_INVALID_STATE);
            return 0;
        }

        spi_transaction_t transaction = {
            .length = (size_t)arg_int * 8U,
            .tx_buffer = arg_ptr,
        };
        const esp_err_t err = spi_device_polling_transmit(display->spi, &transaction);
        pcd8544_set_error(display, err);
        return err == ESP_OK;
    }
    default:
        return 1;
    }
}

static uint8_t pcd8544_u8x8_gpio_delay_cb(u8x8_t *u8x8,
                                          uint8_t message,
                                          uint8_t arg_int,
                                          void *arg_ptr)
{
    (void)arg_ptr;
    pcd8544_t *display = pcd8544_from_u8x8(u8x8);
    if (display == NULL || display->last_error != ESP_OK) {
        return 0;
    }

    switch (message) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        return pcd8544_gpio_set_checked(display, display->cs_pin, 1) &&
            pcd8544_gpio_set_checked(display, display->dc_pin, 0) &&
            pcd8544_gpio_set_checked(display, display->reset_pin, 1);
    case U8X8_MSG_GPIO_RESET:
        return pcd8544_gpio_set_checked(display, display->reset_pin, arg_int != 0);
    case U8X8_MSG_DELAY_MILLI:
        if (arg_int <= 10) {
            esp_rom_delay_us((uint32_t)arg_int * 1000U);
        } else {
            vTaskDelay(pdMS_TO_TICKS(arg_int));
        }
        return 1;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us((uint32_t)arg_int * 10U);
        return 1;
    case U8X8_MSG_DELAY_100NANO:
    case U8X8_MSG_DELAY_NANO:
        return 1;
    default:
        return 1;
    }
}

static esp_err_t pcd8544_configure_pins(pcd8544_t *display)
{
    const uint64_t pin_mask = (1ULL << (uint32_t)display->cs_pin) |
        (1ULL << (uint32_t)display->dc_pin) |
        (1ULL << (uint32_t)display->reset_pin);
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "gpio config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->cs_pin, 1), TAG, "cs idle failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->dc_pin, 0), TAG, "dc idle failed");
    ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)display->reset_pin, 1), TAG, "reset idle failed");
    display->pins_configured = true;
    return ESP_OK;
}

esp_err_t pcd8544_init(pcd8544_t *display,
                       const char *spi_bus,
                       int cs_pin,
                       int dc_pin,
                       int reset_pin)
{
    if (display == NULL || spi_bus == NULL || spi_bus[0] == '\0' ||
        cs_pin < 0 || cs_pin >= 64 ||
        dc_pin < 0 || dc_pin >= 64 ||
        reset_pin < 0 || reset_pin >= 64 ||
        cs_pin == dc_pin ||
        cs_pin == reset_pin ||
        dc_pin == reset_pin) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(display, 0, sizeof(*display));
    display->cs_pin = cs_pin;
    display->dc_pin = dc_pin;
    display->reset_pin = reset_pin;
    display->last_error = ESP_OK;

    esp_err_t ret = pcd8544_configure_pins(display);
    if (ret != ESP_OK) {
        pcd8544_deinit(display);
        return ret;
    }

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = PCD8544_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ret = solar_os_bus_spi_add_device(spi_bus, &device_config, &display->spi);
    if (ret != ESP_OK) {
        pcd8544_deinit(display);
        return ret;
    }

    u8g2_SetupDisplay(&display->u8g2,
                      u8x8_d_pcd8544_84x48,
                      u8x8_cad_001,
                      pcd8544_u8x8_byte_cb,
                      pcd8544_u8x8_gpio_delay_cb);
    u8g2_SetupBuffer(&display->u8g2,
                     display->buffer,
                     PCD8544_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb,
                     U8G2_R0);
    u8g2_InitDisplay(&display->u8g2);
    u8g2_SetPowerSave(&display->u8g2, 0);
    if (display->last_error != ESP_OK) {
        ret = display->last_error;
        pcd8544_deinit(display);
        return ret;
    }

    return ESP_OK;
}

void pcd8544_deinit(pcd8544_t *display)
{
    if (display == NULL) {
        return;
    }

    if (display->spi != NULL) {
        display->last_error = ESP_OK;
        u8g2_SetPowerSave(&display->u8g2, 1);
        (void)spi_bus_remove_device(display->spi);
        display->spi = NULL;
    }
    if (display->pins_configured && display->cs_pin >= 0 && display->cs_pin < 64) {
        (void)gpio_set_level((gpio_num_t)display->cs_pin, 1);
    }
    if (display->pins_configured && display->reset_pin >= 0 && display->reset_pin < 64) {
        (void)gpio_set_level((gpio_num_t)display->reset_pin, 1);
    }

    memset(display, 0, sizeof(*display));
    display->cs_pin = -1;
    display->dc_pin = -1;
    display->reset_pin = -1;
    display->last_error = ESP_OK;
}

u8g2_t *pcd8544_get_u8g2(pcd8544_t *display)
{
    return display != NULL ? &display->u8g2 : NULL;
}
