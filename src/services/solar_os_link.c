#include "solar_os_link.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_queue.h"

#define LINK_PROTOCOL_VERSION 1U
#define LINK_QUEUE_DEPTH 4U
#define LINK_DUPLICATE_DEPTH 12U
#define LINK_PENDING_DEPTH 8U
#define LINK_DESTROY_WAIT_MS 1100U

typedef struct {
    uint32_t source;
    uint16_t sequence;
    uint8_t type;
    bool active;
} link_duplicate_t;

typedef struct {
    uint32_t destination;
    uint16_t sequence;
    bool active;
} link_pending_t;

typedef struct {
    bool active;
    bool closing;
    uint32_t generation;
    size_t refs;
    char name[SOLAR_OS_LINK_NAME_MAX];
    uint32_t local_id;
    size_t frame_mtu;
    uint16_t next_sequence;
    QueueHandle_t rx_queue;
    QueueHandle_t tx_queue;
    link_duplicate_t duplicates[LINK_DUPLICATE_DEPTH];
    size_t duplicate_next;
    link_pending_t pending[LINK_PENDING_DEPTH];
    size_t pending_next;
    solar_os_link_status_t status;
} link_instance_t;

typedef struct {
    int index;
    uint32_t generation;
    QueueHandle_t rx_queue;
    QueueHandle_t tx_queue;
} link_ref_t;

static SemaphoreHandle_t link_mutex;
static StaticSemaphore_t link_mutex_storage;
static portMUX_TYPE link_init_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR link_instance_t link_instances[SOLAR_OS_LINK_INSTANCE_MAX];
static uint32_t link_generation = 1;

static bool link_name_valid(const char *name)
{
    return name != NULL && name[0] != '\0' &&
           strnlen(name, SOLAR_OS_LINK_NAME_MAX) < SOLAR_OS_LINK_NAME_MAX;
}

