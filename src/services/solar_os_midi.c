#include "solar_os_midi.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SOLAR_OS_MIDI_SUBSCRIBER_MAX 4U
#define SOLAR_OS_MIDI_RX_QUEUE_DEPTH 32U
#define SOLAR_OS_MIDI_TX_QUEUE_DEPTH 32U

typedef struct {
    bool active;
    uint32_t token;
    char owner[SOLAR_OS_MIDI_OWNER_MAX];
    solar_os_midi_message_t queue[SOLAR_OS_MIDI_RX_QUEUE_DEPTH];
    size_t head;
    size_t count;
} solar_os_midi_subscriber_t;

typedef struct {
    bool active;
    uint8_t channel;
    uint8_t controller;
    bool has_value;
    uint8_t value;
    uint32_t updates;
} solar_os_midi_cc_stream_t;

static solar_os_midi_subscriber_t midi_subscribers[SOLAR_OS_MIDI_SUBSCRIBER_MAX];
static solar_os_midi_cc_stream_t
    midi_cc_streams[SOLAR_OS_MIDI_CC_STREAM_MAX];
static solar_os_midi_message_t midi_tx_queue[SOLAR_OS_MIDI_TX_QUEUE_DEPTH];
static size_t midi_tx_head;
static size_t midi_tx_count;
static solar_os_midi_status_t midi_status;
static SemaphoreHandle_t midi_mutex;
static StaticSemaphore_t midi_mutex_buffer;
static uint32_t midi_next_token;

static esp_err_t midi_ensure_mutex(void);

static bool midi_cc_stream_address_valid(uint8_t channel, uint8_t controller)
{
    return channel >= 1U && channel <= 16U && controller <= 127U;
}

static void midi_cc_stream_id(uint8_t channel,
                              uint8_t controller,
                              char *id,
                              size_t id_len)
{
    snprintf(id, id_len, "midi.cc.%u.%u", (unsigned)channel,
             (unsigned)controller);
}

