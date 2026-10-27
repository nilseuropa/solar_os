#include "solar_os_link_stream.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_queue.h"

#define LINK_STREAM_QUEUE_BYTES 2048U
#define LINK_STREAM_HEADER_SIZE 20U
#define LINK_STREAM_MAGIC_0 0x53U
#define LINK_STREAM_MAGIC_1 0x56U
#define LINK_STREAM_VERSION 3U
#define LINK_STREAM_ACK_DELAY_MS 10U
#define LINK_STREAM_RETRY_MS 800U
#define LINK_STREAM_RETRY_JITTER_MS 400U
#define LINK_STREAM_WINDOW_SIZE 8U
#define LINK_STREAM_OPEN_INTERVAL_MS 15000U
#define LINK_STREAM_PEER_TIMEOUT_MS 45000U
#define LINK_STREAM_PORT_WRITE_TIMEOUT_MS 5000U

typedef enum {
    LINK_STREAM_OPCODE_OPEN = 1,
    LINK_STREAM_OPCODE_DATA = 2,
    LINK_STREAM_OPCODE_ACK = 3,
    LINK_STREAM_OPCODE_CLOSE = 4,
} link_stream_opcode_t;

typedef struct {
    uint16_t sequence;
    size_t len;
    uint8_t data[SOLAR_OS_LINK_PAYLOAD_MAX - LINK_STREAM_HEADER_SIZE];
    bool sent;
    bool ever_sent;
    uint32_t last_send_ms;
} link_stream_tx_slot_t;

typedef struct {
    bool active;
    uint32_t generation;
    char port[SOLAR_OS_PORT_NAME_MAX];
    char link[SOLAR_OS_LINK_NAME_MAX];
    uint32_t peer_id;
    solar_os_link_stream_config_t config;
    uint8_t window_frames;
    size_t data_mtu;
    QueueHandle_t rx_queue;
    QueueHandle_t tx_queue;
    bool port_open;
    bool transport_stopped;
    uint32_t local_session;
    bool local_session_confirmed;
    uint32_t remote_session;
    bool had_remote_session;
    uint32_t remote_last_seen_ms;
    uint32_t next_open_ms;
    uint32_t last_open_ms;
    bool open_sent;
    uint16_t next_tx_sequence;
    uint16_t expected_rx_sequence;
    bool received_any;
    link_stream_tx_slot_t tx_window[LINK_STREAM_WINDOW_SIZE];
    size_t tx_window_count;
    /* Byte counters wrap modulo 2^32. The receive right edge is absolute in
     * the peer transmit epoch, so duplicate/stale credit cannot inflate it. */
    uint32_t tx_assigned_bytes;
    uint32_t remote_receive_edge;
    uint32_t rx_received_bytes;
    uint32_t advertised_receive_edge;
    bool credit_advertised;
    uint32_t next_credit_probe_ms;
    unsigned unacknowledged_frames;
    bool acknowledgement_pending;
    uint32_t acknowledgement_due_ms;
    bool close_pending;
    uint32_t close_session;
    solar_os_link_stream_status_t counters;
} link_stream_t;

typedef struct {
    bool valid;
    uint32_t generation;
    link_stream_opcode_t opcode;
    char port[SOLAR_OS_PORT_NAME_MAX];
    char link[SOLAR_OS_LINK_NAME_MAX];
    uint32_t peer_id;
    uint32_t session;
    uint16_t sequence;
    uint32_t acknowledgement_session;
    uint16_t acknowledgement_sequence;
    uint32_t receive_edge;
    size_t data_len;
    uint8_t data[SOLAR_OS_LINK_PAYLOAD_MAX - LINK_STREAM_HEADER_SIZE];
} link_stream_action_t;

static SemaphoreHandle_t stream_mutex;
static EXT_RAM_BSS_ATTR link_stream_t streams[SOLAR_OS_LINK_STREAM_MAX];
static uint32_t stream_generation = 1U;
static size_t stream_next_index;

static uint16_t stream_next_sequence(uint16_t sequence)
{
    sequence++;
    return sequence != 0U ? sequence : 1U;
}

static uint16_t stream_previous_sequence(uint16_t sequence)
{
    return sequence == 1U ? UINT16_MAX : (uint16_t)(sequence - 1U);
}

static bool stream_sequence_is_recent(uint16_t sequence, uint16_t expected)
{
    uint16_t previous = expected;
    for (size_t i = 0; i < LINK_STREAM_WINDOW_SIZE; i++) {
        previous = stream_previous_sequence(previous);
        if (sequence == previous) {
            return true;
        }
    }
    return false;
}

static uint32_t stream_retry_delay_ms(const link_stream_t *stream,
                                      const link_stream_tx_slot_t *slot)
{
    const uint32_t mixed = stream->local_session ^
        ((uint32_t)slot->sequence * 2654435761U);
    return stream->config.retry_ms +
        (mixed % (stream->config.retry_jitter_ms + 1U));
}

static uint32_t stream_new_session(void)
{
    uint32_t session = esp_random();
    return session != 0U ? session : 1U;
}

static void stream_write_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void stream_write_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static uint16_t stream_read_u16(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static uint32_t stream_read_u32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) |
           src[3];
}

