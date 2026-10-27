#include "solar_os_spi_dlp.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_display.h"
#include "solar_os_dlp_display.h"
#include <string.h>

esp_err_t solar_os_spi_dlp_attach(const char *name,
                                      const solar_os_expansion_binding_t *b,
                                      size_t n) {
  if (!name || !*name ||
      strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) >=
          SOLAR_OS_DISPLAY_TARGET_NAME_MAX ||
      !b)
    return ESP_ERR_INVALID_ARG;
  solar_os_display_link_config_t c = {.cs = -1,
                                      .ready = -1,
                                      .reset = -1,
                                      .reset_ms = 10,
                                      .boot_ms = 5000,
                                      .clock_hz = 10000000};
  char bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
  for (size_t i = 0; i < n; i++) {
    if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS)
      strlcpy(bus, b[i].target, sizeof(bus));
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS)
      c.cs = b[i].value;
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
             !strcmp(b[i].role, "ready"))
      c.ready = b[i].value;
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
             !strcmp(b[i].role, "reset"))
      c.reset = b[i].value;
    else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
      if (!strcmp(b[i].role, "clock"))
        c.clock_hz = (uint32_t)b[i].value * 1000;
      else if (!strcmp(b[i].role, "reset-ms"))
        c.reset_ms = (uint32_t)b[i].value;
      else if (!strcmp(b[i].role, "boot-ms"))
        c.boot_ms = (uint32_t)b[i].value;
      else if (!strcmp(b[i].role, "compressed") && (b[i].value == 0 || b[i].value == 1))
        c.compressed = b[i].value != 0;
      else if (!strcmp(b[i].role, "reset-active"))
        c.reset_active_high = b[i].value != 0;
      else
        return ESP_ERR_INVALID_ARG;
    } else
      return ESP_ERR_INVALID_ARG;
  }
  solar_os_expansion_spi_bus_t descriptor;
  if (!solar_os_expansion_find_spi_bus(bus, &descriptor, NULL) ||
      descriptor.miso_pin < 0 || descriptor.max_transfer_size < 256 ||
      !solar_os_expansion_spi_cs_allowed(bus, c.cs) || c.clock_hz < 100000 ||
      !c.boot_ms || c.boot_ms > 60000 || !c.reset_ms || c.reset_ms > 1000)
    return ESP_ERR_INVALID_ARG;
  c.bus = bus;
  solar_os_display_link_t *driver = NULL;
  esp_err_t error = solar_os_display_link_create(&c, &driver);
  if (error != ESP_OK) return error;
  const sdlp_info_t *info = solar_os_display_link_info(driver);
  uint32_t hz = c.clock_hz < info->max_clock_hz ? c.clock_hz : info->max_clock_hz;
  uint64_t cell_us = UINT64_C(2048000000) / hz + info->min_gap_us + 100;
  uint32_t bytes_sec = (uint32_t)(UINT64_C(208000000) / (2 * cell_us));
  unsigned bits[] = {1, 2, 4, 8, 16};
  uint32_t pps = bytes_sec * 8 / bits[info->preferred_format];
  return solar_os_dlp_display_register(name, "spi-dlp", driver, pps ? pps : 1);
}
esp_err_t solar_os_spi_dlp_detach(const char *name) {
  return solar_os_dlp_display_detach(name);
}
