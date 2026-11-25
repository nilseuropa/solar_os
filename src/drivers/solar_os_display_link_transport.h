#pragma once
#include "solar_os_display_link.h"

/* Transport ownership transfers to the common host, including on open failure.
 * prepare(false) fences a previous connection before rediscovery; prepare(true)
 * additionally resets hardware where available. No baud policy belongs here. */
typedef struct {
  sdlp_transport_t protocol;
  esp_err_t (*prepare)(void *, bool reset);
  esp_err_t (*configured)(void *, const sdlp_info_t *);
  void (*destroy)(void *);
  /* Core transport hook; set/clear only while the host serializes a call. */
  void (*set_call_options)(void *, const solar_os_display_link_call_options_t *);
  bool reset_available;
  uint32_t clock_hz;
} solar_os_display_link_transport_t;

esp_err_t solar_os_display_link_open_transport(
    const solar_os_display_link_transport_t *,
    const solar_os_display_link_options_t *, solar_os_display_link_t **);
esp_err_t solar_os_display_link_open_transport_with_options(
    const solar_os_display_link_transport_t *, const solar_os_display_link_options_t *,
    const solar_os_display_link_call_options_t *, solar_os_display_link_t **);
