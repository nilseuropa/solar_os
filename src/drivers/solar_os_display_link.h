#pragma once
#include "esp_err.h"
#include "sdlp.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"
typedef struct solar_os_display_link solar_os_display_link_t;
typedef struct {
  uint32_t timeout_ms; /* Connection and minimum stream command budget; 0 = 5 s. */
  bool compressed; /* Opt in to negotiated lossless LZ4; default is raw/RLE. */
  int format; /* -1 = peripheral preference; otherwise a DLP pixel format. */
  bool (*cancelled)(void *);
  void *cancel_user;
} solar_os_display_link_options_t;
typedef struct {
  const char *bus;
  int cs, ready, reset;
  bool reset_active_high, compressed;
  uint32_t reset_ms, boot_ms, clock_hz;
} solar_os_display_link_config_t;
esp_err_t solar_os_display_link_create(const solar_os_display_link_config_t *,
                                       solar_os_display_link_t **);
/* Opens an existing exclusive duplex byte stream. No display target or text
 * framebuffer is created. The connection owns the stream until destroyed. */
esp_err_t solar_os_display_link_open_stream(const char *stream, const char *owner,
    const solar_os_display_link_options_t *, solar_os_display_link_t **);
/* Present a complete packed image in the negotiated DLP format. Returns only
 * after presentation completion; retained/delta state belongs to this link. */
esp_err_t solar_os_display_link_present(solar_os_display_link_t *, const uint8_t *, size_t);
unsigned solar_os_display_link_format(const solar_os_display_link_t *);
void solar_os_display_link_destroy(solar_os_display_link_t *);
u8g2_t *solar_os_display_link_u8g2(solar_os_display_link_t *);
const sdlp_info_t *solar_os_display_link_info(const solar_os_display_link_t *);
esp_err_t solar_os_display_link_surface(void *,
                                        const solar_os_display_surface_t *);
esp_err_t solar_os_display_link_frame(void *,
                                      const solar_os_display_raster_t *);
esp_err_t solar_os_display_link_power_state(void *);
