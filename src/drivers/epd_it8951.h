#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

#define SOLAR_OS_IT8951_WIDTH 1872U
#define SOLAR_OS_IT8951_HEIGHT 1404U

/* ED103TC2 profile. Both SPI directions are required; there is no DC pin. */
typedef struct {
    const char *spi_bus;
    int cs, reset, busy, power, ite_power;
    unsigned clock_hz;
    unsigned rotation;
    uint16_t vcom_mv;
} solar_os_it8951_config_t;

typedef struct {
    spi_device_handle_t spi;
    solar_os_it8951_config_t config;
    u8g2_t u8g2;
    uint8_t *mono, *gray, *shadow, *transfer;
    uint32_t image_address;
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex;
    esp_err_t last_error;
    bool ready, powered, shadow_valid, shadow_binary, full_refresh;
    unsigned updates;
} solar_os_it8951_t;

esp_err_t solar_os_it8951_init(solar_os_it8951_t *display,
                               const solar_os_it8951_config_t *config);
void solar_os_it8951_deinit(solar_os_it8951_t *display);
esp_err_t solar_os_it8951_set_power_save(solar_os_it8951_t *display, bool sleep);
esp_err_t solar_os_it8951_present(solar_os_it8951_t *display);
esp_err_t solar_os_it8951_present_frame(solar_os_it8951_t *display,
                                       const solar_os_display_raster_t *frame);
esp_err_t solar_os_it8951_present_surface(solar_os_it8951_t *display,
                                         const solar_os_display_surface_t *surface);
const char *solar_os_it8951_mode(const solar_os_it8951_t *display);
esp_err_t solar_os_it8951_set_mode(solar_os_it8951_t *display, const char *mode);
