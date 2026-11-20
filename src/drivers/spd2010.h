#pragma once

#include "solar_os_display_surface.h"
#include "solar_os_gpio_controller.h"
#include "esp_err.h"
#include "u8g2.h"

typedef struct solar_os_spd2010 solar_os_spd2010_t;
typedef struct {
    int host, sclk, data[4], cs, backlight;
    uint32_t clock_hz;
    uint8_t rotation;
    bool bgr, invert, backlight_pwm, backlight_active_high, power_active_high;
    bool have_power, have_reset;
    bool keep_power_on_shutdown;
    solar_os_gpio_line_ref_t power, reset;
} solar_os_spd2010_config_t;

esp_err_t solar_os_spd2010_create(const solar_os_spd2010_config_t *, solar_os_spd2010_t **);
esp_err_t solar_os_spd2010_destroy(solar_os_spd2010_t *);
u8g2_t *solar_os_spd2010_u8g2(solar_os_spd2010_t *);
esp_err_t solar_os_spd2010_resume(solar_os_spd2010_t *);
esp_err_t solar_os_spd2010_shutdown(solar_os_spd2010_t *);
esp_err_t solar_os_spd2010_power_save(solar_os_spd2010_t *, bool);
esp_err_t solar_os_spd2010_frame(void *, const solar_os_display_raster_t *);
esp_err_t solar_os_spd2010_surface(void *, const solar_os_display_surface_t *);
esp_err_t solar_os_spd2010_colors(solar_os_spd2010_t *, uint32_t foreground, uint32_t background);
bool solar_os_spd2010_has_backlight(const solar_os_spd2010_t *);
esp_err_t solar_os_spd2010_get_brightness(const solar_os_spd2010_t *, uint8_t *);
esp_err_t solar_os_spd2010_set_brightness(solar_os_spd2010_t *, uint8_t);
