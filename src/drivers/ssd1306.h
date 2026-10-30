#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy default geometry also bounds the storage for every supported panel. */
#define SSD1306_WIDTH 128U
#define SSD1306_HEIGHT 64U
#define SSD1306_BUFFER_SIZE 1024U
/* One native page plus the I2C command/data control byte. */
#define SSD1306_I2C_TRANSFER_MAX (SSD1306_WIDTH + 1U)

/* Profile IDs are controller-specific. Zero preserves the 128x64 default.
 * SSD1306: 1=128x32 Univision, 2=64x48, 3=64x32, 4=96x16 ER,
 *          5=96x40, 6=72x40 ER, 7=48x64 Winstar.
 * SH1106:  1=128x32 Visionox, 2=64x32, 3=72x40 Wise. */

typedef enum {
    SSD1306_CONTROLLER_SSD1306 = 0,
    SSD1306_CONTROLLER_SH1106,
} ssd1306_controller_t;

typedef struct {
    esp_err_t (*probe)(void *ctx);
    esp_err_t (*transmit)(void *ctx, const uint8_t *data, size_t len);
    void *ctx;
} ssd1306_io_t;

typedef struct {
    u8g2_t u8g2;
    uint8_t buffer[SSD1306_BUFFER_SIZE];
    uint8_t transfer[SSD1306_I2C_TRANSFER_MAX];
    size_t transfer_len;
    size_t buffer_size;
    uint8_t address;
    ssd1306_io_t io;
    esp_err_t last_error;
    bool initialized;
} ssd1306_t;

esp_err_t ssd1306_init(ssd1306_t *display,
                       uint8_t address,
                       ssd1306_controller_t controller);
/* The caller owns the transport context until deinit, including display
 * updates and the final power-save command. No default bus is initialized. */
esp_err_t ssd1306_init_with_io(ssd1306_t *display,
                               uint8_t address,
                               ssd1306_controller_t controller,
                               const ssd1306_io_t *io);
esp_err_t ssd1306_init_profile_with_io(ssd1306_t *display,
                                      uint8_t address,
                                      ssd1306_controller_t controller,
                                      unsigned panel,
                                      unsigned rotation,
                                      const ssd1306_io_t *io);
void ssd1306_deinit(ssd1306_t *display);
u8g2_t *ssd1306_get_u8g2(ssd1306_t *display);

#ifdef __cplusplus
}
#endif
