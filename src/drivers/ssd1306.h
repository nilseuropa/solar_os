#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_WIDTH 128U
#define SSD1306_HEIGHT 64U
#define SSD1306_BUFFER_SIZE 1024U
#define SSD1306_I2C_TRANSFER_MAX 32U

typedef enum {
    SSD1306_CONTROLLER_SSD1306 = 0,
    SSD1306_CONTROLLER_SH1106,
} ssd1306_controller_t;

typedef struct {
    u8g2_t u8g2;
    uint8_t buffer[SSD1306_BUFFER_SIZE];
    uint8_t transfer[SSD1306_I2C_TRANSFER_MAX];
    size_t transfer_len;
    uint8_t address;
    esp_err_t last_error;
    bool initialized;
} ssd1306_t;

esp_err_t ssd1306_init(ssd1306_t *display,
                       uint8_t address,
                       ssd1306_controller_t controller);
void ssd1306_deinit(ssd1306_t *display);
u8g2_t *ssd1306_get_u8g2(ssd1306_t *display);

#ifdef __cplusplus
}
#endif