static esp_err_t link_ensure_init(void)
{
    portENTER_CRITICAL(&link_init_lock);
    if (!link_mutex) {
        link_mutex = xSemaphoreCreateMutexStatic(&link_mutex_storage);
    }
    const SemaphoreHandle_t mutex = link_mutex;
    portEXIT_CRITICAL(&link_init_lock);
    return mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static int link_find_locked(const char *name, bool include_closing)
{
    for (size_t i = 0; i < SOLAR_OS_LINK_INSTANCE_MAX; i++) {
        if ((link_instances[i].active || (include_closing && link_instances[i].closing)) &&
            strcmp(link_instances[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool link_ref_valid_locked(const link_ref_t *ref)
{
    if (ref == NULL || ref->index < 0 || ref->index >= (int)SOLAR_OS_LINK_INSTANCE_MAX) {
        return false;
    }
    const link_instance_t *instance = &link_instances[ref->index];
    return instance->generation == ref->generation && instance->refs > 0;
}

static esp_err_t link_acquire(const char *name, link_ref_t *ref)
{
    if (!link_name_valid(name) || ref == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = link_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    memset(ref, 0, sizeof(*ref));
    ref->index = -1;
    xSemaphoreTake(link_mutex, portMAX_DELAY);
    const int index = link_find_locked(name, false);
    if (index < 0) {
        xSemaphoreGive(link_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    link_instance_t *instance = &link_instances[index];
    instance->refs++;
    ref->index = index;
    ref->generation = instance->generation;
    ref->rx_queue = instance->rx_queue;
    ref->tx_queue = instance->tx_queue;
    xSemaphoreGive(link_mutex);
    return ESP_OK;
}

static void link_release(link_ref_t *ref)
{
    if (ref == NULL || link_mutex == NULL) {
        return;
    }
    xSemaphoreTake(link_mutex, portMAX_DELAY);
    if (link_ref_valid_locked(ref)) {
        link_instances[ref->index].refs--;
    }
    xSemaphoreGive(link_mutex);
    ref->index = -1;
}

static uint16_t link_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffffU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) != 0 ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void link_write_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void link_write_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static uint16_t link_read_u16(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static uint32_t link_read_u32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
}

static bool link_type_valid(solar_os_link_message_type_t type)
{
    return type == SOLAR_OS_LINK_MESSAGE_TEXT || type == SOLAR_OS_LINK_MESSAGE_BINARY ||
           type == SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT ||
           type == SOLAR_OS_LINK_MESSAGE_STREAM;
}

static bool link_type_requests_acknowledgement(solar_os_link_message_type_t type)
{
    return type == SOLAR_OS_LINK_MESSAGE_TEXT || type == SOLAR_OS_LINK_MESSAGE_BINARY;
}

esp_err_t solar_os_link_encode(const solar_os_link_message_t *message, solar_os_link_frame_t *frame)
{
    if (message == NULL || frame == NULL || message->version != LINK_PROTOCOL_VERSION ||
        !link_type_valid(message->type) || message->payload_len > SOLAR_OS_LINK_PAYLOAD_MAX ||
        (message->type == SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT &&
         (message->payload_len != 0 || (message->flags & SOLAR_OS_LINK_FLAG_ACK_REQUESTED) != 0)) ||
        (message->type == SOLAR_OS_LINK_MESSAGE_STREAM &&
         (message->flags & SOLAR_OS_LINK_FLAG_ACK_REQUESTED) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    frame->data[0] = (uint8_t)((message->version << 4) | (message->flags & 0x0fU));
    frame->data[1] = (uint8_t)message->type;
    link_write_u16(&frame->data[2], message->sequence);
    link_write_u32(&frame->data[4], message->source);
    link_write_u32(&frame->data[8], message->destination);
    if (message->payload_len > 0) {
        memcpy(&frame->data[SOLAR_OS_LINK_HEADER_SIZE], message->payload, message->payload_len);
    }
    const size_t crc_offset = SOLAR_OS_LINK_HEADER_SIZE + message->payload_len;
    link_write_u16(&frame->data[crc_offset], link_crc16(frame->data, crc_offset));
    frame->len = crc_offset + SOLAR_OS_LINK_CRC_SIZE;
    return ESP_OK;
}

esp_err_t
solar_os_link_decode(const uint8_t *frame, size_t frame_len, solar_os_link_message_t *message)
{
    if (frame == NULL || message == NULL ||
        frame_len < SOLAR_OS_LINK_HEADER_SIZE + SOLAR_OS_LINK_CRC_SIZE ||
        frame_len > SOLAR_OS_LINK_FRAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t crc_offset = frame_len - SOLAR_OS_LINK_CRC_SIZE;
    if (link_read_u16(&frame[crc_offset]) != link_crc16(frame, crc_offset)) {
        return ESP_ERR_INVALID_CRC;
    }

    memset(message, 0, sizeof(*message));
    message->version = frame[0] >> 4;
    message->flags = frame[0] & 0x0fU;
    message->type = (solar_os_link_message_type_t)frame[1];
    message->sequence = link_read_u16(&frame[2]);
    message->source = link_read_u32(&frame[4]);
    message->destination = link_read_u32(&frame[8]);
    message->payload_len = crc_offset - SOLAR_OS_LINK_HEADER_SIZE;
    if (message->version != LINK_PROTOCOL_VERSION || !link_type_valid(message->type) ||
        (message->type == SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT &&
         (message->payload_len != 0 || (message->flags & SOLAR_OS_LINK_FLAG_ACK_REQUESTED) != 0)) ||
        (message->type == SOLAR_OS_LINK_MESSAGE_STREAM &&
         (message->flags & SOLAR_OS_LINK_FLAG_ACK_REQUESTED) != 0)) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (message->payload_len > 0) {
        memcpy(message->payload, &frame[SOLAR_OS_LINK_HEADER_SIZE], message->payload_len);
    }
    return ESP_OK;
}

esp_err_t solar_os_link_init(void)
{
    return link_ensure_init();
}

uint32_t solar_os_link_default_local_id(void)
{
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        return 1;
    }

    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < sizeof(mac); i++) {
        hash ^= mac[i];
        hash *= 16777619U;
    }
    if (hash == 0 || hash == SOLAR_OS_LINK_BROADCAST) {
        hash ^= 0xa5a5a5a5U;
    }
    return hash;
}

esp_err_t solar_os_link_create(const char *name, uint32_t local_id, size_t frame_mtu)
{
    if (!link_name_valid(name) || local_id == 0 || local_id == SOLAR_OS_LINK_BROADCAST ||
        frame_mtu < SOLAR_OS_LINK_HEADER_SIZE + SOLAR_OS_LINK_CRC_SIZE ||
        frame_mtu > SOLAR_OS_LINK_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = link_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    QueueHandle_t rx_queue =
        solar_os_queue_create(LINK_QUEUE_DEPTH, sizeof(solar_os_link_message_t));
    if (rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    QueueHandle_t tx_queue = solar_os_queue_create(LINK_QUEUE_DEPTH, sizeof(solar_os_link_frame_t));
    if (tx_queue == NULL) {
        solar_os_queue_delete(rx_queue);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(link_mutex, portMAX_DELAY);
    if (link_find_locked(name, true) >= 0) {
        xSemaphoreGive(link_mutex);
        solar_os_queue_delete(tx_queue);
        solar_os_queue_delete(rx_queue);
        return ESP_ERR_INVALID_STATE;
    }

    link_instance_t *slot = NULL;
    for (size_t i = 0; i < SOLAR_OS_LINK_INSTANCE_MAX; i++) {
        if (!link_instances[i].active && !link_instances[i].closing) {
            slot = &link_instances[i];
            break;
        }
    }
    if (slot == NULL) {
        xSemaphoreGive(link_mutex);
        solar_os_queue_delete(tx_queue);
        solar_os_queue_delete(rx_queue);
        return ESP_ERR_NO_MEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->generation = link_generation++;
    if (link_generation == 0) {
        link_generation = 1;
    }
    strlcpy(slot->name, name, sizeof(slot->name));
    slot->local_id = local_id;
    slot->frame_mtu = frame_mtu;
    slot->next_sequence = 1;
    slot->rx_queue = rx_queue;
    slot->tx_queue = tx_queue;
    slot->status.last_error = ESP_OK;
    xSemaphoreGive(link_mutex);
    return ESP_OK;
}

esp_err_t solar_os_link_destroy(const char *name)
{
    if (!link_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = link_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(link_mutex, portMAX_DELAY);
    const int index = link_find_locked(name, true);
    if (index < 0) {
        xSemaphoreGive(link_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    link_instance_t *instance = &link_instances[index];
    const uint32_t generation = instance->generation;
    instance->active = false;
    instance->closing = true;
    xSemaphoreGive(link_mutex);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LINK_DESTROY_WAIT_MS);
    for (;;) {
        xSemaphoreTake(link_mutex, portMAX_DELAY);
        if (instance->generation != generation) {
            /* Another destroy has already retired this instance. */
            xSemaphoreGive(link_mutex);
            return ESP_OK;
        }
        if (instance->refs == 0) {
            const QueueHandle_t rx_queue = instance->rx_queue;
            const QueueHandle_t tx_queue = instance->tx_queue;
            memset(instance, 0, sizeof(*instance));
            xSemaphoreGive(link_mutex);
            solar_os_queue_delete(tx_queue);
            solar_os_queue_delete(rx_queue);
            return ESP_OK;
        }
        xSemaphoreGive(link_mutex);
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            /* A later call can finish the same retired generation. */
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

size_t solar_os_link_count(void)
{
    if (link_ensure_init() != ESP_OK) {
        return 0;
    }
    size_t count = 0;
    xSemaphoreTake(link_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_LINK_INSTANCE_MAX; i++) {
        if (link_instances[i].active && !link_instances[i].closing) {
            count++;
        }
    }
    xSemaphoreGive(link_mutex);
    return count;
}

static void link_fill_status_locked(const link_instance_t *instance, solar_os_link_status_t *status)
{
    *status = instance->status;
    strlcpy(status->name, instance->name, sizeof(status->name));
    status->local_id = instance->local_id;
    status->frame_mtu = instance->frame_mtu;
    status->next_sequence = instance->next_sequence;
    status->rx_queued = uxQueueMessagesWaiting(instance->rx_queue);
    status->tx_queued = uxQueueMessagesWaiting(instance->tx_queue);
    status->acknowledgements_pending = 0;
    for (size_t i = 0; i < LINK_PENDING_DEPTH; i++) {
        if (instance->pending[i].active) {
            status->acknowledgements_pending++;
        }
    }
}

bool solar_os_link_get(size_t index, solar_os_link_status_t *status)
{
    if (status == NULL || link_ensure_init() != ESP_OK) {
        return false;
    }
    size_t current = 0;
    xSemaphoreTake(link_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_LINK_INSTANCE_MAX; i++) {
        const link_instance_t *instance = &link_instances[i];
        if (!instance->active || instance->closing) {
            continue;
        }
        if (current++ == index) {
            link_fill_status_locked(instance, status);
            xSemaphoreGive(link_mutex);
            return true;
        }
    }
    xSemaphoreGive(link_mutex);
    return false;
}

esp_err_t solar_os_link_get_status(const char *name, solar_os_link_status_t *status)
{
    if (!link_name_valid(name) || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = link_ensure_init();
    if (ret != ESP_OK) {
        return ret;
    }
    xSemaphoreTake(link_mutex, portMAX_DELAY);
    const int index = link_find_locked(name, false);
    if (index < 0) {
        xSemaphoreGive(link_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    link_fill_status_locked(&link_instances[index], status);
    xSemaphoreGive(link_mutex);
    return ESP_OK;
}

static link_pending_t *link_pending_alloc_locked(link_instance_t *instance)
{
    for (size_t i = 0; i < LINK_PENDING_DEPTH; i++) {
        if (!instance->pending[i].active) {
            return &instance->pending[i];
        }
    }
    return &instance->pending[instance->pending_next++ % LINK_PENDING_DEPTH];
}

esp_err_t solar_os_link_send(const char *name,
                             solar_os_link_message_type_t type,
                             uint32_t destination,
                             const void *payload,
                             size_t payload_len,
                             uint16_t *sequence)
{
    if ((type != SOLAR_OS_LINK_MESSAGE_TEXT && type != SOLAR_OS_LINK_MESSAGE_BINARY &&
         type != SOLAR_OS_LINK_MESSAGE_STREAM) ||
        destination == 0 || payload_len > SOLAR_OS_LINK_PAYLOAD_MAX ||
        (type == SOLAR_OS_LINK_MESSAGE_STREAM && destination == SOLAR_OS_LINK_BROADCAST) ||
        (payload == NULL && payload_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    link_ref_t ref;
    esp_err_t ret = link_acquire(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_link_message_t message = {
        .version = LINK_PROTOCOL_VERSION,
        .type = type,
        .destination = destination,
        .payload_len = payload_len,
    };
    xSemaphoreTake(link_mutex, portMAX_DELAY);
    if (!link_ref_valid_locked(&ref)) {
        xSemaphoreGive(link_mutex);
        link_release(&ref);
        return ESP_ERR_INVALID_STATE;
    }
    link_instance_t *instance = &link_instances[ref.index];
    message.source = instance->local_id;
    message.sequence = instance->next_sequence++;
    if (instance->next_sequence == 0) {
        instance->next_sequence = 1;
    }
    if (destination != SOLAR_OS_LINK_BROADCAST &&
        link_type_requests_acknowledgement(type)) {
        message.flags |= SOLAR_OS_LINK_FLAG_ACK_REQUESTED;
    }
    xSemaphoreGive(link_mutex);

    if (payload_len > 0) {
        memcpy(message.payload, payload, payload_len);
    }
    solar_os_link_frame_t frame;
    ret = solar_os_link_encode(&message, &frame);
    if (ret == ESP_OK && frame.len > instance->frame_mtu) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK && xQueueSend(ref.tx_queue, &frame, 0) != pdTRUE) {
        ret = ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(link_mutex, portMAX_DELAY);
    if (link_ref_valid_locked(&ref)) {
        instance = &link_instances[ref.index];
        instance->status.last_error = ret;
        if (ret == ESP_OK) {
            instance->status.tx_messages++;
            if (destination != SOLAR_OS_LINK_BROADCAST &&
                link_type_requests_acknowledgement(type)) {
                link_pending_t *pending = link_pending_alloc_locked(instance);
                pending->active = true;
                pending->destination = destination;
                pending->sequence = message.sequence;
            }
        } else {
            instance->status.dropped++;
        }
    }
    xSemaphoreGive(link_mutex);
    if (sequence != NULL && ret == ESP_OK) {
        *sequence = message.sequence;
    }
    link_release(&ref);
    return ret;
}

esp_err_t solar_os_link_take_tx(const char *name, solar_os_link_frame_t *frame, uint32_t timeout_ms)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    link_ref_t ref;
    esp_err_t ret = link_acquire(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = xQueueReceive(ref.tx_queue, frame, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? ESP_OK
                                                                                  : ESP_ERR_TIMEOUT;
    link_release(&ref);
    return ret;
}

static bool link_is_duplicate_locked(const link_instance_t *instance,
                                     const solar_os_link_message_t *message)
{
    for (size_t i = 0; i < LINK_DUPLICATE_DEPTH; i++) {
        const link_duplicate_t *entry = &instance->duplicates[i];
        if (entry->active && entry->source == message->source &&
            entry->sequence == message->sequence && entry->type == (uint8_t)message->type) {
            return true;
        }
    }
    return false;
}

static void link_remember_locked(link_instance_t *instance, const solar_os_link_message_t *message)
{
    link_duplicate_t *entry =
        &instance->duplicates[instance->duplicate_next++ % LINK_DUPLICATE_DEPTH];
    entry->active = true;
    entry->source = message->source;
    entry->sequence = message->sequence;
    entry->type = (uint8_t)message->type;
}

static void link_ack_pending_locked(link_instance_t *instance,
                                    const solar_os_link_message_t *message)
{
    for (size_t i = 0; i < LINK_PENDING_DEPTH; i++) {
        link_pending_t *pending = &instance->pending[i];
        if (pending->active && pending->destination == message->source &&
            pending->sequence == message->sequence) {
            pending->active = false;
            instance->status.acknowledgements_received++;
            return;
        }
    }
}

static esp_err_t link_queue_ack(link_instance_t *instance,
                                QueueHandle_t tx_queue,
                                const solar_os_link_message_t *message)
{
    solar_os_link_message_t ack = {
        .version = LINK_PROTOCOL_VERSION,
        .type = SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT,
        .sequence = message->sequence,
        .source = instance->local_id,
        .destination = message->source,
    };
    solar_os_link_frame_t frame;
    esp_err_t ret = solar_os_link_encode(&ack, &frame);
    if (ret == ESP_OK && xQueueSend(tx_queue, &frame, 0) != pdTRUE) {
        ret = ESP_ERR_NO_MEM;
    }
    return ret;
}

static esp_err_t link_queue_received(QueueHandle_t rx_queue,
                                     const solar_os_link_message_t *message,
                                     bool *evicted)
{
    *evicted = false;
    if (xQueueSend(rx_queue, message, 0) == pdTRUE) {
        return ESP_OK;
    }

    solar_os_link_message_t oldest;
    if (xQueueReceive(rx_queue, &oldest, 0) == pdTRUE) {
        *evicted = true;
    }
    return xQueueSend(rx_queue, message, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t solar_os_link_ingest(const char *name,
                               const uint8_t *frame,
                               size_t frame_len,
                               solar_os_link_ingest_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    solar_os_link_message_t message;
    esp_err_t ret = solar_os_link_decode(frame, frame_len, &message);
    link_ref_t ref;
    esp_err_t acquire_ret = link_acquire(name, &ref);
    if (acquire_ret != ESP_OK) {
        return acquire_ret;
    }
    if (ret != ESP_OK) {
        xSemaphoreTake(link_mutex, portMAX_DELAY);
        if (link_ref_valid_locked(&ref)) {
            link_instance_t *instance = &link_instances[ref.index];
            instance->status.last_error = ret;
            if (ret == ESP_ERR_INVALID_CRC) {
                instance->status.crc_errors++;
            } else {
                instance->status.dropped++;
            }
        }
        xSemaphoreGive(link_mutex);
        link_release(&ref);
        return ret;
    }

    bool duplicate = false;
    bool send_ack = false;
    xSemaphoreTake(link_mutex, portMAX_DELAY);
    if (!link_ref_valid_locked(&ref)) {
        xSemaphoreGive(link_mutex);
        link_release(&ref);
        return ESP_ERR_INVALID_STATE;
    }
    link_instance_t *instance = &link_instances[ref.index];
    if (message.destination != instance->local_id &&
        message.destination != SOLAR_OS_LINK_BROADCAST) {
        xSemaphoreGive(link_mutex);
        link_release(&ref);
        return ESP_ERR_NOT_FOUND;
    }
    if (message.type == SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT) {
        link_ack_pending_locked(instance, &message);
        instance->status.last_error = ESP_OK;
        result->acknowledgement = true;
        result->message = message;
        xSemaphoreGive(link_mutex);
        link_release(&ref);
        return ESP_OK;
    }
    duplicate = message.type != SOLAR_OS_LINK_MESSAGE_STREAM &&
                link_is_duplicate_locked(instance, &message);
    if (duplicate) {
        instance->status.duplicates++;
    }
    send_ack = message.destination != SOLAR_OS_LINK_BROADCAST &&
               (message.flags & SOLAR_OS_LINK_FLAG_ACK_REQUESTED) != 0;
    xSemaphoreGive(link_mutex);

    bool queue_evicted = false;
    if (!duplicate && message.type != SOLAR_OS_LINK_MESSAGE_STREAM) {
        ret = link_queue_received(ref.rx_queue, &message, &queue_evicted);
    }
    esp_err_t ack_ret = ESP_OK;
    if (ret == ESP_OK && send_ack) {
        ack_ret = link_queue_ack(instance, ref.tx_queue, &message);
    }

    xSemaphoreTake(link_mutex, portMAX_DELAY);
    if (link_ref_valid_locked(&ref)) {
        instance = &link_instances[ref.index];
        if (ret == ESP_OK) {
            if (!duplicate && message.type != SOLAR_OS_LINK_MESSAGE_STREAM) {
                link_remember_locked(instance, &message);
            }
            if (!duplicate) {
                instance->status.rx_messages++;
                if (queue_evicted) {
                    instance->status.dropped++;
                }
            }
            if (send_ack && ack_ret == ESP_OK) {
                instance->status.acknowledgements_sent++;
            } else if (send_ack) {
                instance->status.dropped++;
            }
        } else {
            instance->status.dropped++;
        }
        instance->status.last_error = ret != ESP_OK ? ret : ack_ret;
    }
    xSemaphoreGive(link_mutex);

    result->accepted = ret == ESP_OK && !duplicate;
    result->duplicate = duplicate;
    result->message = message;
    link_release(&ref);
    return ret;
}

esp_err_t
solar_os_link_receive(const char *name, solar_os_link_message_t *message, uint32_t timeout_ms)
{
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    link_ref_t ref;
    esp_err_t ret = link_acquire(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = xQueueReceive(ref.rx_queue, message, pdMS_TO_TICKS(timeout_ms)) == pdTRUE
              ? ESP_OK
              : ESP_ERR_TIMEOUT;
    link_release(&ref);
    return ret;
}

const char *solar_os_link_message_type_name(solar_os_link_message_type_t type)
{
    switch (type) {
    case SOLAR_OS_LINK_MESSAGE_TEXT:
        return "text";
    case SOLAR_OS_LINK_MESSAGE_BINARY:
        return "binary";
    case SOLAR_OS_LINK_MESSAGE_ACKNOWLEDGEMENT:
        return "acknowledgement";
    case SOLAR_OS_LINK_MESSAGE_STREAM:
        return "stream";
    default:
        return "unknown";
    }
}
