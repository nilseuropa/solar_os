#pragma once
#include "solar_os_display_link.h"
#include "solar_os_expansion.h"
/* Takes ownership of the connection on success and failure. */
esp_err_t solar_os_dlp_display_register(const char *name, const char *driver,
    solar_os_display_link_t *link, uint32_t pixel_budget);
esp_err_t solar_os_dlp_display_detach(const char *name);
/* Checked registration takes ownership only on success. The same owner
 * must detach the target. Failed detach preserves the link for retry. */
esp_err_t solar_os_dlp_display_register_owned(const char *name, const char *driver,
    solar_os_display_link_t *link, uint32_t pixel_budget, const void *owner);
esp_err_t solar_os_dlp_display_detach_owned(const char *name, const void *owner);
extern const solar_os_expansion_driver_t solar_os_stream_dlp_expansion_driver;
