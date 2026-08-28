#include "tft_ili9341.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pwm_port.h"
#include "solar_os_buses.h"
#include "solar_os_vector.h"
#define ILI9341_RGB565_BLACK 0x0000
#define ILI9341_RGB565_WHITE 0xffff

static const char *TAG = "tft_ili9341";
static tft_ili9341_t *active_display;

static uint16_t ili9341_rgb888_to_rgb565(uint32_t rgb888) {
  return (uint16_t)(((rgb888 >> 8) & 0xf800U) | ((rgb888 >> 5) & 0x07e0U) |
                    ((rgb888 >> 3) & 0x001fU));
}

static const u8x8_display_info_t ili9341_display_info_template = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 20,
    .post_reset_wait_ms = 120,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = 40000000,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = 0,
    .tile_height = 0,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = 0,
    .pixel_height = 0,
};

static bool gpio_valid(gpio_num_t pin) {
  return pin >= 0 && pin < GPIO_NUM_MAX;
}

static uint64_t gpio_pin_mask(gpio_num_t pin) {
  return gpio_valid(pin) ? (1ULL << (unsigned)pin) : 0ULL;
}

static esp_err_t ili9341_tx_byte(tft_ili9341_t *display, uint8_t value) {
  spi_transaction_t transaction = {
      .flags = SPI_TRANS_USE_TXDATA,
      .length = 8,
      .tx_data = {value},
  };

  return spi_device_polling_transmit(display->spi, &transaction);
}

static esp_err_t ili9341_tx_bytes(tft_ili9341_t *display, const uint8_t *data,
                                  size_t length) {
  while (length > 0) {
    const size_t chunk =
        length > display->line_buffer_size ? display->line_buffer_size : length;
    if (data != display->line_buffer) {
      memcpy(display->line_buffer, data, chunk);
    }

    spi_transaction_t transaction = {
        .length = chunk * 8U,
        .tx_buffer = display->line_buffer,
    };
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(display->spi, &transaction),
                        TAG, "spi transmit failed");
    data += chunk;
    length -= chunk;
  }

  return ESP_OK;
}

static esp_err_t ili9341_cmd_data(tft_ili9341_t *display, uint8_t command,
                                  const uint8_t *data, size_t length) {
  ESP_RETURN_ON_ERROR(gpio_set_level(display->config.dc_pin, 0), TAG,
                      "dc command failed");
  ESP_RETURN_ON_ERROR(ili9341_tx_byte(display, command), TAG,
                      "command transmit failed");
  if (length == 0) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(gpio_set_level(display->config.dc_pin, 1), TAG,
                      "dc data failed");
  return ili9341_tx_bytes(display, data, length);
}

static esp_err_t ili9341_cmd(tft_ili9341_t *display, uint8_t command) {
  return ili9341_cmd_data(display, command, NULL, 0);
}

static bool ili9341_checked_cmd_data(tft_ili9341_t *display, uint8_t command,
                                     const uint8_t *data, size_t length) {
  const esp_err_t err = ili9341_cmd_data(display, command, data, length);
  if (err != ESP_OK) {
    display->last_error = err;
    ESP_LOGE(TAG, "command 0x%02x failed: %s", command, esp_err_to_name(err));
    return false;
  }

  return true;
}

static bool ili9341_checked_cmd(tft_ili9341_t *display, uint8_t command) {
  const esp_err_t err = ili9341_cmd(display, command);
  if (err != ESP_OK) {
    display->last_error = err;
    ESP_LOGE(TAG, "command 0x%02x failed: %s", command, esp_err_to_name(err));
    return false;
  }

  return true;
}

static bool ili9341_backlight_supported(const tft_ili9341_t *display) {
  return display != NULL && gpio_valid(display->config.backlight_pin);
}

static uint8_t ili9341_backlight_duty(const tft_ili9341_t *display,
                                      uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  return display->config.backlight_active_high ? percent
                                               : (uint8_t)(100U - percent);
}

