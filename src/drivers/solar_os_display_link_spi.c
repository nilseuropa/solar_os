#include "solar_os_display_link_transport.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_memory.h"
#include <string.h>
typedef struct {
  solar_os_display_link_config_t config;
  char bus[16];
  spi_device_handle_t spi;
  uint32_t spi_hz;
  uint64_t last_cs_us;
  bool ready_enabled, pins_initialized;
} spi_transport_t;
static uint64_t now_us(void *ctx) {
  (void)ctx;
  return (uint64_t)esp_timer_get_time();
}
static void sleep_us(void *ctx, uint32_t us) {
  (void)ctx;
  uint64_t end = now_us(NULL) + us;
  uint64_t time;
  while ((time = now_us(NULL)) < end) {
    uint32_t remaining = (uint32_t)(end - time);
    TickType_t ticks = pdMS_TO_TICKS(remaining / 1000);
    if (ticks > 1)
      vTaskDelay(ticks - 1);
    else
      esp_rom_delay_us(remaining > 1000 ? 1000 : remaining);
  }
}
static int exchange(void *ctx, const uint8_t tx[256], uint8_t rx[256],
                    uint32_t hz, uint32_t gap, uint64_t deadline) {
  spi_transport_t *d = ctx;
  uint64_t time = now_us(NULL);
  while (time < d->last_cs_us + gap ||
         (d->ready_enabled && !gpio_get_level(d->config.ready))) {
    if (time >= deadline)
      return -1;
    sleep_us(NULL, 100);
    time = now_us(NULL);
  }
  if (time >= deadline)
    return -1;
  if (!d->spi || d->spi_hz != hz) {
    if (d->spi) {
      if (spi_bus_remove_device(d->spi) != ESP_OK)
        return -1;
      d->spi = NULL;
    }
    spi_device_interface_config_t cfg = {.clock_speed_hz = (int)hz,
                                         .mode = 1,
                                         .spics_io_num = -1,
                                         .queue_size = 1};
    if (solar_os_bus_spi_add_device(d->bus, &cfg, &d->spi) != ESP_OK)
      return -1;
    d->spi_hz = hz;
  }
  time = now_us(NULL);
  if (time >= deadline)
    return -1;
  /* ESP-IDF only accepts portMAX_DELAY for bus acquisition. Recheck the
   * protocol deadline after obtaining the bus, before asserting CS. */
  if (spi_device_acquire_bus(d->spi, portMAX_DELAY) != ESP_OK)
    return -1;
  if (now_us(NULL) >= deadline) {
    spi_device_release_bus(d->spi);
    return -1;
  }
  esp_err_t err = gpio_set_level(d->config.cs, 0);
  if (err == ESP_OK) {
    /* Allow a software SPI-slave output-enable handler to observe CS. */
    esp_rom_delay_us(10);
    spi_transaction_t tr = {
        .length = 2048, .rxlength = 2048, .tx_buffer = tx, .rx_buffer = rx};
    err = spi_device_polling_transmit(d->spi, &tr);
    esp_rom_delay_us(2);
  }
  esp_err_t cs_err = gpio_set_level(d->config.cs, 1);
  d->last_cs_us = now_us(NULL);
  spi_device_release_bus(d->spi);
  return err == ESP_OK && cs_err == ESP_OK ? 0 : -1;
}

static esp_err_t prepare(void *ctx, bool reset) {
  spi_transport_t *d = ctx;
  d->ready_enabled = false;
  if (reset && d->config.reset >= 0) {
    gpio_set_level(d->config.reset, d->config.reset_active_high ? 1 : 0);
    sleep_us(NULL, d->config.reset_ms * 1000);
    gpio_set_level(d->config.reset, d->config.reset_active_high ? 0 : 1);
  }
  return ESP_OK;
}
static esp_err_t configured(void *ctx, const sdlp_info_t *info) {
  spi_transport_t *d = ctx;
  if (d->config.ready >= 0 && !(info->capabilities & SDLP_READY)) return ESP_ERR_NOT_SUPPORTED;
  d->ready_enabled = d->config.ready >= 0;
  return ESP_OK;
}
static void destroy(void *ctx) {
  spi_transport_t *d = ctx;
  if (!d) return;
  if (d->spi) spi_bus_remove_device(d->spi);
  if (d->pins_initialized) {
    gpio_set_level(d->config.cs, 1); gpio_reset_pin(d->config.cs);
    if (d->config.ready >= 0) gpio_reset_pin(d->config.ready);
    if (d->config.reset >= 0) {
      gpio_set_level(d->config.reset, d->config.reset_active_high ? 0 : 1);
      gpio_reset_pin(d->config.reset);
    }
  }
  solar_os_memory_free(d);
}
esp_err_t solar_os_display_link_create(const solar_os_display_link_config_t *c,
    solar_os_display_link_t **out) {
  if (!c || !out || !c->bus || strlen(c->bus) >= 16 ||
      !GPIO_IS_VALID_OUTPUT_GPIO(c->cs) ||
      (c->ready >= 0 && !GPIO_IS_VALID_GPIO(c->ready)) ||
      (c->reset >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(c->reset)) ||
      c->cs == c->ready || c->cs == c->reset ||
      (c->ready >= 0 && c->ready == c->reset) || c->clock_hz < 100000 ||
      !c->boot_ms || c->boot_ms > 60000 || !c->reset_ms || c->reset_ms > 1000)
    return ESP_ERR_INVALID_ARG;
  *out = NULL;
  spi_transport_t *d = solar_os_memory_calloc(1, sizeof(*d),
      SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "dlp.spi");
  if (!d) return ESP_ERR_NO_MEM;
  d->config = *c; strlcpy(d->bus, c->bus, sizeof(d->bus)); d->config.bus = d->bus;
  esp_err_t e;
  gpio_set_level(c->cs, 1);
  gpio_config_t pins = {.pin_bit_mask = UINT64_C(1) << c->cs,
                        .mode = GPIO_MODE_OUTPUT};
  if (c->reset >= 0) {
    gpio_set_level(c->reset, c->reset_active_high ? 0 : 1);
    pins.pin_bit_mask |= UINT64_C(1) << c->reset;
  }
  e = gpio_config(&pins);
  if (e != ESP_OK)
    { destroy(d); return e; }
  d->pins_initialized = true;
  if (c->ready >= 0) {
    pins.pin_bit_mask = UINT64_C(1) << c->ready;
    pins.mode = GPIO_MODE_INPUT;
    pins.pull_down_en = GPIO_PULLDOWN_ENABLE;
    e = gpio_config(&pins);
    if (e != ESP_OK)
      { destroy(d); return e; }
  }

  solar_os_display_link_transport_t io = {
    .protocol = {.ctx = d, .now_us = now_us, .sleep_us = sleep_us, .exchange = exchange},
    .prepare = prepare, .configured = configured, .destroy = destroy,
    .reset_available = c->reset >= 0, .clock_hz = c->clock_hz};
  solar_os_display_link_options_t options = {.timeout_ms = c->boot_ms, .format = -1,
      .compressed = c->compressed};
  return solar_os_display_link_open_transport(&io, &options, out);
}
