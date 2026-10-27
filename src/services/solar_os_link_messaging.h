#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_link.h"

bool solar_os_link_messaging_available(void);
esp_err_t solar_os_link_messaging_init(void);
esp_err_t solar_os_link_messaging_start(const char *link);
void solar_os_link_messaging_stop(const char *link);
void solar_os_link_messaging_process(const char *link, uint32_t now_ms);
void solar_os_link_messaging_note_transmit(const char *link, const solar_os_link_frame_t *frame, esp_err_t result,
                                           uint32_t now_ms);
esp_err_t solar_os_link_messaging_note_ingest(const char *link, const solar_os_link_ingest_result_t *result,
                                              uint32_t now_ms);
