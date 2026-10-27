#include "solar_os_dlp_display.h"
#include <stdio.h>
#include <string.h>

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count) {
  if (!name || !bindings) return ESP_ERR_INVALID_ARG;
  const char *stream = NULL;
  solar_os_display_link_options_t options = {.timeout_ms = 5000, .format = -1};
  for (size_t i = 0; i < count; i++) {
    if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM) stream = bindings[i].target;
    else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
        !strcmp(bindings[i].role, "timeout-ms")) options.timeout_ms = bindings[i].value;
    else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
        !strcmp(bindings[i].role, "compressed") &&
        (bindings[i].value == 0 || bindings[i].value == 1)) options.compressed = bindings[i].value != 0;
    else return ESP_ERR_INVALID_ARG;
  }
  if (!stream) return ESP_ERR_INVALID_ARG;
  char owner[SOLAR_OS_STREAM_OWNER_MAX];
  int size = snprintf(owner, sizeof(owner), "dlp:%s", name);
  if (size < 0 || size >= sizeof(owner)) return ESP_ERR_INVALID_ARG;
  solar_os_display_link_t *link = NULL;
  esp_err_t error = solar_os_display_link_open_stream(stream, owner, &options, &link);
  if (error != ESP_OK) return error;
  return solar_os_dlp_display_register(name, "stream-dlp", link, 0);
}
static const solar_os_expansion_binding_spec_t bindings[] = {
  {.key = "compressed", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
    .role = "compressed", .has_value_range = true, .min_value = 0, .max_value = 1},
  {.key = "stream", .value_hint = "duplex-byte-stream", .kind = SOLAR_OS_EXPANSION_BINDING_BYTE_STREAM,
    .role = "stream", .required = true},
  {.key = "timeout-ms", .value_hint = "ms", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
    .role = "timeout-ms", .has_value_range = true, .min_value = 100, .max_value = 60000},
};
const solar_os_expansion_driver_t solar_os_stream_dlp_expansion_driver = {
  .name = "stream-dlp", .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
  .summary = "DLP display on an existing duplex byte stream",
  .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
  .attach = attach, .detach = solar_os_dlp_display_detach,
};
