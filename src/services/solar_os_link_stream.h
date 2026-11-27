#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_link.h"
#include "solar_os_port.h"

esp_err_t solar_os_link_stream_init(void);
esp_err_t solar_os_link_stream_create(const char *link,
                                      const char *port,
                                      uint32_t peer_id);
void solar_os_link_stream_config_default(
    solar_os_link_stream_config_t *config);
esp_err_t solar_os_link_stream_create_configured(
    const char *link,
    const char *port,
    uint32_t peer_id,
    const solar_os_link_stream_config_t *config);
esp_err_t solar_os_link_stream_remove(const char *port);
size_t solar_os_link_stream_count(void);
bool solar_os_link_stream_get(size_t index,
                              solar_os_link_stream_status_t *status);
esp_err_t solar_os_link_stream_get_status(const char *port,
                                          solar_os_link_stream_status_t *status);

/* A transport adapter can identify the exact stream it provisioned even if
 * the name has since been removed and reused through the generic registry. */
esp_err_t solar_os_link_stream_create_tracked(const char *link, const char *port,
    uint32_t peer_id, const solar_os_link_stream_config_t *config, uint32_t *generation);
esp_err_t solar_os_link_stream_remove_tracked(const char *port, uint32_t generation);
esp_err_t solar_os_link_stream_get_status_tracked(const char *port, uint32_t generation,
    solar_os_link_stream_status_t *status);

esp_err_t solar_os_link_stream_ingest(const char *link,
                                      const solar_os_link_message_t *message,
                                      uint32_t now_ms);
void solar_os_link_stream_process(const char *link, uint32_t now_ms);
/* A transport that only listens when its send queue is empty can cap the
 * configured window to preserve receive turns after each transmission. */
void solar_os_link_stream_process_with_window(const char *link,
                                              uint32_t now_ms,
                                              uint8_t window_limit);
void solar_os_link_stream_transport_stopped(const char *link);