static esp_err_t ili9341_apply_backlight(tft_ili9341_t *display,
                                         uint8_t percent) {
  if (!ili9341_backlight_supported(display)) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  if (display->config.backlight_pwm) {
    return pwm_port_set(display->config.backlight_pin,
                        display->config.backlight_pwm_hz,
                        ili9341_backlight_duty(display, percent));
  }
  const int active = display->config.backlight_active_high ? 1 : 0;
  return gpio_set_level(display->config.backlight_pin,
                        percent > 0 ? active : !active);
}

static void ili9341_set_backlight_power(tft_ili9341_t *display, bool on) {
  if (display == NULL) {
    return;
  }

  display->backlight_power = on;
  const uint8_t percent = on ? display->backlight_percent : 0;
  const esp_err_t err = ili9341_apply_backlight(display, percent);
  if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
    display->last_error = err;
    ESP_LOGW(TAG, "backlight set failed: %s", esp_err_to_name(err));
  }
}

static esp_err_t ili9341_configure_control_pins(tft_ili9341_t *display) {
  uint64_t pin_mask = 0;
  pin_mask |= gpio_pin_mask(display->config.dc_pin);
  pin_mask |= gpio_pin_mask(display->config.reset_pin);
  pin_mask |= gpio_pin_mask(display->config.backlight_pin);

  if (pin_mask == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  const gpio_config_t io_config = {
      .pin_bit_mask = pin_mask,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "gpio config failed");
  ESP_RETURN_ON_ERROR(gpio_set_level(display->config.dc_pin, 1), TAG,
                      "dc high failed");
  if (gpio_valid(display->config.reset_pin)) {
    ESP_RETURN_ON_ERROR(gpio_set_level(display->config.reset_pin, 1), TAG,
                        "rst high failed");
  }
  return ESP_OK;
}

static void ili9341_hardware_reset(tft_ili9341_t *display) {
  if (!gpio_valid(display->config.reset_pin)) {
    return;
  }

  gpio_set_level(display->config.reset_pin, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(display->config.reset_pin, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(display->config.reset_pin, 1);
  vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t ili9341_set_window(tft_ili9341_t *display, uint16_t x0,
                                    uint16_t y0, uint16_t x1, uint16_t y1) {
  const uint8_t col[] = {
      (uint8_t)(x0 >> 8),
      (uint8_t)(x0 & 0xff),
      (uint8_t)(x1 >> 8),
      (uint8_t)(x1 & 0xff),
  };
  const uint8_t row[] = {
      (uint8_t)(y0 >> 8),
      (uint8_t)(y0 & 0xff),
      (uint8_t)(y1 >> 8),
      (uint8_t)(y1 & 0xff),
  };

  ESP_RETURN_ON_ERROR(ili9341_cmd_data(display, 0x2a, col, sizeof(col)), TAG,
                      "set col failed");
  return ili9341_cmd_data(display, 0x2b, row, sizeof(row));
}

static void ili9341_fill_line(tft_ili9341_t *display, uint16_t rgb565,
                              size_t pixels) {
  solar_os_vector_fill_rgb565_be(display->line_buffer, rgb565, pixels);
}

static esp_err_t ili9341_fill_screen(tft_ili9341_t *display, uint16_t rgb565) {
  ESP_RETURN_ON_ERROR(
      ili9341_set_window(display, 0, 0, display->config.width - 1,
                         display->config.height - 1),
      TAG, "window failed");
  ESP_RETURN_ON_ERROR(ili9341_cmd(display, 0x2c), TAG, "ram write failed");
  ESP_RETURN_ON_ERROR(gpio_set_level(display->config.dc_pin, 1), TAG,
                      "dc data failed");

  ili9341_fill_line(display, rgb565, display->config.width);
  for (uint16_t row = 0; row < display->config.height; row++) {
    ESP_RETURN_ON_ERROR(ili9341_tx_bytes(display, display->line_buffer,
                                         display->config.width * 2U),
                        TAG, "fill transmit failed");
  }

  return ESP_OK;
}

static void ili9341_invalidate_shadow(tft_ili9341_t *display) {
  if (display != NULL) {
    display->shadow_valid_rows = 0;
  }
}

static bool ili9341_shadow_matches(tft_ili9341_t *display,
                                   const uint8_t *tile_data, uint8_t x_pos,
                                   uint8_t y_pos, uint8_t count) {
  if (display == NULL || display->shadow == NULL ||
      display->shadow_size != display->buffer_size || tile_data == NULL ||
      y_pos >= display->tile_height || x_pos >= display->tile_width ||
      count == 0 || x_pos + count > display->tile_width ||
      (display->shadow_valid_rows & (1ULL << y_pos)) == 0) {
    return false;
  }

  const size_t offset =
      ((size_t)y_pos * display->buffer_row_bytes) + ((size_t)x_pos * 8U);
  return memcmp(&display->shadow[offset], tile_data, (size_t)count * 8U) == 0;
}

static void ili9341_shadow_update(tft_ili9341_t *display,
                                  const uint8_t *tile_data, uint8_t x_pos,
                                  uint8_t y_pos, uint8_t count) {
  if (display == NULL || display->shadow == NULL ||
      display->shadow_size != display->buffer_size || tile_data == NULL ||
      y_pos >= display->tile_height || x_pos >= display->tile_width ||
      count == 0 || x_pos + count > display->tile_width) {
    return;
  }

  const size_t offset =
      ((size_t)y_pos * display->buffer_row_bytes) + ((size_t)x_pos * 8U);
  memcpy(&display->shadow[offset], tile_data, (size_t)count * 8U);
  display->shadow_valid_rows |= (1ULL << y_pos);
}

static void ili9341_line_from_tile(tft_ili9341_t *display,
                                   const uint8_t *tile_data, int row,
                                   int width) {
  solar_os_vector_expand_1bpp_to_rgb565_be(
      display->line_buffer, tile_data, (unsigned)row,
      display->foreground_rgb565, display->background_rgb565, (size_t)width);
}

static esp_err_t ili9341_draw_tile(tft_ili9341_t *display,
                                   const u8x8_tile_t *tile) {
  if (display == NULL || tile == NULL || tile->tile_ptr == NULL ||
      tile->cnt == 0) {
    return ESP_OK;
  }
  if (tile->x_pos >= display->tile_width ||
      tile->y_pos >= display->tile_height) {
    return ESP_OK;
  }

  uint8_t count = tile->cnt;
  if (tile->x_pos + count > display->tile_width) {
    count = display->tile_width - tile->x_pos;
  }
  if (count == 0) {
    return ESP_OK;
  }

  if (ili9341_shadow_matches(display, tile->tile_ptr, tile->x_pos, tile->y_pos,
                             count)) {
    return ESP_OK;
  }

  const uint16_t x = (uint16_t)tile->x_pos * 8U;
  const uint16_t y = (uint16_t)tile->y_pos * 8U;
  uint16_t width = (uint16_t)count * 8U;
  uint16_t height = 8;
  if (x + width > display->config.width) {
    width = display->config.width - x;
  }
  if (y + height > display->config.height) {
    height = display->config.height - y;
  }
  if (width == 0 || height == 0) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(
      ili9341_set_window(display, x, y, x + width - 1, y + height - 1), TAG,
      "tile window failed");
  ESP_RETURN_ON_ERROR(ili9341_cmd(display, 0x2c), TAG, "tile ram write failed");
  ESP_RETURN_ON_ERROR(gpio_set_level(display->config.dc_pin, 1), TAG,
                      "dc data failed");

  for (uint16_t row = 0; row < height; row++) {
    ili9341_line_from_tile(display, tile->tile_ptr, row, width);
    ESP_RETURN_ON_ERROR(
        ili9341_tx_bytes(display, display->line_buffer, (size_t)width * 2U),
        TAG, "tile transmit failed");
  }

  ili9341_shadow_update(display, tile->tile_ptr, tile->x_pos, tile->y_pos,
                        count);
  return ESP_OK;
}

static esp_err_t ili9341_full_init(tft_ili9341_t *display) {
  ili9341_hardware_reset(display);

  if (!ili9341_checked_cmd(display, 0x01)) {
    return display->last_error;
  }
  vTaskDelay(pdMS_TO_TICKS(120));

  if (display->config.st7796) {
    const uint8_t f0_enable_1[] = {0xc3};
    const uint8_t f0_enable_2[] = {0x96};
    const uint8_t madctl[] = {display->config.madctl};
    const uint8_t colmod[] = {0x55};
    const uint8_t b4[] = {0x01};
    const uint8_t b6[] = {0x80, 0x02, 0x3b};
    const uint8_t e8[] = {0x40, 0x8a, 0x00, 0x00, 0x29, 0x19, 0xa5, 0x33};
    const uint8_t c1[] = {0x06};
    const uint8_t c2[] = {0xa7};
    const uint8_t c5[] = {0x18};
    const uint8_t e0[] = {
        0xf0, 0x09, 0x0b, 0x06, 0x04, 0x15, 0x2f,
        0x54, 0x42, 0x3c, 0x17, 0x14, 0x18, 0x1b,
    };
    const uint8_t e1[] = {
        0xe0, 0x09, 0x0b, 0x06, 0x04, 0x03, 0x2b,
        0x43, 0x42, 0x3b, 0x16, 0x14, 0x17, 0x1b,
    };
    const uint8_t f0_disable_1[] = {0x3c};
    const uint8_t f0_disable_2[] = {0x69};

    if (!ili9341_checked_cmd(display, 0x11)) {
      return display->last_error;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    if (!ili9341_checked_cmd_data(display, 0xf0, f0_enable_1,
                                  sizeof(f0_enable_1)) ||
        !ili9341_checked_cmd_data(display, 0xf0, f0_enable_2,
                                  sizeof(f0_enable_2)) ||
        !ili9341_checked_cmd_data(display, 0x36, madctl, sizeof(madctl)) ||
        !ili9341_checked_cmd_data(display, 0x3a, colmod, sizeof(colmod)) ||
        !ili9341_checked_cmd_data(display, 0xb4, b4, sizeof(b4)) ||
        !ili9341_checked_cmd_data(display, 0xb6, b6, sizeof(b6)) ||
        !ili9341_checked_cmd_data(display, 0xe8, e8, sizeof(e8)) ||
        !ili9341_checked_cmd_data(display, 0xc1, c1, sizeof(c1)) ||
        !ili9341_checked_cmd_data(display, 0xc2, c2, sizeof(c2)) ||
        !ili9341_checked_cmd_data(display, 0xc5, c5, sizeof(c5)) ||
        !ili9341_checked_cmd_data(display, 0xe0, e0, sizeof(e0)) ||
        !ili9341_checked_cmd_data(display, 0xe1, e1, sizeof(e1)) ||
        !ili9341_checked_cmd_data(display, 0xf0, f0_disable_1,
                                  sizeof(f0_disable_1)) ||
        !ili9341_checked_cmd_data(display, 0xf0, f0_disable_2,
                                  sizeof(f0_disable_2))) {
      return display->last_error;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
  } else {
    const uint8_t ef[] = {0x03, 0x80, 0x02};
    const uint8_t cf[] = {0x00, 0xc1, 0x30};
    const uint8_t ed[] = {0x64, 0x03, 0x12, 0x81};
    const uint8_t e8[] = {0x85, 0x00, 0x78};
    const uint8_t cb[] = {0x39, 0x2c, 0x00, 0x34, 0x02};
    const uint8_t f7[] = {0x20};
    const uint8_t ea[] = {0x00, 0x00};
    const uint8_t c0[] = {0x23};
    const uint8_t c1[] = {0x10};
    const uint8_t c5[] = {0x3e, 0x28};
    const uint8_t c7[] = {0x86};
    const uint8_t madctl[] = {display->config.madctl};
    const uint8_t colmod[] = {0x55};
    const uint8_t b1[] = {0x00, 0x18};
    const uint8_t b6[] = {0x08, 0x82, 0x27};
    const uint8_t f2[] = {0x00};
    const uint8_t gamma[] = {0x01};
    const uint8_t e0[] = {
        0x0f, 0x31, 0x2b, 0x0c, 0x0e, 0x08, 0x4e, 0xf1,
        0x37, 0x07, 0x10, 0x03, 0x0e, 0x09, 0x00,
    };
    const uint8_t e1[] = {
        0x00, 0x0e, 0x14, 0x03, 0x11, 0x07, 0x31, 0xc1,
        0x48, 0x08, 0x0f, 0x0c, 0x31, 0x36, 0x0f,
    };

    if (!ili9341_checked_cmd_data(display, 0xef, ef, sizeof(ef)) ||
        !ili9341_checked_cmd_data(display, 0xcf, cf, sizeof(cf)) ||
        !ili9341_checked_cmd_data(display, 0xed, ed, sizeof(ed)) ||
        !ili9341_checked_cmd_data(display, 0xe8, e8, sizeof(e8)) ||
        !ili9341_checked_cmd_data(display, 0xcb, cb, sizeof(cb)) ||
        !ili9341_checked_cmd_data(display, 0xf7, f7, sizeof(f7)) ||
        !ili9341_checked_cmd_data(display, 0xea, ea, sizeof(ea)) ||
        !ili9341_checked_cmd_data(display, 0xc0, c0, sizeof(c0)) ||
        !ili9341_checked_cmd_data(display, 0xc1, c1, sizeof(c1)) ||
        !ili9341_checked_cmd_data(display, 0xc5, c5, sizeof(c5)) ||
        !ili9341_checked_cmd_data(display, 0xc7, c7, sizeof(c7)) ||
        !ili9341_checked_cmd_data(display, 0x36, madctl, sizeof(madctl)) ||
        !ili9341_checked_cmd_data(display, 0x3a, colmod, sizeof(colmod)) ||
        !ili9341_checked_cmd_data(display, 0xb1, b1, sizeof(b1)) ||
        !ili9341_checked_cmd_data(display, 0xb6, b6, sizeof(b6)) ||
        !ili9341_checked_cmd_data(display, 0xf2, f2, sizeof(f2)) ||
        !ili9341_checked_cmd_data(display, 0x26, gamma, sizeof(gamma)) ||
        !ili9341_checked_cmd_data(display, 0xe0, e0, sizeof(e0)) ||
        !ili9341_checked_cmd_data(display, 0xe1, e1, sizeof(e1)) ||
        !ili9341_checked_cmd(display, 0x11)) {
        return display->last_error;
    }

    vTaskDelay(pdMS_TO_TICKS(120));
  }

  ESP_RETURN_ON_ERROR(ili9341_fill_screen(display, display->background_rgb565),
                      TAG, "screen clear failed");

  if (!ili9341_checked_cmd(display, 0x29)) {
    return display->last_error;
  }
  /* The FNK0104S panel requires display inversion on for literal RGB colors. */
  if (display->config.st7796 && !ili9341_checked_cmd(display, 0x21)) {
    return display->last_error;
  }
  vTaskDelay(pdMS_TO_TICKS(20));
  ili9341_set_backlight_power(display, true);

  ili9341_invalidate_shadow(display);
  display->last_error = ESP_OK;
  return ESP_OK;
}

static uint8_t ili9341_u8x8_byte_cb(u8x8_t *u8x8, uint8_t message,
                                    uint8_t arg_int, void *arg_ptr) {
  (void)u8x8;
  (void)message;
  (void)arg_int;
  (void)arg_ptr;
  return 1;
}

static uint8_t ili9341_u8x8_display_cb(u8x8_t *u8x8, uint8_t message,
                                       uint8_t arg_int, void *arg_ptr) {
  if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
    if (active_display == NULL) {
      return 0;
    }
    u8x8_d_helper_display_setup_memory(u8x8, &active_display->display_info);
    return 1;
  }

  tft_ili9341_t *display = active_display;
  if (display == NULL) {
    return 0;
  }

  switch (message) {
    case U8X8_MSG_DISPLAY_INIT:
      return ili9341_full_init(display) == ESP_OK ? 1 : 0;

    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
      ili9341_invalidate_shadow(display);
      if (arg_int == 0) {
        ili9341_set_backlight_power(display, true);
        return ili9341_cmd(display, 0x29) == ESP_OK ? 1 : 0;
      }
      ili9341_set_backlight_power(display, false);
      return ili9341_cmd(display, 0x28) == ESP_OK ? 1 : 0;

    case U8X8_MSG_DISPLAY_DRAW_TILE:
      return ili9341_draw_tile(display, (const u8x8_tile_t *)arg_ptr) == ESP_OK
                 ? 1
                 : 0;

    default:
      return 0;
  }
}

esp_err_t tft_ili9341_init(tft_ili9341_t *display,
                           const tft_ili9341_config_t *config) {
  if (display == NULL || config == NULL || config->spi_bus == NULL ||
      config->spi_bus[0] == '\0' || !gpio_valid(config->cs_pin) ||
      !gpio_valid(config->dc_pin) || config->width == 0 ||
      config->height == 0 || config->width > 480 || config->height > 480) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(display, 0, sizeof(*display));
  display->config = *config;
  if (display->config.spi_clock_hz == 0) {
    display->config.spi_clock_hz = 40000000U;
  }
  if (display->config.backlight_pwm_hz == 0) {
    display->config.backlight_pwm_hz = 20000U;
  }
  if (display->config.rotation == NULL) {
    display->config.rotation = U8G2_R0;
  }
  display->tile_width = (display->config.width + 7U) / 8U;
  display->tile_height = (display->config.height + 7U) / 8U;
  display->buffer_row_bytes = display->tile_width * 8U;
  display->display_info = ili9341_display_info_template;
  display->display_info.sck_clock_hz = display->config.spi_clock_hz;
  display->display_info.tile_width = display->tile_width;
  display->display_info.tile_height = display->tile_height;
  display->display_info.pixel_width = display->config.width;
  display->display_info.pixel_height = display->config.height;
  display->last_error = ESP_OK;
  display->backlight_percent = 100;
  display->foreground_rgb565 = ILI9341_RGB565_BLACK;
  display->background_rgb565 = ILI9341_RGB565_WHITE;

  ESP_RETURN_ON_ERROR(ili9341_configure_control_pins(display), TAG,
                      "control pin config failed");
  ili9341_set_backlight_power(display, false);

  const spi_device_interface_config_t device_config = {
      .clock_speed_hz = (int)display->config.spi_clock_hz,
      .mode = 0,
      .spics_io_num = display->config.cs_pin,
      .queue_size = 1,
  };
  ESP_RETURN_ON_ERROR(
      solar_os_bus_spi_add_device(display->config.spi_bus, &device_config,
                                  &display->spi),
      TAG, "spi add device failed");

  display->line_buffer_size = display->config.width * 2U;
  /* SPI transmits directly from this line buffer, so it must be internal DMA
   * memory. */
  display->line_buffer = heap_caps_malloc(display->line_buffer_size,
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (display->line_buffer == NULL) {
    tft_ili9341_deinit(display);
    return ESP_ERR_NO_MEM;
  }

  display->buffer_size = display->buffer_row_bytes * display->tile_height;
  /* Driver framebuffer only requires byte-addressable memory. */
  display->buffer = heap_caps_malloc(display->buffer_size, MALLOC_CAP_8BIT);
  if (display->buffer == NULL) {
    tft_ili9341_deinit(display);
    return ESP_ERR_NO_MEM;
  }
  memset(display->buffer, 0, display->buffer_size);

  display->shadow_size = display->buffer_size;
  /* Full-frame shadow is large and never used as a DMA source. */
  display->shadow = heap_caps_malloc(display->shadow_size,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (display->shadow == NULL) {
    ESP_LOGW(
        TAG,
        "display shadow allocation failed, partial update skipping disabled");
    display->shadow_size = 0;
  } else {
    memset(display->shadow, 0, display->shadow_size);
    ili9341_invalidate_shadow(display);
  }

  active_display = display;
  u8g2_SetupDisplay(&display->u8g2, ili9341_u8x8_display_cb, u8x8_dummy_cb,
                    ili9341_u8x8_byte_cb, u8x8_dummy_cb);
  u8g2_SetupBuffer(&display->u8g2, display->buffer, display->tile_height,
                   u8g2_ll_hvline_vertical_top_lsb, display->config.rotation);
  u8g2_InitDisplay(&display->u8g2);
  u8g2_SetPowerSave(&display->u8g2, 0);

  return display->last_error;
}

esp_err_t tft_ili9341_resume(tft_ili9341_t *display) {
  if (display == NULL || display->spi == NULL || display->buffer == NULL ||
      display->line_buffer == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  ESP_RETURN_ON_ERROR(ili9341_configure_control_pins(display), TAG,
                      "resume pin config failed");
  active_display = display;
  display->last_error = ESP_OK;
  ili9341_invalidate_shadow(display);
  u8g2_InitDisplay(&display->u8g2);
  u8g2_SetPowerSave(&display->u8g2, 0);
  return display->last_error;
}

esp_err_t tft_ili9341_set_colors(tft_ili9341_t *display,
                                 uint32_t foreground_rgb888,
                                 uint32_t background_rgb888) {
  if (display == NULL || foreground_rgb888 > 0xffffffU ||
      background_rgb888 > 0xffffffU) {
    return ESP_ERR_INVALID_ARG;
  }

  const uint16_t foreground_rgb565 =
      ili9341_rgb888_to_rgb565(foreground_rgb888);
  const uint16_t background_rgb565 =
      ili9341_rgb888_to_rgb565(background_rgb888);
  if (display->foreground_rgb565 != foreground_rgb565 ||
      display->background_rgb565 != background_rgb565) {
    display->foreground_rgb565 = foreground_rgb565;
    display->background_rgb565 = background_rgb565;
    ili9341_invalidate_shadow(display);
  }
  return ESP_OK;
}

void tft_ili9341_deinit(tft_ili9341_t *display) {
  if (display == NULL) {
    return;
  }

  ili9341_set_backlight_power(display, false);

  if (display->spi != NULL) {
    spi_bus_remove_device(display->spi);
    display->spi = NULL;
  }

  if (display->line_buffer != NULL) {
    heap_caps_free(display->line_buffer);
    display->line_buffer = NULL;
  }
  if (display->buffer != NULL) {
    heap_caps_free(display->buffer);
    display->buffer = NULL;
  }
  if (display->shadow != NULL) {
    heap_caps_free(display->shadow);
    display->shadow = NULL;
  }

  if (active_display == display) {
    active_display = NULL;
  }

  display->buffer_size = 0;
  display->shadow_size = 0;
  display->line_buffer_size = 0;
  display->shadow_valid_rows = 0;
}

u8g2_t *tft_ili9341_get_u8g2(tft_ili9341_t *display) {
  return display == NULL ? NULL : &display->u8g2;
}

bool tft_ili9341_backlight_supported(void) {
  return active_display != NULL && ili9341_backlight_supported(active_display);
}

esp_err_t tft_ili9341_get_backlight(const tft_ili9341_t *display,
                                    uint8_t *percent) {
  if (percent == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (display == NULL) {
    *percent = 0;
    return ESP_ERR_INVALID_STATE;
  }
  if (!ili9341_backlight_supported(display)) {
    *percent = 100;
    return ESP_ERR_NOT_SUPPORTED;
  }

  *percent = display->backlight_percent;
  return ESP_OK;
}

esp_err_t tft_ili9341_set_backlight(tft_ili9341_t *display, uint8_t percent) {
  if (display == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (percent > 100) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!ili9341_backlight_supported(display)) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  display->backlight_percent = percent;
  if (display->backlight_power) {
    return ili9341_apply_backlight(display, percent);
  }
  return ESP_OK;
}
