#pragma once

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "solar_os_native_media_widgets_abi.h"

typedef struct solar_os_signal_widget solar_os_signal_widget_t;

esp_err_t solar_os_signal_widget_create(uint32_t kind, size_t samples, solar_os_signal_widget_t **widget);
void solar_os_signal_widget_destroy(solar_os_signal_widget_t *widget);
void solar_os_signal_widget_reset(solar_os_signal_widget_t *widget);
esp_err_t solar_os_signal_widget_submit_s16(solar_os_signal_widget_t *widget,
    const int16_t *samples, size_t frames, uint8_t channels);
void solar_os_signal_widget_draw(solar_os_signal_widget_t *widget,
    solar_os_gfx_t *gfx, int x, int y, int width, int height);
