#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCD8544_WIDTH 84U
#define PCD8544_HEIGHT 48U
#define PCD8544_BUFFER_SIZE 528U

typedef struct {
    u8g2_t u8g2;
    uint8_t buffer[PCD8544_BUFFER_SIZE];
    spi_device_handle_t spi;
    int cs_pin;
    int dc_pin;
    int reset_pin;
    esp_err_t last_error;
    bool bus_acquired;
    bool pins_configured;
} pcd8544_t;

esp_err_t pcd8544_init(pcd8544_t *display, int cs_pin, int dc_pin, int reset_pin);
void pcd8544_deinit(pcd8544_t *display);
u8g2_t *pcd8544_get_u8g2(pcd8544_t *display);

#ifdef __cplusplus
}
#endif
