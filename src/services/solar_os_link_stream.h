#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_link.h"
#include "solar_os_port.h"

#define SOLAR_OS_LINK_STREAM_MAX 2U

typedef struct {
    const char *port_label;
    uint32_t acknowledgement_delay_ms;
    uint32_t retry_ms;
    uint32_t retry_jitter_ms;
    uint32_t open_interval_ms;
    uint32_t peer_timeout_ms;
} solar_os_link_stream_config_t;

typedef enum {
    SOLAR_OS_LINK_STREAM_DECODE_NONE = 0,
    SOLAR_OS_LINK_STREAM_DECODE_STRUCTURE,
    SOLAR_OS_LINK_STREAM_DECODE_MAGIC,
    SOLAR_OS_LINK_STREAM_DECODE_VERSION,
    SOLAR_OS_LINK_STREAM_DECODE_OPCODE,
    SOLAR_OS_LINK_STREAM_DECODE_SESSION,
    SOLAR_OS_LINK_STREAM_DECODE_DATA,
    SOLAR_OS_LINK_STREAM_DECODE_CONTROL_SEQUENCE,
    SOLAR_OS_LINK_STREAM_DECODE_CONTROL_DATA,
    SOLAR_OS_LINK_STREAM_DECODE_ACKNOWLEDGEMENT,
} solar_os_link_stream_decode_issue_t;

typedef struct {
    char port[SOLAR_OS_PORT_NAME_MAX];
    char link[SOLAR_OS_LINK_NAME_MAX];
    uint8_t protocol_version;
    uint32_t peer_id;
    bool port_open;
    bool connected;
    size_t data_mtu;
    size_t rx_queued;
    size_t tx_queued;
    size_t tx_inflight;
    uint32_t retry_ms;
    uint32_t retry_jitter_ms;
    uint32_t peer_timeout_ms;
    uint32_t bytes_received;
    uint32_t bytes_sent;
    uint32_t frames_received;
    uint32_t frames_sent;
    uint32_t acknowledgements_received;
    uint32_t acknowledgements_sent;
    uint32_t retries;
    uint32_t reconnects;
    uint32_t dropped;
    uint32_t decode_errors;
    solar_os_link_stream_decode_issue_t last_decode_issue;
    uint8_t last_decode_opcode;
    uint16_t last_decode_sequence;
    size_t last_decode_data_len;
    esp_err_t last_error;
} solar_os_link_stream_status_t;

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

esp_err_t solar_os_link_stream_ingest(const char *link,
                                      const solar_os_link_message_t *message,
                                      uint32_t now_ms);
void solar_os_link_stream_process(const char *link, uint32_t now_ms);
void solar_os_link_stream_transport_stopped(const char *link);
