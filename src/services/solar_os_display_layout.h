#pragma once

#include "esp_err.h"
#include "solar_os_display.h"
#include "solar_os_display_layout_core.h"

#define SOLAR_OS_DISPLAY_LAYOUT_MAX 3U

esp_err_t solar_os_display_layout_create(const solar_os_display_layout_request_v1_t *request,
                                         char *busy_owner, size_t capacity);
esp_err_t solar_os_display_layout_remove(solar_os_display_layout_kind_t kind, const char *name);
size_t solar_os_display_layout_count(void);
bool solar_os_display_layout_get(size_t index, solar_os_display_layout_info_t *info);
esp_err_t solar_os_display_layout_find(solar_os_display_layout_kind_t kind, const char *name,
                                       solar_os_display_layout_info_t *info);
const char *solar_os_display_layout_axis_name(solar_os_display_layout_axis_t axis);
