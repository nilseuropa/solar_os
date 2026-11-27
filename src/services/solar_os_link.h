#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_link_types_v1.h"

esp_err_t solar_os_link_init(void);
uint32_t solar_os_link_default_local_id(void);

esp_err_t solar_os_link_create(const char *name, uint32_t local_id, size_t frame_mtu);
esp_err_t solar_os_link_destroy(const char *name);
size_t solar_os_link_count(void);
bool solar_os_link_get(size_t index, solar_os_link_status_t *status);
esp_err_t solar_os_link_get_status(const char *name, solar_os_link_status_t *status);

esp_err_t solar_os_link_send(const char *name,
                             solar_os_link_message_type_t type,
                             uint32_t destination,
                             const void *payload,
                             size_t payload_len,
                             uint16_t *sequence);
esp_err_t
solar_os_link_take_tx(const char *name, solar_os_link_frame_t *frame, uint32_t timeout_ms);
esp_err_t solar_os_link_ingest(const char *name,
                               const uint8_t *frame,
                               size_t frame_len,
                               solar_os_link_ingest_result_t *result);
esp_err_t
solar_os_link_receive(const char *name, solar_os_link_message_t *message, uint32_t timeout_ms);

esp_err_t solar_os_link_encode(const solar_os_link_message_t *message,
                               solar_os_link_frame_t *frame);
esp_err_t
solar_os_link_decode(const uint8_t *frame, size_t frame_len, solar_os_link_message_t *message);
const char *solar_os_link_message_type_name(solar_os_link_message_type_t type);
