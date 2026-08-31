#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_link_stream.h"
#include "solar_os_messaging_types.h"
#include "solar_os_meshcore_stream_codec.h"

#define SOLAR_OS_MESHCORE_STREAM_MAX SOLAR_OS_LINK_STREAM_MAX

typedef struct {
    solar_os_endpoint_id_t endpoint_id;
    uint32_t peer_id;
    uint32_t mesh_packets_sent;
    uint32_t mesh_packets_received;
    uint32_t transport_errors;
    esp_err_t transport_last_error;
    solar_os_link_stream_status_t stream;
} solar_os_meshcore_stream_status_t;

esp_err_t solar_os_meshcore_stream_init(void);
esp_err_t solar_os_meshcore_stream_transport_start(
    const uint8_t public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE]);
void solar_os_meshcore_stream_transport_stop(void);
bool solar_os_meshcore_stream_transport_active(void);

esp_err_t solar_os_meshcore_stream_create(
    const char *port,
    solar_os_endpoint_id_t endpoint_id);
esp_err_t solar_os_meshcore_stream_remove(const char *port);
size_t solar_os_meshcore_stream_count(void);
bool solar_os_meshcore_stream_get(size_t index,
                                  solar_os_meshcore_stream_status_t *status);
esp_err_t solar_os_meshcore_stream_get_status(
    const char *port,
    solar_os_meshcore_stream_status_t *status);

void solar_os_meshcore_stream_process(uint32_t now_ms);
esp_err_t solar_os_meshcore_stream_take_tx(
    solar_os_endpoint_id_t *endpoint_id,
    uint8_t *envelope,
    size_t envelope_capacity,
    size_t *envelope_len);
void solar_os_meshcore_stream_note_tx(solar_os_endpoint_id_t endpoint_id,
                                      esp_err_t error);
esp_err_t solar_os_meshcore_stream_ingest(
    const uint8_t peer_public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE],
    const uint8_t *envelope,
    size_t envelope_len,
    uint32_t now_ms);
