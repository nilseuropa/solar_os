#include "solar_os_dlp_display.h"
#include "solar_os_display.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

typedef struct {
  bool reserved;
  char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
  solar_os_display_link_t *link;
} device_t;
static device_t devices[4];
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t solar_os_dlp_display_register(const char *name, const char *driver,
    solar_os_display_link_t *link, uint32_t budget) {
  esp_err_t error = ESP_ERR_INVALID_ARG;
  device_t *slot = NULL;
  if (!link || !name || !*name || strlen(name) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX || !driver)
    goto fail;
  portENTER_CRITICAL(&lock);
  bool duplicate = false;
  for (size_t i = 0; i < 4; i++) {
    if (devices[i].reserved && !strcmp(devices[i].name, name)) duplicate = true;
    if (!devices[i].reserved && !slot) slot = &devices[i];
  }
  if (slot && !duplicate) {
    slot->reserved = true; strlcpy(slot->name, name, sizeof(slot->name));
  }
  portEXIT_CRITICAL(&lock);
  if (duplicate) { error = ESP_ERR_INVALID_STATE; goto fail; }
  if (!slot) { error = ESP_ERR_NO_MEM; goto fail; }
  const sdlp_info_t *info = solar_os_display_link_info(link);
  solar_os_display_target_t target = {0};
  strlcpy(target.name, name, sizeof(target.name));
  strlcpy(target.source, "expansion", sizeof(target.source));
  strlcpy(target.driver, driver, sizeof(target.driver));
  strlcpy(target.controller, "DLP v1", sizeof(target.controller));
  strlcpy(target.role, "aux", sizeof(target.role));
  target.width = info->width; target.height = info->height;
  target.ready = true; target.black_is_one = true;
  target.u8g2 = solar_os_display_link_u8g2(link);
  if (!target.u8g2) { error = ESP_ERR_NO_MEM; goto release; }
  target.frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT |
      SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT;
  target.surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT;
  target.surface_context = target.frame_context = target.power_state_context = link;
  target.present_surface = solar_os_display_link_surface;
  target.present_frame = solar_os_display_link_frame;
  target.present_power_state = solar_os_display_link_power_state;
  /* Required scheduling preference, not a transport throughput estimate.
   * Each synchronous presentation still waits for peripheral completion. */
  target.preferred_stream_fps = 25;
  target.presentation_stack_bytes = 8192;
  target.max_stream_pixels_per_second = budget;
  if (budget) {
    uint32_t fps = budget / ((uint32_t)info->width * info->height);
    target.preferred_stream_fps = fps > UINT16_MAX ? UINT16_MAX : fps ? fps : 1;
  }
  error = solar_os_display_register_target(&target);
release:
  portENTER_CRITICAL(&lock);
  if (error == ESP_OK) slot->link = link;
  else memset(slot, 0, sizeof(*slot));
  portEXIT_CRITICAL(&lock);
  if (error == ESP_OK) return ESP_OK;
fail:
  solar_os_display_link_destroy(link);
  return error;
}

esp_err_t solar_os_dlp_display_detach(const char *name) {
  if (!name) return ESP_ERR_INVALID_ARG;
  device_t *slot = NULL;
  portENTER_CRITICAL(&lock);
  for (size_t i = 0; i < 4; i++)
    if (devices[i].reserved && !strcmp(devices[i].name, name)) { slot = &devices[i]; break; }
  solar_os_display_link_t *link = slot ? slot->link : NULL;
  portEXIT_CRITICAL(&lock);
  if (!link) return ESP_ERR_NOT_FOUND;
  esp_err_t error = solar_os_display_unregister_target(name);
  if (error != ESP_OK) return error;
  solar_os_display_link_destroy(link);
  portENTER_CRITICAL(&lock);
  memset(slot, 0, sizeof(*slot));
  portEXIT_CRITICAL(&lock);
  return ESP_OK;
}
