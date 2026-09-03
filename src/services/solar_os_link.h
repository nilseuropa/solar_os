#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_LINK_NAME_MAX 20
#define SOLAR_OS_LINK_INSTANCE_MAX 3
#define SOLAR_OS_LINK_FRAME_MAX 256
#define SOLAR_OS_LINK_HEADER_SIZE 12
#define SOLAR_OS_LINK_CRC_SIZE 2
#define SOLAR_OS_LINK_PAYLOAD_MAX \
    (SOLAR_OS_LINK_FRAME_MAX - SOLAR_OS_LINK_HEADER_SIZE - SOLAR_OS_LINK_CRC_SIZE)
#define SOLAR_OS_LINK_BROADCAST UINT32_MAX

#define SOLAR_OS_LINK_FLAG_ACK_REQUESTED (1U << 0)
#define SOLAR_OS_LINK_FLAG_RELAYED (1U << 1)

typedef enum {
    SOLAR_OS_LINK_MESSAGE_TEXT = 1,
    SOLAR_OS_LINK_MESSAGE_BINARY = 2,
    SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT = 3,
    SOLAR_OS_LINK_MESSAGE_STREAM = 4,
} solar_os_link_message_type_t;

typedef struct {
    uint8_t version;
    uint8_t flags;
    solar_os_link_message_type_t type;
    uint16_t sequence;
    uint32_t source;
    uint32_t destination;
    size_t payload_len;
    uint8_t payload[SOLAR_OS_LINK_PAYLOAD_MAX];
} solar_os_link_message_t;

typedef struct {
    size_t len;
    uint8_t data[SOLAR_OS_LINK_FRAME_MAX];
} solar_os_link_frame_t;

typedef struct {
    bool accepted;
    bool duplicate;
    bool acknowledgement;
    solar_os_link_message_t message;
} solar_os_link_ingest_result_t;

typedef struct {
    char name[SOLAR_OS_LINK_NAME_MAX];
    uint32_t local_id;
    size_t frame_mtu;
    uint16_t next_sequence;
    size_t rx_queued;
    size_t tx_queued;
    size_t acknowledgements_pending;
    uint32_t tx_messages;
    uint32_t rx_messages;
    uint32_t duplicates;
    uint32_t crc_errors;
    uint32_t acknowledgements_sent;
    uint32_t acknowledgements_received;
    uint32_t dropped;
    esp_err_t last_error;
} solar_os_link_status_t;

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