static int midi_cc_stream_find_locked(uint8_t channel, uint8_t controller)
{
    for (size_t i = 0; i < SOLAR_OS_MIDI_CC_STREAM_MAX; i++) {
        if (midi_cc_streams[i].active &&
            midi_cc_streams[i].channel == channel &&
            midi_cc_streams[i].controller == controller) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t midi_cc_stream_read_scalar(
    void *user,
    const solar_os_stream_read_options_t *options,
    float *value)
{
    (void)options;
    if (user == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    const solar_os_midi_cc_stream_t *stream = user;
    const bool available = stream->active && midi_status.running &&
        stream->has_value;
    if (available) {
        *value = (float)stream->value;
    }
    xSemaphoreGive(midi_mutex);
    return available ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t midi_ensure_mutex(void)
{
    if (midi_mutex == NULL) {
        midi_mutex = xSemaphoreCreateMutexStatic(&midi_mutex_buffer);
    }
    return midi_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint32_t midi_allocate_token(void)
{
    midi_next_token++;
    if (midi_next_token == 0U) {
        midi_next_token++;
    }
    return midi_next_token;
}

esp_err_t solar_os_midi_subscribe(const char *owner,
                                  solar_os_midi_subscription_t *subscription)
{
    if (owner == NULL || owner[0] == '\0' || subscription == NULL ||
        strnlen(owner, SOLAR_OS_MIDI_OWNER_MAX) >= SOLAR_OS_MIDI_OWNER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    *subscription = (solar_os_midi_subscription_t)SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_MIDI_SUBSCRIBER_MAX; i++) {
        if (midi_subscribers[i].active) {
            continue;
        }
        memset(&midi_subscribers[i], 0, sizeof(midi_subscribers[i]));
        midi_subscribers[i].active = true;
        midi_subscribers[i].token = midi_allocate_token();
        strlcpy(midi_subscribers[i].owner, owner, sizeof(midi_subscribers[i].owner));
        subscription->index = (int)i;
        subscription->token = midi_subscribers[i].token;
        xSemaphoreGive(midi_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(midi_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_midi_unsubscribe(solar_os_midi_subscription_t *subscription)
{
    if (subscription == NULL || subscription->index < 0 ||
        subscription->index >= (int)SOLAR_OS_MIDI_SUBSCRIBER_MAX ||
        subscription->token == 0U || midi_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    solar_os_midi_subscriber_t *subscriber =
        &midi_subscribers[(size_t)subscription->index];
    if (!subscriber->active || subscriber->token != subscription->token) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memset(subscriber, 0, sizeof(*subscriber));
    *subscription = (solar_os_midi_subscription_t)SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

esp_err_t solar_os_midi_receive(solar_os_midi_subscription_t *subscription,
                                solar_os_midi_message_t *message)
{
    if (subscription == NULL || message == NULL || subscription->index < 0 ||
        subscription->index >= (int)SOLAR_OS_MIDI_SUBSCRIBER_MAX ||
        subscription->token == 0U || midi_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    solar_os_midi_subscriber_t *subscriber =
        &midi_subscribers[(size_t)subscription->index];
    if (!subscriber->active || subscriber->token != subscription->token) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (subscriber->count == 0U) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_TIMEOUT;
    }
    *message = subscriber->queue[subscriber->head];
    subscriber->head = (subscriber->head + 1U) % SOLAR_OS_MIDI_RX_QUEUE_DEPTH;
    subscriber->count--;
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

esp_err_t solar_os_midi_send(const solar_os_midi_message_t *message)
{
    if (!solar_os_midi_message_valid(message)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    if (!midi_status.running) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (midi_tx_count >= SOLAR_OS_MIDI_TX_QUEUE_DEPTH) {
        midi_status.tx_drops++;
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NO_MEM;
    }
    const size_t tail = (midi_tx_head + midi_tx_count) % SOLAR_OS_MIDI_TX_QUEUE_DEPTH;
    midi_tx_queue[tail] = *message;
    midi_tx_count++;
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

void solar_os_midi_get_status(solar_os_midi_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (midi_ensure_mutex() != ESP_OK) {
        status->last_error = ESP_ERR_NO_MEM;
        return;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    *status = midi_status;
    xSemaphoreGive(midi_mutex);
}

esp_err_t solar_os_midi_cc_stream_add(uint8_t channel, uint8_t controller)
{
    if (!midi_cc_stream_address_valid(channel, controller)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    if (midi_cc_stream_find_locked(channel, controller) >= 0) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_midi_cc_stream_t *slot = NULL;
    for (size_t i = 0; i < SOLAR_OS_MIDI_CC_STREAM_MAX; i++) {
        if (!midi_cc_streams[i].active) {
            slot = &midi_cc_streams[i];
            break;
        }
    }
    if (slot == NULL) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NO_MEM;
    }
    *slot = (solar_os_midi_cc_stream_t) {
        .active = true,
        .channel = channel,
        .controller = controller,
    };
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_SCALAR,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_SHARED,
        },
        .read_scalar = midi_cc_stream_read_scalar,
        .user = slot,
    };
    midi_cc_stream_id(channel, controller, driver.info.id,
                      sizeof(driver.info.id));
    strlcpy(driver.info.provider, "midi", sizeof(driver.info.provider));
    strlcpy(driver.info.device, "midi", sizeof(driver.info.device));
    strlcpy(driver.info.unit, "CC", sizeof(driver.info.unit));
    strlcpy(driver.info.format, "f32", sizeof(driver.info.format));
    snprintf(driver.info.summary, sizeof(driver.info.summary),
             "MIDI channel %u controller %u", (unsigned)channel,
             (unsigned)controller);
    error = solar_os_stream_register(&driver);
    if (error != ESP_OK) {
        memset(slot, 0, sizeof(*slot));
    }
    xSemaphoreGive(midi_mutex);
    return error;
}

esp_err_t solar_os_midi_cc_stream_remove(uint8_t channel, uint8_t controller)
{
    if (!midi_cc_stream_address_valid(channel, controller)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_error = midi_ensure_mutex();
    if (init_error != ESP_OK) {
        return init_error;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    const int index = midi_cc_stream_find_locked(channel, controller);
    if (index < 0) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    char id[SOLAR_OS_STREAM_ID_MAX];
    midi_cc_stream_id(channel, controller, id, sizeof(id));
    const esp_err_t error = solar_os_stream_unregister(id);
    if (error == ESP_OK) {
        memset(&midi_cc_streams[index], 0, sizeof(midi_cc_streams[index]));
    }
    xSemaphoreGive(midi_mutex);
    return error;
}

esp_err_t solar_os_midi_cc_stream_clear(size_t *removed)
{
    if (removed != NULL) {
        *removed = 0U;
    }
    if (midi_ensure_mutex() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t first_error = ESP_OK;
    size_t count = 0U;
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_MIDI_CC_STREAM_MAX; i++) {
        solar_os_midi_cc_stream_t *slot = &midi_cc_streams[i];
        if (!slot->active) {
            continue;
        }
        char id[SOLAR_OS_STREAM_ID_MAX];
        midi_cc_stream_id(slot->channel, slot->controller, id, sizeof(id));
        const esp_err_t error = solar_os_stream_unregister(id);
        if (error == ESP_OK) {
            memset(slot, 0, sizeof(*slot));
            count++;
        } else if (first_error == ESP_OK) {
            first_error = error;
        }
    }
    xSemaphoreGive(midi_mutex);
    if (removed != NULL) {
        *removed = count;
    }
    return first_error;
}

size_t solar_os_midi_cc_stream_count(void)
{
    if (midi_ensure_mutex() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_MIDI_CC_STREAM_MAX; i++) {
        count += midi_cc_streams[i].active ? 1U : 0U;
    }
    xSemaphoreGive(midi_mutex);
    return count;
}

bool solar_os_midi_cc_stream_get(size_t index,
                                 solar_os_midi_cc_stream_info_t *info)
{
    if (info == NULL || midi_ensure_mutex() != ESP_OK) {
        return false;
    }
    size_t seen = 0U;
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_MIDI_CC_STREAM_MAX; i++) {
        const solar_os_midi_cc_stream_t *slot = &midi_cc_streams[i];
        if (!slot->active) {
            continue;
        }
        if (seen++ == index) {
            *info = (solar_os_midi_cc_stream_info_t) {
                .channel = slot->channel,
                .controller = slot->controller,
                .has_value = slot->has_value && midi_status.running,
                .value = slot->value,
                .updates = slot->updates,
            };
            midi_cc_stream_id(slot->channel, slot->controller, info->id,
                              sizeof(info->id));
            xSemaphoreGive(midi_mutex);
            return true;
        }
    }
    xSemaphoreGive(midi_mutex);
    return false;
}

esp_err_t solar_os_midi_worker_start(const char *bus_name)
{
    if (bus_name == NULL || bus_name[0] == '\0' ||
        strnlen(bus_name, SOLAR_OS_BUS_NAME_MAX) >= SOLAR_OS_BUS_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    memset(&midi_status, 0, sizeof(midi_status));
    midi_status.running = true;
    strlcpy(midi_status.bus_name, bus_name, sizeof(midi_status.bus_name));
    midi_tx_head = 0U;
    midi_tx_count = 0U;
    for (size_t i = 0; i < SOLAR_OS_MIDI_CC_STREAM_MAX; i++) {
        midi_cc_streams[i].has_value = false;
    }
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

void solar_os_midi_worker_stop(void)
{
    if (midi_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    midi_status.running = false;
    midi_tx_head = 0U;
    midi_tx_count = 0U;
    for (size_t i = 0; i < SOLAR_OS_MIDI_SUBSCRIBER_MAX; i++) {
        midi_subscribers[i].head = 0U;
        midi_subscribers[i].count = 0U;
    }
    for (size_t i = 0; i < SOLAR_OS_MIDI_CC_STREAM_MAX; i++) {
        midi_cc_streams[i].has_value = false;
    }
    xSemaphoreGive(midi_mutex);
}

void solar_os_midi_worker_publish(const solar_os_midi_message_t *message)
{
    if (!solar_os_midi_message_valid(message) || midi_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    midi_status.rx_messages++;
    if ((message->status & 0xf0U) == 0xb0U) {
        const uint8_t channel = (uint8_t)((message->status & 0x0fU) + 1U);
        const int index = midi_cc_stream_find_locked(channel, message->data1);
        if (index >= 0) {
            solar_os_midi_cc_stream_t *stream = &midi_cc_streams[index];
            stream->has_value = true;
            stream->value = message->data2;
            stream->updates++;
        }
    }
    for (size_t i = 0; i < SOLAR_OS_MIDI_SUBSCRIBER_MAX; i++) {
        solar_os_midi_subscriber_t *subscriber = &midi_subscribers[i];
        if (!subscriber->active) {
            continue;
        }
        if (subscriber->count >= SOLAR_OS_MIDI_RX_QUEUE_DEPTH) {
            subscriber->head = (subscriber->head + 1U) % SOLAR_OS_MIDI_RX_QUEUE_DEPTH;
            subscriber->count--;
            midi_status.subscriber_drops++;
        }
        const size_t tail =
            (subscriber->head + subscriber->count) % SOLAR_OS_MIDI_RX_QUEUE_DEPTH;
        subscriber->queue[tail] = *message;
        subscriber->count++;
    }
    xSemaphoreGive(midi_mutex);
}

bool solar_os_midi_worker_take_tx(solar_os_midi_message_t *message)
{
    if (message == NULL || midi_ensure_mutex() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    if (!midi_status.running || midi_tx_count == 0U) {
        xSemaphoreGive(midi_mutex);
        return false;
    }
    *message = midi_tx_queue[midi_tx_head];
    midi_tx_head = (midi_tx_head + 1U) % SOLAR_OS_MIDI_TX_QUEUE_DEPTH;
    midi_tx_count--;
    xSemaphoreGive(midi_mutex);
    return true;
}

void solar_os_midi_worker_note_rx_bytes(size_t count)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.rx_bytes += (uint32_t)count;
        xSemaphoreGive(midi_mutex);
    }
}

void solar_os_midi_worker_note_tx(size_t count)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.tx_bytes += (uint32_t)count;
        midi_status.tx_messages++;
        midi_status.last_error = ESP_OK;
        xSemaphoreGive(midi_mutex);
    }
}

void solar_os_midi_worker_note_unsupported(void)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.parser_unsupported++;
        xSemaphoreGive(midi_mutex);
    }
}

void solar_os_midi_worker_note_error(esp_err_t error)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.last_error = error;
        xSemaphoreGive(midi_mutex);
    }
}