static esp_err_t stream_ensure_init(void)
{
    if (stream_mutex != NULL) {
        return ESP_OK;
    }
    stream_mutex = xSemaphoreCreateMutex();
    return stream_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static int stream_find_port_locked(const char *port)
{
    for (size_t i = 0; i < SOLAR_OS_LINK_STREAM_MAX; i++) {
        if (streams[i].active && strcmp(streams[i].port, port) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int stream_find_peer_locked(const char *link, uint32_t peer_id)
{
    for (size_t i = 0; i < SOLAR_OS_LINK_STREAM_MAX; i++) {
        if (streams[i].active && streams[i].peer_id == peer_id &&
            strcmp(streams[i].link, link) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void stream_reset_queues_locked(link_stream_t *stream)
{
    if (stream->rx_queue != NULL) {
        xQueueReset(stream->rx_queue);
    }
    if (stream->tx_queue != NULL) {
        xQueueReset(stream->tx_queue);
    }
}

static void stream_reset_local_locked(link_stream_t *stream, bool preserve_outstanding)
{
    stream->local_session = stream_new_session();
    stream->local_session_confirmed = false;
    stream->open_sent = false;
    stream->next_tx_sequence = 1U;
    stream->tx_assigned_bytes = 0U;
    stream->remote_receive_edge = 0U;
    stream->next_credit_probe_ms = 0U;
    if (!preserve_outstanding) {
        memset(stream->tx_window, 0, sizeof(stream->tx_window));
        stream->tx_window_count = 0U;
    }
    for (size_t i = 0; i < stream->tx_window_count; i++) {
        link_stream_tx_slot_t *slot = &stream->tx_window[i];
        stream->tx_assigned_bytes += (uint32_t)slot->len;
        slot->sequence = stream->next_tx_sequence;
        stream->next_tx_sequence = stream_next_sequence(stream->next_tx_sequence);
        slot->sent = false;
        slot->ever_sent = false;
        slot->last_send_ms = 0U;
    }
}

static bool stream_connected_locked(const link_stream_t *stream, uint32_t now_ms)
{
    return stream->port_open && !stream->transport_stopped && stream->remote_session != 0U &&
           (uint32_t)(now_ms - stream->remote_last_seen_ms) <=
               stream->config.peer_timeout_ms;
}

static void stream_fill_status_locked(const link_stream_t *stream,
                                      uint32_t now_ms,
                                      solar_os_link_stream_status_t *status)
{
    *status = stream->counters;
    strlcpy(status->port, stream->port, sizeof(status->port));
    strlcpy(status->link, stream->link, sizeof(status->link));
    status->protocol_version = LINK_STREAM_VERSION;
    status->peer_id = stream->peer_id;
    status->port_open = stream->port_open;
    status->connected = stream_connected_locked(stream, now_ms);
    status->data_mtu = stream->data_mtu;
    status->window_frames = stream->window_frames;
    const int32_t credit = (int32_t)(stream->remote_receive_edge - stream->tx_assigned_bytes);
    status->tx_credit = credit > 0 ? (size_t)credit : 0U;
    status->retry_ms = stream->config.retry_ms;
    status->retry_jitter_ms = stream->config.retry_jitter_ms;
    status->peer_timeout_ms = stream->config.peer_timeout_ms;
    status->rx_queued = stream->rx_queue != NULL ? uxQueueMessagesWaiting(stream->rx_queue) : 0U;
    status->tx_queued = stream->tx_queue != NULL ? uxQueueMessagesWaiting(stream->tx_queue) : 0U;
    status->tx_inflight = 0U;
    for (size_t i = 0; i < stream->tx_window_count; i++) {
        status->tx_inflight += stream->tx_window[i].len;
    }
}

static esp_err_t stream_port_open(void *user)
{
    link_stream_t *stream = (link_stream_t *)user;
    if (stream == NULL || stream_ensure_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    if (!stream->active || stream->port_open) {
        xSemaphoreGive(stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    stream_reset_queues_locked(stream);
    stream->port_open = true;
    stream->transport_stopped = false;
    stream->remote_session = 0U;
    stream->had_remote_session = false;
    stream->remote_last_seen_ms = 0U;
    stream->expected_rx_sequence = 1U;
    stream->received_any = false;
    stream->rx_received_bytes = 0U;
    stream->credit_advertised = false;
    stream->unacknowledged_frames = 0U;
    stream->acknowledgement_pending = false;
    stream->acknowledgement_due_ms = 0U;
    stream->close_pending = false;
    stream_reset_local_locked(stream, false);
    stream->counters.last_error = ESP_OK;
    xSemaphoreGive(stream_mutex);
    return ESP_OK;
}

static esp_err_t stream_port_close(void *user)
{
    link_stream_t *stream = (link_stream_t *)user;
    if (stream == NULL || stream_ensure_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    if (!stream->active || !stream->port_open) {
        xSemaphoreGive(stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    stream->close_pending = stream->local_session != 0U;
    stream->close_session = stream->local_session;
    stream->port_open = false;
    stream->remote_session = 0U;
    stream->acknowledgement_pending = false;
    stream->acknowledgement_due_ms = 0U;
    memset(stream->tx_window, 0, sizeof(stream->tx_window));
    stream->tx_window_count = 0U;
    stream_reset_queues_locked(stream);
    xSemaphoreGive(stream_mutex);
    return ESP_OK;
}

static esp_err_t stream_port_read(void *user,
                                  uint8_t *data,
                                  size_t len,
                                  uint32_t timeout_ms,
                                  size_t *read_len)
{
    link_stream_t *stream = (link_stream_t *)user;
    if (read_len != NULL) {
        *read_len = 0U;
    }
    if (stream == NULL || (data == NULL && len > 0U) || len == 0U) {
        return len == 0U ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    const bool ready = stream->active && stream->port_open;
    QueueHandle_t queue = stream->rx_queue;
    xSemaphoreGive(stream_mutex);
    if (!ready || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t used = 0U;
    if (xQueueReceive(queue, &data[used], pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    used++;
    while (used < len && xQueueReceive(queue, &data[used], 0) == pdTRUE) {
        used++;
    }
    if (read_len != NULL) {
        *read_len = used;
    }
    return ESP_OK;
}

static esp_err_t stream_port_write(void *user,
                                   const uint8_t *data,
                                   size_t len,
                                   size_t *written)
{
    link_stream_t *stream = (link_stream_t *)user;
    if (written != NULL) {
        *written = 0U;
    }
    if (stream == NULL || (data == NULL && len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0U) {
        return ESP_OK;
    }

    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    const bool ready = stream->active && stream->port_open;
    QueueHandle_t queue = stream->tx_queue;
    xSemaphoreGive(stream_mutex);
    if (!ready || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(LINK_STREAM_PORT_WRITE_TIMEOUT_MS);
    size_t used = 0U;
    while (used < len) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0 ||
            xQueueSend(queue, &data[used], deadline - now) != pdTRUE) {
            if (written != NULL) {
                *written = used;
            }
            return ESP_ERR_TIMEOUT;
        }
        used++;
    }
    if (written != NULL) {
        *written = used;
    }
    return ESP_OK;
}

esp_err_t solar_os_link_stream_init(void)
{
    return stream_ensure_init();
}

void solar_os_link_stream_config_default(
    solar_os_link_stream_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (solar_os_link_stream_config_t){
        .port_label = "SolarOS Link virtual serial",
        .window_frames = LINK_STREAM_WINDOW_SIZE,
        .acknowledgement_delay_ms = LINK_STREAM_ACK_DELAY_MS,
        .retry_ms = LINK_STREAM_RETRY_MS,
        .retry_jitter_ms = LINK_STREAM_RETRY_JITTER_MS,
        .open_interval_ms = LINK_STREAM_OPEN_INTERVAL_MS,
        .peer_timeout_ms = LINK_STREAM_PEER_TIMEOUT_MS,
    };
}

esp_err_t solar_os_link_stream_create(const char *link,
                                      const char *port,
                                      uint32_t peer_id)
{
    solar_os_link_stream_config_t config;
    solar_os_link_stream_config_default(&config);
    return solar_os_link_stream_create_configured(
        link, port, peer_id, &config);
}

esp_err_t solar_os_link_stream_create_configured(
    const char *link,
    const char *port,
    uint32_t peer_id,
    const solar_os_link_stream_config_t *config)
{
    if (link == NULL || link[0] == '\0' || port == NULL || port[0] == '\0' ||
        strnlen(link, SOLAR_OS_LINK_NAME_MAX) >= SOLAR_OS_LINK_NAME_MAX ||
        strnlen(port, SOLAR_OS_PORT_NAME_MAX) >= SOLAR_OS_PORT_NAME_MAX ||
        peer_id == 0U || peer_id == SOLAR_OS_LINK_BROADCAST ||
        config == NULL || config->port_label == NULL ||
        strnlen(config->port_label, SOLAR_OS_PORT_LABEL_MAX) >=
            SOLAR_OS_PORT_LABEL_MAX || config->retry_ms == 0U ||
        config->window_frames == 0U || config->window_frames > LINK_STREAM_WINDOW_SIZE ||
        config->open_interval_ms == 0U ||
        config->peer_timeout_ms < config->retry_ms ||
        UINT32_MAX - config->retry_ms < config->retry_jitter_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = stream_ensure_init();
    if (error != ESP_OK) {
        return error;
    }

    solar_os_link_status_t link_status;
    error = solar_os_link_get_status(link, &link_status);
    if (error != ESP_OK) {
        return error;
    }
    if (peer_id == link_status.local_id ||
        link_status.frame_mtu <= SOLAR_OS_LINK_HEADER_SIZE + SOLAR_OS_LINK_CRC_SIZE +
                                     LINK_STREAM_HEADER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t payload_mtu =
        link_status.frame_mtu - SOLAR_OS_LINK_HEADER_SIZE - SOLAR_OS_LINK_CRC_SIZE;

    QueueHandle_t rx_queue = solar_os_queue_create(LINK_STREAM_QUEUE_BYTES, sizeof(uint8_t));
    if (rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    QueueHandle_t tx_queue = solar_os_queue_create(LINK_STREAM_QUEUE_BYTES, sizeof(uint8_t));
    if (tx_queue == NULL) {
        solar_os_queue_delete(rx_queue);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    if (stream_find_port_locked(port) >= 0 || stream_find_peer_locked(link, peer_id) >= 0) {
        xSemaphoreGive(stream_mutex);
        solar_os_queue_delete(tx_queue);
        solar_os_queue_delete(rx_queue);
        return ESP_ERR_INVALID_STATE;
    }
    link_stream_t *stream = NULL;
    for (size_t i = 0; i < SOLAR_OS_LINK_STREAM_MAX; i++) {
        if (!streams[i].active) {
            stream = &streams[i];
            break;
        }
    }
    if (stream == NULL) {
        xSemaphoreGive(stream_mutex);
        solar_os_queue_delete(tx_queue);
        solar_os_queue_delete(rx_queue);
        return ESP_ERR_NO_MEM;
    }

    memset(stream, 0, sizeof(*stream));
    stream->active = true;
    stream->generation = stream_generation++;
    if (stream_generation == 0U) {
        stream_generation = 1U;
    }
    strlcpy(stream->port, port, sizeof(stream->port));
    strlcpy(stream->link, link, sizeof(stream->link));
    stream->peer_id = peer_id;
    stream->config = *config;
    stream->window_frames = config->window_frames;
    stream->data_mtu = payload_mtu - LINK_STREAM_HEADER_SIZE;
    stream->rx_queue = rx_queue;
    stream->tx_queue = tx_queue;
    stream->expected_rx_sequence = 1U;
    stream->next_tx_sequence = 1U;
    stream->counters.last_error = ESP_OK;
    xSemaphoreGive(stream_mutex);

    const solar_os_port_driver_t driver = {
        .name = stream->port,
        .label = config->port_label,
        .capabilities = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE,
        .read = stream_port_read,
        .write = stream_port_write,
        .open = stream_port_open,
        .close = stream_port_close,
        .user = stream,
    };
    error = solar_os_port_register(&driver);
    if (error == ESP_OK) {
        return ESP_OK;
    }

    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    memset(stream, 0, sizeof(*stream));
    xSemaphoreGive(stream_mutex);
    solar_os_queue_delete(tx_queue);
    solar_os_queue_delete(rx_queue);
    return error;
}

esp_err_t solar_os_link_stream_remove(const char *port)
{
    if (port == NULL || port[0] == '\0' || stream_ensure_init() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    const int index = stream_find_port_locked(port);
    xSemaphoreGive(stream_mutex);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t error = solar_os_port_unregister(port);
    if (error != ESP_OK) {
        return error;
    }

    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    link_stream_t *stream = &streams[index];
    QueueHandle_t rx_queue = stream->rx_queue;
    QueueHandle_t tx_queue = stream->tx_queue;
    memset(stream, 0, sizeof(*stream));
    solar_os_queue_delete(tx_queue);
    solar_os_queue_delete(rx_queue);
    xSemaphoreGive(stream_mutex);
    return ESP_OK;
}

size_t solar_os_link_stream_count(void)
{
    if (stream_ensure_init() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_LINK_STREAM_MAX; i++) {
        if (streams[i].active) {
            count++;
        }
    }
    xSemaphoreGive(stream_mutex);
    return count;
}

bool solar_os_link_stream_get(size_t index,
                              solar_os_link_stream_status_t *status)
{
    if (status == NULL || stream_ensure_init() != ESP_OK) {
        return false;
    }
    const uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    size_t current = 0U;
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_LINK_STREAM_MAX; i++) {
        if (!streams[i].active) {
            continue;
        }
        if (current++ == index) {
            stream_fill_status_locked(&streams[i], now_ms, status);
            xSemaphoreGive(stream_mutex);
            return true;
        }
    }
    xSemaphoreGive(stream_mutex);
    return false;
}

esp_err_t solar_os_link_stream_get_status(const char *port,
                                          solar_os_link_stream_status_t *status)
{
    if (port == NULL || status == NULL || stream_ensure_init() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    const int index = stream_find_port_locked(port);
    if (index < 0) {
        xSemaphoreGive(stream_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    stream_fill_status_locked(&streams[index], now_ms, status);
    xSemaphoreGive(stream_mutex);
    return ESP_OK;
}

static esp_err_t stream_decode(const solar_os_link_message_t *message,
                               link_stream_opcode_t *opcode,
                               uint32_t *session,
                               uint16_t *sequence,
                               uint32_t *acknowledgement_session,
                               uint16_t *acknowledgement_sequence,
                               uint32_t *receive_edge,
                               const uint8_t **data,
                               size_t *data_len,
                               solar_os_link_stream_decode_issue_t *issue)
{
    if (issue != NULL) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_STRUCTURE;
    }
    if (message == NULL || message->type != SOLAR_OS_LINK_MESSAGE_STREAM ||
        message->destination == SOLAR_OS_LINK_BROADCAST ||
        message->payload_len < LINK_STREAM_HEADER_SIZE || opcode == NULL ||
        session == NULL || sequence == NULL ||
        acknowledgement_session == NULL ||
        acknowledgement_sequence == NULL || receive_edge == NULL || data == NULL ||
        data_len == NULL || issue == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (message->payload[0] != LINK_STREAM_MAGIC_0 ||
        message->payload[1] != LINK_STREAM_MAGIC_1) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_MAGIC;
        return ESP_ERR_INVALID_ARG;
    }
    if (message->payload[2] != LINK_STREAM_VERSION) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_VERSION;
        return ESP_ERR_INVALID_ARG;
    }
    const link_stream_opcode_t decoded = (link_stream_opcode_t)message->payload[3];
    if (decoded < LINK_STREAM_OPCODE_OPEN || decoded > LINK_STREAM_OPCODE_CLOSE) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_OPCODE;
        return ESP_ERR_INVALID_ARG;
    }
    *opcode = decoded;
    *session = stream_read_u32(&message->payload[4]);
    *sequence = stream_read_u16(&message->payload[8]);
    *acknowledgement_session = stream_read_u32(&message->payload[10]);
    *acknowledgement_sequence = stream_read_u16(&message->payload[14]);
    *receive_edge = stream_read_u32(&message->payload[16]);
    *data = &message->payload[LINK_STREAM_HEADER_SIZE];
    *data_len = message->payload_len - LINK_STREAM_HEADER_SIZE;
    if (*session == 0U) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_SESSION;
        return ESP_ERR_INVALID_ARG;
    }
    if (decoded == LINK_STREAM_OPCODE_DATA &&
        (*sequence == 0U || *data_len == 0U)) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_DATA;
        return ESP_ERR_INVALID_ARG;
    }
    if (decoded != LINK_STREAM_OPCODE_DATA && *sequence != 0U) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_CONTROL_SEQUENCE;
        return ESP_ERR_INVALID_ARG;
    }
    if (decoded != LINK_STREAM_OPCODE_DATA && *data_len != 0U) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_CONTROL_DATA;
        return ESP_ERR_INVALID_ARG;
    }
    if ((*acknowledgement_session == 0U &&
         (*acknowledgement_sequence != 0U || *receive_edge != 0U)) ||
        (decoded == LINK_STREAM_OPCODE_ACK &&
         *acknowledgement_session == 0U)) {
        *issue = SOLAR_OS_LINK_STREAM_DECODE_ACKNOWLEDGEMENT;
        return ESP_ERR_INVALID_ARG;
    }
    *issue = SOLAR_OS_LINK_STREAM_DECODE_NONE;
    return ESP_OK;
}

static void stream_apply_acknowledgement_locked(
    link_stream_t *stream,
    uint32_t acknowledgement_session,
    uint16_t acknowledgement_sequence,
    uint32_t receive_edge)
{
    if (acknowledgement_session != stream->local_session) {
        return;
    }

    if ((int32_t)(receive_edge - stream->remote_receive_edge) > 0) {
        stream->remote_receive_edge = receive_edge;
    }
    if (stream->tx_window_count == 0U || acknowledgement_sequence == 0U) {
        return;
    }
    size_t acknowledged = 0U;
    uint32_t acknowledged_bytes = 0U;
    for (size_t i = 0; i < stream->tx_window_count; i++) {
        acknowledged_bytes += (uint32_t)stream->tx_window[i].len;
        if (stream->tx_window[i].sequence == acknowledgement_sequence) {
            acknowledged = i + 1U;
            break;
        }
    }
    if (acknowledged == 0U) {
        return;
    }

    const size_t remaining = stream->tx_window_count - acknowledged;
    if (remaining > 0U) {
        memmove(stream->tx_window,
                &stream->tx_window[acknowledged],
                remaining * sizeof(stream->tx_window[0]));
    }
    memset(&stream->tx_window[remaining],
           0,
           acknowledged * sizeof(stream->tx_window[0]));
    stream->tx_window_count = remaining;
    /* Only a matching data acknowledgement proves the peer has adopted this
     * transmit epoch. OPEN alone can be an old periodic announcement. */
    stream->local_session_confirmed = true;
    stream->counters.bytes_sent += acknowledged_bytes;
    stream->counters.acknowledgements_received++;
}

static void stream_note_decode_error(const char *link,
                                     const solar_os_link_message_t *message,
                                     esp_err_t error,
                                     solar_os_link_stream_decode_issue_t issue,
                                     link_stream_opcode_t opcode,
                                     uint16_t sequence,
                                     size_t data_len)
{
    if (link == NULL || message == NULL || stream_ensure_init() != ESP_OK) {
        return;
    }
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    const int index = stream_find_peer_locked(link, message->source);
    if (index >= 0) {
        streams[index].counters.decode_errors++;
        streams[index].counters.dropped++;
        streams[index].counters.last_decode_issue = issue;
        streams[index].counters.last_decode_opcode = (uint8_t)opcode;
        streams[index].counters.last_decode_sequence = sequence;
        streams[index].counters.last_decode_data_len = data_len;
        streams[index].counters.last_error = error;
    }
    xSemaphoreGive(stream_mutex);
}

esp_err_t solar_os_link_stream_ingest(const char *link,
                                      const solar_os_link_message_t *message,
                                      uint32_t now_ms)
{
    link_stream_opcode_t opcode = (link_stream_opcode_t)0;
    uint32_t session = 0U;
    uint16_t sequence = 0U;
    uint32_t acknowledgement_session = 0U;
    uint16_t acknowledgement_sequence = 0U;
    uint32_t receive_edge = 0U;
    const uint8_t *data = NULL;
    size_t data_len = 0U;
    solar_os_link_stream_decode_issue_t decode_issue =
        SOLAR_OS_LINK_STREAM_DECODE_NONE;
    esp_err_t error = stream_decode(message,
                                    &opcode,
                                    &session,
                                    &sequence,
                                    &acknowledgement_session,
                                    &acknowledgement_sequence,
                                    &receive_edge,
                                    &data,
                                    &data_len,
                                    &decode_issue);
    if (error != ESP_OK) {
        stream_note_decode_error(link,
                                 message,
                                 error,
                                 decode_issue,
                                 opcode,
                                 sequence,
                                 data_len);
        return error;
    }
    if (link == NULL || stream_ensure_init() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    const int index = stream_find_peer_locked(link, message->source);
    if (index < 0) {
        xSemaphoreGive(stream_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    link_stream_t *stream = &streams[index];
    if (!stream->port_open) {
        xSemaphoreGive(stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    stream->counters.last_error = ESP_OK;
    if (opcode == LINK_STREAM_OPCODE_OPEN) {
        const uint32_t previous_session = stream->remote_session;
        if (previous_session != session) {
            stream->remote_session = session;
            stream->expected_rx_sequence = 1U;
            stream->received_any = false;
            stream->rx_received_bytes = 0U;
            stream->credit_advertised = false;
            stream->unacknowledged_frames = 0U;
            stream->acknowledgement_pending = false;
            stream->acknowledgement_due_ms = 0U;
            xQueueReset(stream->rx_queue);
            /* Reset an established transmit direction once. Until it is
             * confirmed, a new peer epoch may be the response to our reset;
             * rotating again would make both ends reset each other forever. */
            if (stream->local_session_confirmed) {
                stream_reset_local_locked(stream, true);
            } else {
                stream->open_sent = false;
                for (size_t i = 0; i < stream->tx_window_count; i++) {
                    stream->tx_window[i].sent = false;
                }
            }
            if (stream->had_remote_session) {
                stream->counters.reconnects++;
            } else {
                stream->open_sent = false;
            }
            stream->had_remote_session = true;
        }
        /* Reply to a peer that lost our OPEN (or restarted its transport),
         * even if its transmit epoch stayed the same. Rate limiting prevents
         * two established peers from endlessly echoing each other's OPEN. */
        if (!stream->open_sent ||
            (uint32_t)(now_ms - stream->last_open_ms) >= stream->config.retry_ms) {
            stream->open_sent = false;
        }
    } else if (session != stream->remote_session) {
        stream->counters.dropped++;
        stream->counters.last_error = ESP_ERR_INVALID_STATE;
        xSemaphoreGive(stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    stream->remote_last_seen_ms = now_ms;

    stream_apply_acknowledgement_locked(stream,
                                        acknowledgement_session,
                                        acknowledgement_sequence,
                                        receive_edge);

    switch (opcode) {
    case LINK_STREAM_OPCODE_OPEN:
        break;
    case LINK_STREAM_OPCODE_DATA:
        if (data_len > stream->data_mtu) {
            stream->counters.dropped++;
            stream->counters.last_error = ESP_ERR_INVALID_SIZE;
            error = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (sequence == stream->expected_rx_sequence) {
            if (uxQueueSpacesAvailable(stream->rx_queue) < data_len) {
                stream->counters.dropped++;
                stream->counters.last_error = ESP_ERR_NO_MEM;
                error = ESP_ERR_NO_MEM;
                break;
            }
            for (size_t i = 0; i < data_len; i++) {
                (void)xQueueSend(stream->rx_queue, &data[i], 0);
            }
            stream->expected_rx_sequence = stream_next_sequence(stream->expected_rx_sequence);
            stream->received_any = true;
            stream->rx_received_bytes += (uint32_t)data_len;
            stream->unacknowledged_frames++;
            stream->counters.bytes_received += (uint32_t)data_len;
            stream->counters.frames_received++;
        } else if (!stream_sequence_is_recent(sequence,
                                               stream->expected_rx_sequence)) {
            stream->counters.dropped++;
        }
        if (stream->expected_rx_sequence == 1U && !stream->received_any) {
            break;
        }
        /* Start the timer on the first frame; a continuous burst must not
         * postpone its ACK indefinitely. A half-window is acknowledged early. */
        if (!stream->acknowledgement_pending) {
            stream->acknowledgement_due_ms = now_ms + stream->config.acknowledgement_delay_ms;
        }
        const unsigned batch = (stream->window_frames + 1U) / 2U;
        if (stream->unacknowledged_frames >= (batch > 1U ? batch : 2U)) {
            stream->acknowledgement_due_ms = now_ms;
        }
        stream->acknowledgement_pending = true;
        break;
    case LINK_STREAM_OPCODE_ACK:
        break;
    case LINK_STREAM_OPCODE_CLOSE:
        stream->remote_session = 0U;
        stream->remote_last_seen_ms = 0U;
        stream->acknowledgement_pending = false;
        stream->acknowledgement_due_ms = 0U;
        xQueueReset(stream->rx_queue);
        stream->open_sent = false;
        break;
    default:
        error = ESP_ERR_INVALID_ARG;
        break;
    }
    xSemaphoreGive(stream_mutex);
    return error;
}

static void stream_prepare_action_locked(link_stream_t *stream,
                                         uint32_t now_ms,
                                         link_stream_action_t *action)
{
    if (stream->remote_session != 0U &&
        (uint32_t)(now_ms - stream->remote_last_seen_ms) >
            stream->config.peer_timeout_ms) {
        stream->remote_session = 0U;
        stream->remote_last_seen_ms = 0U;
        stream->open_sent = false;
        stream->acknowledgement_pending = false;
        stream->acknowledgement_due_ms = 0U;
        stream->counters.last_error = ESP_ERR_TIMEOUT;
    }

    action->valid = true;
    action->generation = stream->generation;
    strlcpy(action->port, stream->port, sizeof(action->port));
    strlcpy(action->link, stream->link, sizeof(action->link));
    action->peer_id = stream->peer_id;
    if (stream->remote_session != 0U) {
        action->acknowledgement_session = stream->remote_session;
        action->acknowledgement_sequence = stream->received_any
            ? stream_previous_sequence(stream->expected_rx_sequence) : 0U;
        action->receive_edge = stream->rx_received_bytes +
            (uint32_t)uxQueueSpacesAvailable(stream->rx_queue);
        /* Reading bytes reopens the receive window even with no outbound data.
         * Coalesce credit updates using the same bounded ACK timer. */
        if ((!stream->credit_advertised ||
             action->receive_edge != stream->advertised_receive_edge) &&
            !stream->acknowledgement_pending) {
            stream->acknowledgement_pending = true;
            stream->acknowledgement_due_ms = now_ms + stream->config.acknowledgement_delay_ms;
        }
    }
    if (stream->close_pending) {
        action->opcode = LINK_STREAM_OPCODE_CLOSE;
        action->session = stream->close_session;
        return;
    }
    if (!stream->port_open) {
        action->valid = false;
        return;
    }
    if (!stream->open_sent || (int32_t)(now_ms - stream->next_open_ms) >= 0) {
        action->opcode = LINK_STREAM_OPCODE_OPEN;
        action->session = stream->local_session;
        return;
    }
    if (!stream_connected_locked(stream, now_ms)) {
        action->valid = false;
        return;
    }

    /* Due ACKs take priority over a sustained transmit burst. Sparse echo is
     * still piggybacked if it arrives before the deadline. */
    if (stream->acknowledgement_pending &&
        (int32_t)(now_ms - stream->acknowledgement_due_ms) >= 0) {
        action->opcode = LINK_STREAM_OPCODE_ACK;
        action->session = stream->local_session;
        return;
    }

    while (stream->tx_window_count < stream->window_frames) {
        link_stream_tx_slot_t *slot =
            &stream->tx_window[stream->tx_window_count];
        const int32_t credit = (int32_t)(stream->remote_receive_edge - stream->tx_assigned_bytes);
        if (credit <= 0) {
            break;
        }
        const size_t limit = (size_t)credit < stream->data_mtu ? (size_t)credit : stream->data_mtu;
        size_t len = 0U;
        while (len < limit &&
               xQueueReceive(stream->tx_queue, &slot->data[len], 0) == pdTRUE) {
            len++;
        }
        if (len == 0U) {
            break;
        }
        slot->sequence = stream->next_tx_sequence;
        stream->next_tx_sequence = stream_next_sequence(stream->next_tx_sequence);
        stream->tx_assigned_bytes += (uint32_t)len;
        slot->len = len;
        slot->sent = false;
        slot->ever_sent = false;
        slot->last_send_ms = 0U;
        stream->tx_window_count++;
    }

    link_stream_tx_slot_t *slot = NULL;
    for (size_t i = 0; i < stream->tx_window_count && i < stream->window_frames; i++) {
        if (!stream->tx_window[i].sent) {
            slot = &stream->tx_window[i];
            break;
        }
    }
    if (slot == NULL && stream->tx_window_count > 0U &&
        (uint32_t)(now_ms - stream->tx_window[0].last_send_ms) >=
            stream_retry_delay_ms(stream, &stream->tx_window[0])) {
        for (size_t i = 0; i < stream->tx_window_count; i++) {
            stream->tx_window[i].sent = false;
        }
        slot = &stream->tx_window[0];
    }
    if (slot == NULL) {
        if (uxQueueMessagesWaiting(stream->tx_queue) > 0U &&
            stream->tx_window_count == 0U &&
            (int32_t)(now_ms - stream->next_credit_probe_ms) >= 0) {
            /* An OPEN carries current credit and asks the peer to do likewise.
             * This recovers a lost zero-window update without resetting epochs. */
            action->opcode = LINK_STREAM_OPCODE_OPEN;
            action->session = stream->local_session;
        } else {
            action->valid = false;
        }
        return;
    }

    action->opcode = LINK_STREAM_OPCODE_DATA;
    action->session = stream->local_session;
    action->sequence = slot->sequence;
    action->data_len = slot->len;
    memcpy(action->data, slot->data, action->data_len);
}

static esp_err_t stream_send_action(const link_stream_action_t *action)
{
    uint8_t payload[SOLAR_OS_LINK_PAYLOAD_MAX];
    payload[0] = LINK_STREAM_MAGIC_0;
    payload[1] = LINK_STREAM_MAGIC_1;
    payload[2] = LINK_STREAM_VERSION;
    payload[3] = (uint8_t)action->opcode;
    stream_write_u32(&payload[4], action->session);
    stream_write_u16(&payload[8], action->sequence);
    stream_write_u32(&payload[10], action->acknowledgement_session);
    stream_write_u16(&payload[14], action->acknowledgement_sequence);
    stream_write_u32(&payload[16], action->receive_edge);
    if (action->data_len > 0U) {
        memcpy(&payload[LINK_STREAM_HEADER_SIZE], action->data, action->data_len);
    }
    return solar_os_link_send(action->link,
                              SOLAR_OS_LINK_MESSAGE_STREAM,
                              action->peer_id,
                              payload,
                              LINK_STREAM_HEADER_SIZE + action->data_len,
                              NULL);
}

void solar_os_link_stream_process(const char *link, uint32_t now_ms)
{
    solar_os_link_stream_process_with_window(link, now_ms, LINK_STREAM_WINDOW_SIZE);
}

void solar_os_link_stream_process_with_window(const char *link,
                                              uint32_t now_ms,
                                              uint8_t window_limit)
{
    if (link == NULL || window_limit == 0U || stream_ensure_init() != ESP_OK) {
        return;
    }
    /* Keep at most one frame queued ahead of the transport. The rest stays
     * in our window, leaving packet clients room and avoiding failed enqueue
     * attempts (reported as Link drops) while a radio transmission is busy. */
    solar_os_link_status_t transport;
    if (solar_os_link_get_status(link, &transport) != ESP_OK ||
        transport.tx_queued != 0U) {
        return;
    }
    link_stream_action_t action = {0};
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    for (size_t offset = 0; offset < SOLAR_OS_LINK_STREAM_MAX; offset++) {
        const size_t i = (stream_next_index + offset) % SOLAR_OS_LINK_STREAM_MAX;
        if (streams[i].active && strcmp(streams[i].link, link) == 0) {
            streams[i].window_frames = streams[i].config.window_frames < window_limit
                ? streams[i].config.window_frames : window_limit;
            streams[i].transport_stopped = false;
            stream_prepare_action_locked(&streams[i], now_ms, &action);
            if (action.valid) {
                stream_next_index = (i + 1U) % SOLAR_OS_LINK_STREAM_MAX;
                break;
            }
        }
    }
    xSemaphoreGive(stream_mutex);
    if (!action.valid) {
        return;
    }

    const esp_err_t error = stream_send_action(&action);
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    const int index = stream_find_port_locked(action.port);
    /* A port may close/reopen, or adopt a new epoch, while the transport send
     * runs outside the mutex. Its old completion must not mark the new OPEN
     * or resequenced data as already sent. */
    if (index >= 0 && streams[index].generation == action.generation &&
        streams[index].local_session == action.session) {
        link_stream_t *stream = &streams[index];
        stream->counters.last_error = error;
        if (error == ESP_OK) {
            if (action.acknowledgement_session == stream->remote_session &&
                action.acknowledgement_session != 0U) {
                stream->advertised_receive_edge = action.receive_edge;
                stream->credit_advertised = true;
                if (action.acknowledgement_sequence ==
                    (stream->received_any ? stream_previous_sequence(stream->expected_rx_sequence) : 0U)) {
                    stream->acknowledgement_pending = false;
                    stream->unacknowledged_frames = 0U;
                    stream->counters.acknowledgements_sent++;
                }
            }
            switch (action.opcode) {
            case LINK_STREAM_OPCODE_ACK:
                break;
            case LINK_STREAM_OPCODE_CLOSE:
                stream->close_pending = false;
                break;
            case LINK_STREAM_OPCODE_OPEN:
                stream->next_credit_probe_ms = now_ms + stream->config.retry_ms;
                stream->last_open_ms = now_ms;
                stream->open_sent = true;
                stream->next_open_ms = now_ms +
                    (stream_connected_locked(stream, now_ms)
                         ? stream->config.open_interval_ms
                         : stream->config.retry_ms);
                break;
            case LINK_STREAM_OPCODE_DATA:
                for (size_t i = 0; i < stream->tx_window_count; i++) {
                    link_stream_tx_slot_t *slot = &stream->tx_window[i];
                    if (slot->sequence != action.sequence) {
                        continue;
                    }
                    if (slot->ever_sent) {
                        stream->counters.retries++;
                    }
                    slot->sent = true;
                    slot->ever_sent = true;
                    slot->last_send_ms = now_ms != 0U ? now_ms : 1U;
                    break;
                }
                stream->counters.frames_sent++;
                break;
            default:
                break;
            }
        }
    }
    xSemaphoreGive(stream_mutex);
}

void solar_os_link_stream_transport_stopped(const char *link)
{
    if (link == NULL || stream_ensure_init() != ESP_OK) {
        return;
    }
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_LINK_STREAM_MAX; i++) {
        link_stream_t *stream = &streams[i];
        if (!stream->active || strcmp(stream->link, link) != 0) {
            continue;
        }
        /* The byte-stream endpoints did not close. Preserve their epochs and
         * receive sequence through a temporary radio outage; forgetting only
         * the receive side would reject the peer's next data sequence. */
        stream->transport_stopped = true;
        stream->open_sent = false;
        for (size_t j = 0; j < stream->tx_window_count; j++) {
            stream->tx_window[j].sent = false;
            stream->tx_window[j].last_send_ms = 0U;
        }
        stream->acknowledgement_pending = false;
        stream->acknowledgement_due_ms = 0U;
        stream->counters.last_error = ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(stream_mutex);
}
