#include "solar_os_meshcore_ble.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_contacts.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_meshcore_ble_protocol.h"
#include "solar_os_messaging.h"
#include "solar_os_meshcore_instances.h"
#include "solar_os_time.h"

#define MESHCORE_BLE_CONNECT_TIMEOUT_MS 12000U
#define MESHCORE_BLE_OPERATION_TIMEOUT_MS 5000U
#define MESHCORE_BLE_RETRY_MIN_MS 1000U
#define MESHCORE_BLE_RETRY_MAX_MS 30000U
#define MESHCORE_BLE_QUEUE_CAPACITY 24U
#define MESHCORE_BLE_POLL_DELAY_MS 20U
#define MESHCORE_BLE_ENDPOINT_CAPABILITY \
    (SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_GROUP | SOLAR_OS_ENDPOINT_CAP_ACK)
#define MESHCORE_BLE_GROUP_REF_BASE UINT32_C(0x424c4500)

static const char *TAG = "meshcore_ble";
static const char MESHCORE_SERVICE_UUID[] = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char MESHCORE_RX_UUID[] = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char MESHCORE_TX_UUID[] = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

typedef struct {
    bool valid;
    char name[SOLAR_OS_MESHCORE_BLE_CHANNEL_NAME_MAX + 1U];
} meshcore_ble_channel_slot_t;

typedef struct {
    bool initialized;
    volatile bool cancel_requested;
    solar_os_ble_session_t session;
    solar_os_ble_peer_t peer;
    uint8_t bda[6];
    uint8_t addr_type;
    bool pin_supplied;
    uint32_t pin;
    uint16_t rx_handle;
    uint16_t tx_handle;
    uint32_t retry_delay_ms;
    uint32_t next_retry_ms;
    bool contacts_refresh;
    bool messages_waiting;
    bool ever_connected;
    uint32_t inflight_request;
    uint32_t inflight_ack;
    uint32_t inflight_deadline_ms;
    solar_os_endpoint_t *endpoints;
    meshcore_ble_channel_slot_t channels[SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY];
    solar_os_meshcore_ble_status_t status;
} meshcore_ble_service_t;

static meshcore_ble_service_t empty_service;
static meshcore_ble_service_t *current_service(void)
{
    meshcore_ble_service_t *state = solar_os_meshcore_current_state(SOLAR_OS_MESHCORE_BACKEND_BLE, sizeof(*state));
    return state ? state : &empty_service;
}
#define service (*current_service())
static void finish_legacy_call(void **lease) { solar_os_meshcore_legacy_end(*lease); }

static portMUX_TYPE service_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void)
{
    return (uint32_t)solar_os_time_uptime_ms();
}

static uint32_t epoch_seconds(void)
{
    uint64_t epoch = 0;
    if (solar_os_time_get_utc_epoch_ms(&epoch) == ESP_OK) {
        return (uint32_t)(epoch / 1000ULL);
    }
    return (uint32_t)(solar_os_time_uptime_ms() / 1000ULL);
}

static void status_update(solar_os_meshcore_ble_state_t state,
                          esp_err_t error, const char *detail)
{
    portENTER_CRITICAL(&service_lock);
    service.status.state = state;
    service.status.last_error = error;
    if (error != ESP_OK) {
        service.status.errors++;
    }
    if (detail != NULL) {
        strlcpy(service.status.detail, detail, sizeof(service.status.detail));
    }
    portEXIT_CRITICAL(&service_lock);
}

static void provider_status(bool connected, esp_err_t error, const char *detail)
{
    (void)solar_os_messaging_provider_set_status(
        solar_os_meshcore_current_id(), true, connected, error, detail);
}

static void schedule_retry(esp_err_t error, const char *detail)
{
    service.next_retry_ms = now_ms() + service.retry_delay_ms;
    if (service.retry_delay_ms < MESHCORE_BLE_RETRY_MAX_MS) {
        uint32_t next = service.retry_delay_ms * 2U;
        service.retry_delay_ms = next < MESHCORE_BLE_RETRY_MAX_MS ?
            next : MESHCORE_BLE_RETRY_MAX_MS;
    }
    status_update(SOLAR_OS_MESHCORE_BLE_BACKOFF, error, detail);
    provider_status(false, error, detail);
}

static void clear_peer(void)
{
    if (service.inflight_request) (void)solar_os_messaging_outbox_update(service.inflight_request,
        SOLAR_OS_DELIVERY_FAILED, "MeshCore BLE disconnected before acknowledgement; outcome unknown");
    if (service.peer != SOLAR_OS_BLE_PEER_INVALID) {
        (void)solar_os_ble_peer_disconnect(service.session, service.peer);
        service.peer = SOLAR_OS_BLE_PEER_INVALID;
    }
    service.rx_handle = 0U;
    service.tx_handle = 0U;
    service.inflight_request = 0U;
    service.inflight_ack = 0U;
    service.inflight_deadline_ms = 0U;
    portENTER_CRITICAL(&service_lock);
    service.status.connected = false;
    service.status.encrypted = false;
    service.status.bonded = false;
    service.status.mtu = 0U;
    portEXIT_CRITICAL(&service_lock);
}

static bool channel_provider_key(uint8_t index, char *buffer, size_t length)
{
    return snprintf(buffer, length, "ble-group:%u", (unsigned)index) > 0;
}

static esp_err_t upsert_channel(const solar_os_meshcore_ble_channel_t *channel)
{
    if (channel == NULL || channel->index >= SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    meshcore_ble_channel_slot_t *slot = &service.channels[channel->index];
    slot->valid = channel->name[0] != '\0';
    strlcpy(slot->name, channel->name, sizeof(slot->name));
    if (!slot->valid) {
        return ESP_OK;
    }
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    (void)channel_provider_key(channel->index, provider_key, sizeof(provider_key));
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = solar_os_meshcore_current_id(),
        .provider_key = provider_key,
        .kind = SOLAR_OS_CONVERSATION_GROUP,
        .title = slot->name,
        .group_ref = MESHCORE_BLE_GROUP_REF_BASE | channel->index,
        .security_flags = SOLAR_OS_SECURITY_ENCRYPTED |
            SOLAR_OS_SECURITY_SHARED_KEY |
            SOLAR_OS_SECURITY_SENDER_UNVERIFIED,
    };
    return solar_os_messaging_conversation_upsert(&request, NULL);
}

static esp_err_t upsert_contact(const solar_os_meshcore_ble_contact_t *contact)
{
    if (contact == NULL || contact->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_endpoint_t existing = {0};
    const bool known = solar_os_contacts_find_endpoint(
        solar_os_meshcore_current_id(),
        contact->public_key, sizeof(contact->public_key), &existing) == ESP_OK;
    return solar_os_contacts_import_discovered(
        solar_os_meshcore_current_id(),
        contact->public_key,
        sizeof(contact->public_key),
        contact->name,
        MESHCORE_BLE_ENDPOINT_CAPABILITY,
        (uint64_t)contact->last_modified * 1000ULL,
        known && existing.provider_metadata_len > 0U ?
            existing.provider_metadata : NULL,
        known ? existing.provider_metadata_len : 0U,
        NULL,
        NULL);
}

static bool endpoint_for_prefix(const uint8_t prefix[6], solar_os_endpoint_t *result)
{
    const size_t count = solar_os_contacts_endpoint_snapshot(
        SOLAR_OS_CONTACT_ID_NONE, service.endpoints, SOLAR_OS_ENDPOINT_CAPACITY);
    for (size_t i = 0; i < count; i++) {
        solar_os_endpoint_t *endpoint = &service.endpoints[i];
        if (endpoint->provider == solar_os_meshcore_current_id() &&
            endpoint->address.length == SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_SIZE &&
            endpoint->trust != SOLAR_OS_CONTACT_TRUST_BLOCKED &&
            memcmp(endpoint->address.bytes, prefix,
                   SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_PREFIX_SIZE) == 0) {
            *result = *endpoint;
            return true;
        }
    }
    return false;
}

static uint32_t transport_security(void)
{
    portENTER_CRITICAL(&service_lock);
    const bool encrypted = service.status.encrypted;
    portEXIT_CRITICAL(&service_lock);
    return encrypted ? SOLAR_OS_SECURITY_TRANSPORT_SECURED : 0U;
}

static esp_err_t publish_contact_message(const uint8_t *frame, size_t length)
{
    solar_os_meshcore_ble_contact_message_t message;
    if (!solar_os_meshcore_ble_parse_contact_message(frame, length, &message)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (message.text_type != 0U && message.text_type != 2U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_endpoint_t endpoint;
    if (!endpoint_for_prefix(message.public_key_prefix, &endpoint)) {
        service.contacts_refresh = true;
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_contact_t contact = {0};
    (void)solar_os_contacts_get(endpoint.contact_id, &contact);
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    snprintf(provider_key, sizeof(provider_key), "direct:%" PRIu32, endpoint.id);
    uint32_t security = SOLAR_OS_SECURITY_ENCRYPTED |
        SOLAR_OS_SECURITY_PEER_KEY_KNOWN | transport_security();
    if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_TRUSTED) {
        security |= SOLAR_OS_SECURITY_PEER_TRUSTED;
    }
    const solar_os_messaging_inbound_t inbound = {
        .provider = solar_os_meshcore_current_id(),
        .conversation_key = provider_key,
        .conversation_kind = SOLAR_OS_CONVERSATION_DIRECT,
        .conversation_title = contact.display_name,
        .contact_id = endpoint.contact_id,
        .endpoint_id = endpoint.id,
        .provider_message_key = solar_os_meshcore_ble_message_key(frame, length),
        .timestamp_ms = (uint64_t)message.timestamp * 1000ULL,
        .security_flags = security,
        .sender = contact.display_name,
        .body = message.text,
    };
    bool inserted = false;
    const esp_err_t error = solar_os_messaging_publish_inbound(
        &inbound, &inserted, NULL);
    if (error == ESP_OK && inserted) {
        portENTER_CRITICAL(&service_lock);
        service.status.received++;
        portEXIT_CRITICAL(&service_lock);
    }
    return error;
}

static esp_err_t publish_channel_message(const uint8_t *frame, size_t length)
{
    solar_os_meshcore_ble_channel_message_t message;
    if (!solar_os_meshcore_ble_parse_channel_message(frame, length, &message)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    meshcore_ble_channel_slot_t *slot = &service.channels[message.channel_index];
    if (!slot->valid) {
        return ESP_ERR_NOT_FOUND;
    }
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    (void)channel_provider_key(message.channel_index, provider_key, sizeof(provider_key));
    char sender[SOLAR_OS_MESSAGING_SENDER_MAX] = {0};
    const char *body = message.text;
    const char *separator = strstr(body, ": ");
    if (separator != NULL) {
        size_t sender_len = (size_t)(separator - body);
        if (sender_len >= sizeof(sender)) sender_len = sizeof(sender) - 1U;
        memcpy(sender, body, sender_len);
        sender[sender_len] = '\0';
        body = separator + 2U;
    }
    const solar_os_messaging_inbound_t inbound = {
        .provider = solar_os_meshcore_current_id(),
        .conversation_key = provider_key,
        .conversation_kind = SOLAR_OS_CONVERSATION_GROUP,
        .conversation_title = slot->name,
        .group_ref = MESHCORE_BLE_GROUP_REF_BASE | message.channel_index,
        .provider_message_key = solar_os_meshcore_ble_message_key(frame, length),
        .timestamp_ms = (uint64_t)message.timestamp * 1000ULL,
        .security_flags = SOLAR_OS_SECURITY_ENCRYPTED |
            SOLAR_OS_SECURITY_SHARED_KEY |
            SOLAR_OS_SECURITY_SENDER_UNVERIFIED | transport_security(),
        .sender = sender,
        .body = body,
    };
    bool inserted = false;
    const esp_err_t error = solar_os_messaging_publish_inbound(
        &inbound, &inserted, NULL);
    if (error == ESP_OK && inserted) {
        portENTER_CRITICAL(&service_lock);
        service.status.received++;
        portEXIT_CRITICAL(&service_lock);
    }
    return error;
}

static void handle_frame(const uint8_t *frame, size_t length)
{
    if (frame == NULL || length == 0U) {
        return;
    }
    switch (frame[0]) {
    case SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE:
    case SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE_V3:
        (void)publish_contact_message(frame, length);
        break;
    case SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE:
    case SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE_V3:
        (void)publish_channel_message(frame, length);
        break;
    case SOLAR_OS_MESHCORE_BLE_PUSH_ADVERT:
    case SOLAR_OS_MESHCORE_BLE_PUSH_NEW_ADVERT:
    case SOLAR_OS_MESHCORE_BLE_PUSH_PATH_UPDATED:
        service.contacts_refresh = true;
        break;
    case SOLAR_OS_MESHCORE_BLE_PUSH_MESSAGES_WAITING:
        service.messages_waiting = true;
        break;
    case SOLAR_OS_MESHCORE_BLE_PUSH_SEND_CONFIRMED:
        if (length >= 5U && service.inflight_request != 0U) {
            const uint32_t ack = (uint32_t)frame[1] |
                ((uint32_t)frame[2] << 8U) |
                ((uint32_t)frame[3] << 16U) |
                ((uint32_t)frame[4] << 24U);
            if (ack == service.inflight_ack) {
                (void)solar_os_messaging_outbox_update(
                    service.inflight_request, SOLAR_OS_DELIVERY_DELIVERED, NULL);
                service.inflight_request = 0U;
                service.inflight_ack = 0U;
                service.inflight_deadline_ms = 0U;
            }
        }
        break;
    default:
        break;
    }
}

static esp_err_t next_frame(uint8_t *frame, size_t *length, uint32_t timeout_ms)
{
    const uint32_t deadline = now_ms() + timeout_ms;
    while (!service.cancel_requested && (int32_t)(deadline - now_ms()) > 0) {
        solar_os_ble_notification_t notification;
        const esp_err_t error = solar_os_ble_peer_poll(
            service.session, service.peer, &notification);
        if (error == ESP_OK) {
            if (notification.handle != service.tx_handle || notification.value_len == 0U) {
                continue;
            }
            memcpy(frame, notification.value, notification.value_len);
            *length = notification.value_len;
            handle_frame(frame, *length);
            return ESP_OK;
        }
        if (error != ESP_ERR_NOT_FOUND) {
            return error;
        }
        vTaskDelay(pdMS_TO_TICKS(MESHCORE_BLE_POLL_DELAY_MS));
    }
    return service.cancel_requested ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
}

static bool type_matches(uint8_t type, const uint8_t *expected, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (expected[i] == type) return true;
    }
    return false;
}

static esp_err_t wait_types(const uint8_t *expected, size_t expected_count,
                            uint8_t *response, size_t *response_len)
{
    uint8_t frame[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
    const uint32_t deadline = now_ms() + MESHCORE_BLE_OPERATION_TIMEOUT_MS;
    while ((int32_t)(deadline - now_ms()) > 0) {
        size_t length = 0U;
        const uint32_t remaining = deadline - now_ms();
        esp_err_t error = next_frame(frame, &length, remaining);
        if (error != ESP_OK) return error;
        if (type_matches(frame[0], expected, expected_count)) {
            if (response != NULL) memcpy(response, frame, length);
            if (response_len != NULL) *response_len = length;
            return frame[0] == SOLAR_OS_MESHCORE_BLE_RESP_ERROR ? ESP_FAIL : ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t command(const uint8_t *request, size_t request_len,
                         const uint8_t *expected, size_t expected_count,
                         uint8_t *response, size_t *response_len)
{
    esp_err_t error = solar_os_ble_peer_write(
        service.session, service.peer, service.rx_handle,
        request, request_len, true, MESHCORE_BLE_OPERATION_TIMEOUT_MS);
    if (error != ESP_OK) return error;
    return wait_types(expected, expected_count, response, response_len);
}

static esp_err_t sync_contacts(void)
{
    uint8_t request[2];
    size_t request_len = solar_os_meshcore_ble_build_get_contacts(
        request, sizeof(request));
    static const uint8_t start_types[] = {
        SOLAR_OS_MESHCORE_BLE_RESP_CONTACTS_START,
        SOLAR_OS_MESHCORE_BLE_RESP_ERROR,
    };
    esp_err_t error = command(request, request_len, start_types,
                              sizeof(start_types), NULL, NULL);
    if (error != ESP_OK) return error;
    size_t count = 0U;
    size_t seen = 0U;
    size_t skipped = 0U;
    for (;;) {
        uint8_t frame[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
        size_t length = 0U;
        error = next_frame(frame, &length, MESHCORE_BLE_OPERATION_TIMEOUT_MS);
        if (error != ESP_OK) break;
        if (frame[0] == SOLAR_OS_MESHCORE_BLE_RESP_CONTACTS_END) break;
        if (frame[0] == SOLAR_OS_MESHCORE_BLE_RESP_ERROR) {
            error = ESP_FAIL;
            break;
        }
        if (frame[0] == SOLAR_OS_MESHCORE_BLE_RESP_CONTACT) {
            seen++;
            solar_os_meshcore_ble_contact_t contact;
            if (solar_os_meshcore_ble_parse_contact(frame, length, &contact) &&
                upsert_contact(&contact) == ESP_OK) {
                count++;
            } else skipped++;
        }
    }
    if (error == ESP_OK) service.contacts_refresh = false;
    if (count > 0U) (void)solar_os_contacts_flush();
    portENTER_CRITICAL(&service_lock);
    service.status.contacts = count;
    service.status.contacts_seen = seen;
    service.status.contacts_skipped = skipped;
    portEXIT_CRITICAL(&service_lock);
    return error;
}

static esp_err_t sync_channels(void)
{
    memset(service.channels, 0, sizeof(service.channels));
    (void)solar_os_messaging_groups_begin_sync(solar_os_meshcore_current_id(), "ble-group:");
    size_t count = 0U;
    for (uint8_t index = 0; index < SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY; index++) {
        uint8_t request[2];
        const size_t request_len = solar_os_meshcore_ble_build_get_channel(
            request, sizeof(request), index);
        uint8_t response[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
        size_t response_len = 0U;
        static const uint8_t response_types[] = {
            SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_INFO,
            SOLAR_OS_MESHCORE_BLE_RESP_ERROR,
        };
        esp_err_t error = command(request, request_len, response_types,
                                  sizeof(response_types), response, &response_len);
        if (error != ESP_OK) {
            continue;
        }
        solar_os_meshcore_ble_channel_t channel;
        if (solar_os_meshcore_ble_parse_channel(response, response_len, &channel) &&
            upsert_channel(&channel) == ESP_OK && channel.name[0] != '\0') {
            count++;
        }
    }
    portENTER_CRITICAL(&service_lock);
    service.status.channels = count;
    portEXIT_CRITICAL(&service_lock);
    return ESP_OK;
}

static esp_err_t sync_messages(void)
{
    for (size_t i = 0; i < SOLAR_OS_MESSAGING_MESSAGE_CAPACITY; i++) {
        uint8_t request[1];
        const size_t request_len = solar_os_meshcore_ble_build_sync_next(
            request, sizeof(request));
        static const uint8_t response_types[] = {
            SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE,
            SOLAR_OS_MESHCORE_BLE_RESP_CONTACT_MESSAGE_V3,
            SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE,
            SOLAR_OS_MESHCORE_BLE_RESP_CHANNEL_MESSAGE_V3,
            SOLAR_OS_MESHCORE_BLE_RESP_NO_MORE_MESSAGES,
            SOLAR_OS_MESHCORE_BLE_RESP_ERROR,
        };
        uint8_t response[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
        size_t response_len = 0U;
        esp_err_t error = command(request, request_len, response_types,
                                  sizeof(response_types), response, &response_len);
        if (error != ESP_OK) return error;
        if (response[0] == SOLAR_OS_MESHCORE_BLE_RESP_NO_MORE_MESSAGES) {
            service.messages_waiting = false;
            return ESP_OK;
        }
    }
    service.messages_waiting = true;
    return ESP_OK;
}

static esp_err_t discover_handles(void)
{
    solar_os_ble_gatt_service_t *services = solar_os_memory_calloc(
        SOLAR_OS_BLE_GATT_MAX_SERVICES, sizeof(*services),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "meshcore.ble.services");
    if (services == NULL) return ESP_ERR_NO_MEM;
    size_t service_count = 0U;
    esp_err_t error = solar_os_ble_peer_services(
        service.session, service.peer, services,
        SOLAR_OS_BLE_GATT_MAX_SERVICES, &service_count);
    if (error != ESP_OK) {
        solar_os_memory_free(services);
        return error;
    }
    size_t meshcore_service = SIZE_MAX;
    for (size_t i = 0; i < service_count; i++) {
        if (strcmp(services[i].uuid, MESHCORE_SERVICE_UUID) == 0) {
            meshcore_service = i;
            break;
        }
    }
    solar_os_memory_free(services);
    if (meshcore_service == SIZE_MAX) return ESP_ERR_NOT_FOUND;
    solar_os_ble_gatt_characteristic_t *chars = solar_os_memory_calloc(
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, sizeof(*chars),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "meshcore.ble.characteristics");
    if (chars == NULL) return ESP_ERR_NO_MEM;
    size_t char_count = 0U;
    error = solar_os_ble_peer_characteristics(
        service.session, service.peer, meshcore_service, chars,
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, &char_count);
    if (error == ESP_OK) {
        for (size_t i = 0; i < char_count; i++) {
            if (strcmp(chars[i].uuid, MESHCORE_RX_UUID) == 0) {
                service.rx_handle = chars[i].handle;
            }
            if (strcmp(chars[i].uuid, MESHCORE_TX_UUID) == 0) {
                service.tx_handle = chars[i].handle;
            }
        }
    }
    solar_os_memory_free(chars);
    if (error != ESP_OK) return error;
    return service.rx_handle != 0U && service.tx_handle != 0U ?
        ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t handshake(void)
{
    uint8_t request[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
    uint8_t response[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
    size_t response_len = 0U;
    size_t request_len = solar_os_meshcore_ble_build_app_start(
        request, sizeof(request), "SolarOS");
    static const uint8_t self_types[] = {
        SOLAR_OS_MESHCORE_BLE_RESP_SELF_INFO,
        SOLAR_OS_MESHCORE_BLE_RESP_ERROR,
    };
    esp_err_t error = command(request, request_len, self_types,
                              sizeof(self_types), response, &response_len);
    if (error != ESP_OK) return error;
    if (response_len > 58U) {
        size_t name_len = response_len - 58U;
        if (name_len >= sizeof(service.status.device)) name_len = sizeof(service.status.device) - 1U;
        portENTER_CRITICAL(&service_lock);
        memcpy(service.status.device, response + 58U, name_len);
        service.status.device[name_len] = '\0';
        portEXIT_CRITICAL(&service_lock);
    }
    request_len = solar_os_meshcore_ble_build_device_query(request, sizeof(request));
    static const uint8_t device_types[] = {
        SOLAR_OS_MESHCORE_BLE_RESP_DEVICE_INFO,
        SOLAR_OS_MESHCORE_BLE_RESP_ERROR,
    };
    error = command(request, request_len, device_types,
                    sizeof(device_types), response, &response_len);
    if (error != ESP_OK) return error;
    solar_os_meshcore_ble_device_info_t device;
    if (!solar_os_meshcore_ble_parse_device_info(
            response, response_len, &device)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    portENTER_CRITICAL(&service_lock);
    service.status.protocol_version = SOLAR_OS_MESHCORE_BLE_PROTOCOL_VERSION;
    service.status.firmware_code = device.firmware_code;
    service.status.companion_contact_capacity = device.max_contacts;
    strlcpy(service.status.model, device.model, sizeof(service.status.model));
    strlcpy(service.status.version, device.version, sizeof(service.status.version));
    strlcpy(service.status.build, device.build, sizeof(service.status.build));
    portEXIT_CRITICAL(&service_lock);
    request_len = solar_os_meshcore_ble_build_set_time(
        request, sizeof(request), epoch_seconds());
    static const uint8_t ok_types[] = {
        SOLAR_OS_MESHCORE_BLE_RESP_OK,
        SOLAR_OS_MESHCORE_BLE_RESP_ERROR,
    };
    error = command(request, request_len, ok_types, sizeof(ok_types), NULL, NULL);
    if (error != ESP_OK) return error;
    error = sync_contacts();
    if (error == ESP_OK) error = sync_channels();
    if (error == ESP_OK) error = sync_messages();
    return error;
}

static esp_err_t connect_peer(void)
{
    status_update(SOLAR_OS_MESHCORE_BLE_CONNECTING, ESP_OK, "connecting");
    esp_err_t error = solar_os_ble_peer_connect(
        service.session, service.bda, service.addr_type,
        MESHCORE_BLE_CONNECT_TIMEOUT_MS, &service.peer);
    if (error != ESP_OK) return error;
    if (service.pin_supplied) {
        status_update(SOLAR_OS_MESHCORE_BLE_PAIRING, ESP_OK, "pairing");
        error = solar_os_ble_peer_pair(
            service.session, service.peer, service.pin,
            MESHCORE_BLE_CONNECT_TIMEOUT_MS);
        if (error != ESP_OK) return error;
    }
    error = discover_handles();
    if (error == ESP_OK) {
        error = solar_os_ble_peer_configure_queue(
            service.session, service.peer, MESHCORE_BLE_QUEUE_CAPACITY);
    }
    if (error == ESP_OK) {
        error = solar_os_ble_peer_subscribe(
            service.session, service.peer, service.tx_handle, 1U,
            MESHCORE_BLE_OPERATION_TIMEOUT_MS);
    }
    if (error != ESP_OK) return error;
    status_update(SOLAR_OS_MESHCORE_BLE_SYNCING, ESP_OK, "synchronizing");
    error = handshake();
    if (error != ESP_OK) return error;
    solar_os_ble_session_info_t info;
    error = solar_os_ble_peer_get_info(service.session, service.peer, &info);
    if (error != ESP_OK || !info.gatt.connected) return ESP_ERR_INVALID_STATE;
    service.retry_delay_ms = MESHCORE_BLE_RETRY_MIN_MS;
    portENTER_CRITICAL(&service_lock);
    service.status.connected = true;
    service.status.encrypted = info.gatt.encrypted;
    service.status.bonded = info.gatt.bonded;
    service.status.mtu = info.gatt.mtu;
    service.status.state = SOLAR_OS_MESHCORE_BLE_ONLINE;
    service.status.last_error = ESP_OK;
    strlcpy(service.status.detail, "online", sizeof(service.status.detail));
    portEXIT_CRITICAL(&service_lock);
    provider_status(true, ESP_OK, "BLE companion online");
    for (size_t i = 0; i < SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY; i++) {
        if (!service.channels[i].valid) continue;
        char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX]; (void)channel_provider_key(i, key, sizeof(key));
        (void)solar_os_messaging_join_update(solar_os_meshcore_current_id(), key, SOLAR_OS_MESSAGING_JOIN_JOINED, NULL);
    }
    return ESP_OK;
}

static esp_err_t process_outbound(void)
{
    if (service.inflight_request != 0U) {
        if ((int32_t)(now_ms() - service.inflight_deadline_ms) >= 0) {
            (void)solar_os_messaging_outbox_update(
                service.inflight_request, SOLAR_OS_DELIVERY_FAILED,
                "MeshCore acknowledgement timeout");
            service.inflight_request = 0U;
            service.inflight_ack = 0U;
            service.inflight_deadline_ms = 0U;
        }
        return ESP_OK;
    }
    solar_os_messaging_outbound_t outbound;
    esp_err_t error = solar_os_messaging_outbox_peek(
        solar_os_meshcore_current_id(), &outbound);
    if (error == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    solar_os_messaging_conversation_t conversation;
    error = solar_os_messaging_conversation_get(outbound.conversation_id, &conversation);
    if (error != ESP_OK) return error;
    uint8_t request[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
    size_t request_len = 0U;
    bool direct = false;
    if (conversation.kind == SOLAR_OS_CONVERSATION_DIRECT) {
        solar_os_endpoint_t endpoint;
        error = solar_os_contacts_get_endpoint(conversation.endpoint_id, &endpoint);
        if (error == ESP_OK &&
            (endpoint.provider != solar_os_meshcore_current_id() ||
             endpoint.address.length != SOLAR_OS_MESHCORE_BLE_PUBLIC_KEY_SIZE ||
             endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED)) {
            error = ESP_ERR_INVALID_STATE;
        }
        if (error == ESP_OK) {
            request_len = solar_os_meshcore_ble_build_send_direct(
                request, sizeof(request), endpoint.address.bytes,
                epoch_seconds(), outbound.body);
            direct = true;
        }
    } else if (conversation.kind == SOLAR_OS_CONVERSATION_GROUP &&
               strncmp(conversation.provider_key, "ble-group:", 10U) == 0) {
        unsigned index = SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY;
        if (sscanf(conversation.provider_key + 10U, "%u", &index) == 1 &&
            index < SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY) {
            request_len = solar_os_meshcore_ble_build_send_channel(
                request, sizeof(request), (uint8_t)index,
                epoch_seconds(), outbound.body);
        }
    } else {
        error = ESP_ERR_NOT_SUPPORTED;
    }
    if (error != ESP_OK || request_len == 0U) {
        (void)solar_os_messaging_outbox_update(
            outbound.id, SOLAR_OS_DELIVERY_FAILED,
            error == ESP_ERR_NOT_SUPPORTED ? "not a BLE companion conversation" :
            "MeshCore BLE message is invalid or too long");
        return ESP_OK;
    }
    (void)solar_os_messaging_outbox_update(
        outbound.id, SOLAR_OS_DELIVERY_SENDING, NULL);
    static const uint8_t expected[] = {
        SOLAR_OS_MESHCORE_BLE_RESP_OK,
        SOLAR_OS_MESHCORE_BLE_RESP_SENT,
        SOLAR_OS_MESHCORE_BLE_RESP_ERROR,
    };
    uint8_t response[SOLAR_OS_MESHCORE_BLE_FRAME_MAX];
    size_t response_len = 0U;
    error = command(request, request_len, expected, sizeof(expected),
                    response, &response_len);
    if (error != ESP_OK) {
        (void)solar_os_messaging_outbox_update(
            outbound.id, SOLAR_OS_DELIVERY_FAILED, "MeshCore BLE send failed");
        return error;
    }
    portENTER_CRITICAL(&service_lock);
    service.status.transmitted++;
    portEXIT_CRITICAL(&service_lock);
    if (direct && response[0] == SOLAR_OS_MESHCORE_BLE_RESP_SENT && response_len >= 10U) {
        service.inflight_request = outbound.id;
        service.inflight_ack = (uint32_t)response[2] |
            ((uint32_t)response[3] << 8U) |
            ((uint32_t)response[4] << 16U) |
            ((uint32_t)response[5] << 24U);
        const uint32_t timeout = (uint32_t)response[6] |
            ((uint32_t)response[7] << 8U) |
            ((uint32_t)response[8] << 16U) |
            ((uint32_t)response[9] << 24U);
        service.inflight_deadline_ms = now_ms() + timeout + 1000U;
    } else {
        (void)solar_os_messaging_outbox_update(
            outbound.id, SOLAR_OS_DELIVERY_SENT, NULL);
    }
    return ESP_OK;
}

esp_err_t solar_os_meshcore_ble_start(const uint8_t bda[6], uint8_t addr_type,
                                      bool pin_supplied, uint32_t pin,
                                      const char *owner)
{
    void *lease __attribute__((cleanup(finish_legacy_call))) = NULL;
    esp_err_t context_error = solar_os_meshcore_legacy_begin(SOLAR_OS_MESHCORE_BACKEND_BLE, &lease);
    if (context_error != ESP_OK) return context_error;
    if (!solar_os_meshcore_current_state(SOLAR_OS_MESHCORE_BACKEND_BLE, sizeof(meshcore_ble_service_t))) return ESP_ERR_NO_MEM;
    if (bda == NULL || owner == NULL || owner[0] == '\0' ||
        addr_type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY ||
        (pin_supplied && pin > 999999U)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool provider_claimed = false;
    esp_err_t error = solar_os_contacts_init();
    if (error == ESP_OK) error = solar_os_messaging_init();
    if (error == ESP_OK) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = solar_os_messaging_provider_claim(
            solar_os_meshcore_current_id(), "BLE companion starting");
        provider_claimed = error == ESP_OK;
    }
    if (error == ESP_OK) error = solar_os_ble_init();
    if (error != ESP_OK) {
        if (provider_claimed) {
            (void)solar_os_messaging_provider_set_status(
                solar_os_meshcore_current_id(),
                false, false, ESP_OK, "start failed");
        }
        return error;
    }
    memset(&service, 0, sizeof(service));
    service.endpoints = solar_os_memory_calloc(
        SOLAR_OS_ENDPOINT_CAPACITY, sizeof(*service.endpoints),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "meshcore.ble.contacts");
    if (service.endpoints == NULL) {
        (void)solar_os_messaging_provider_set_status(
            solar_os_meshcore_current_id(),
            false, false, ESP_OK, "start failed");
        return ESP_ERR_NO_MEM;
    }
    error = solar_os_ble_session_create(owner, &service.session);
    if (error != ESP_OK) {
        solar_os_memory_free(service.endpoints);
        service.endpoints = NULL;
        (void)solar_os_messaging_provider_set_status(
            solar_os_meshcore_current_id(),
            false, false, ESP_OK, "start failed");
        return error;
    }
    service.initialized = true;
    service.peer = SOLAR_OS_BLE_PEER_INVALID;
    memcpy(service.bda, bda, sizeof(service.bda));
    service.addr_type = addr_type;
    service.pin_supplied = pin_supplied;
    service.pin = pin;
    service.retry_delay_ms = MESHCORE_BLE_RETRY_MIN_MS;
    service.contacts_refresh = true;
    service.messages_waiting = true;
    service.status.running = true;
    service.status.pin_supplied = pin_supplied;
    service.status.addr_type = addr_type;
    service.status.last_error = ESP_OK;
    service.status.state = SOLAR_OS_MESHCORE_BLE_CONNECTING;
    memcpy(service.status.bda, bda, sizeof(service.status.bda));
    strlcpy(service.status.detail, "starting", sizeof(service.status.detail));
    provider_status(false, ESP_OK, "BLE companion starting");
    return ESP_OK;
}

void solar_os_meshcore_ble_loop_once(void)
{
    void *lease __attribute__((cleanup(finish_legacy_call))) = NULL;
    esp_err_t context_error = solar_os_meshcore_legacy_begin(SOLAR_OS_MESHCORE_BACKEND_BLE, &lease);
    if (context_error != ESP_OK) return;
    if (!service.initialized || service.cancel_requested) return;
    if (service.peer == SOLAR_OS_BLE_PEER_INVALID) {
        if ((int32_t)(now_ms() - service.next_retry_ms) < 0) return;
        const esp_err_t error = connect_peer();
        if (error != ESP_OK) {
            clear_peer();
            schedule_retry(error,
                error == ESP_ERR_INVALID_STATE ? "BLE busy; retrying" : "connect or sync failed");
        } else {
            portENTER_CRITICAL(&service_lock);
            if (service.ever_connected) {
                service.status.reconnects++;
            } else {
                service.ever_connected = true;
            }
            portEXIT_CRITICAL(&service_lock);
        }
        return;
    }
    solar_os_ble_session_info_t info;
    esp_err_t error = solar_os_ble_peer_get_info(service.session, service.peer, &info);
    if (error != ESP_OK || !info.gatt.connected) {
        clear_peer();
        schedule_retry(error == ESP_OK ? ESP_ERR_INVALID_STATE : error, "disconnected; retrying");
        return;
    }
    for (size_t i = 0; i < 8U; i++) {
        solar_os_ble_notification_t notification;
        error = solar_os_ble_peer_poll(service.session, service.peer, &notification);
        if (error != ESP_OK) break;
        if (notification.handle == service.tx_handle && notification.value_len > 0U) {
            handle_frame(notification.value, notification.value_len);
        }
    }
    if (service.contacts_refresh) {
        error = sync_contacts();
    } else if (service.messages_waiting) {
        error = sync_messages();
    } else {
        error = process_outbound();
    }
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND && error != ESP_ERR_NOT_SUPPORTED) {
        status_update(SOLAR_OS_MESHCORE_BLE_ONLINE, error, "online with protocol error");
        provider_status(true, error, "BLE companion protocol error");
    }
}

void solar_os_meshcore_ble_cancel(void)
{
    void *lease __attribute__((cleanup(finish_legacy_call))) = NULL;
    esp_err_t context_error = solar_os_meshcore_legacy_begin(SOLAR_OS_MESHCORE_BACKEND_BLE, &lease);
    if (context_error != ESP_OK) return;
    service.cancel_requested = true;
    if (service.session != SOLAR_OS_BLE_SESSION_INVALID) {
        (void)solar_os_ble_session_cancel(service.session);
    }
}

void solar_os_meshcore_ble_stop(void)
{
    void *lease __attribute__((cleanup(finish_legacy_call))) = NULL;
    esp_err_t context_error = solar_os_meshcore_legacy_begin(SOLAR_OS_MESHCORE_BACKEND_BLE, &lease);
    if (context_error != ESP_OK) return;
    if (!service.initialized) return;
    if (service.inflight_request) (void)solar_os_messaging_outbox_update(service.inflight_request,
        SOLAR_OS_DELIVERY_FAILED, "MeshCore BLE stopped before acknowledgement; outcome unknown");
    service.inflight_request = 0;
    service.cancel_requested = true;
    if (service.session != SOLAR_OS_BLE_SESSION_INVALID) {
        (void)solar_os_ble_session_close(service.session);
    }
    service.session = SOLAR_OS_BLE_SESSION_INVALID;
    service.peer = SOLAR_OS_BLE_PEER_INVALID;
    solar_os_memory_free(service.endpoints);
    service.endpoints = NULL;
    service.pin = 0U;
    service.pin_supplied = false;
    (void)solar_os_messaging_provider_set_status(
        solar_os_meshcore_current_id(), false, false, ESP_OK, "stopped");
    portENTER_CRITICAL(&service_lock);
    service.status.running = false;
    service.status.connected = false;
    service.status.encrypted = false;
    service.status.bonded = false;
    service.status.state = SOLAR_OS_MESHCORE_BLE_STOPPED;
    service.status.last_error = ESP_OK;
    strlcpy(service.status.detail, "stopped", sizeof(service.status.detail));
    service.initialized = false;
    portEXIT_CRITICAL(&service_lock);
}

esp_err_t solar_os_meshcore_ble_get_status(solar_os_meshcore_ble_status_t *status)
{
    void *lease __attribute__((cleanup(finish_legacy_call))) = NULL;
    esp_err_t context_error = solar_os_meshcore_legacy_begin(SOLAR_OS_MESHCORE_BACKEND_BLE, &lease);
    if (context_error != ESP_OK) return context_error;
    if (!solar_os_meshcore_current_state(SOLAR_OS_MESHCORE_BACKEND_BLE, sizeof(meshcore_ble_service_t))) return ESP_ERR_NO_MEM;
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&service_lock);
    *status = service.status;
    portEXIT_CRITICAL(&service_lock);
    return ESP_OK;
}

void solar_os_meshcore_ble_note_stack_watermark(uint32_t bytes)
{
    void *lease __attribute__((cleanup(finish_legacy_call))) = NULL;
    esp_err_t context_error = solar_os_meshcore_legacy_begin(SOLAR_OS_MESHCORE_BACKEND_BLE, &lease);
    if (context_error != ESP_OK) return;
    portENTER_CRITICAL(&service_lock);
    if (service.status.stack_watermark_bytes == 0U ||
        bytes < service.status.stack_watermark_bytes) {
        service.status.stack_watermark_bytes = bytes;
    }
    portEXIT_CRITICAL(&service_lock);
}

const char *solar_os_meshcore_ble_state_name(solar_os_meshcore_ble_state_t state)
{
    switch (state) {
    case SOLAR_OS_MESHCORE_BLE_STOPPED: return "stopped";
    case SOLAR_OS_MESHCORE_BLE_CONNECTING: return "connecting";
    case SOLAR_OS_MESHCORE_BLE_PAIRING: return "pairing";
    case SOLAR_OS_MESHCORE_BLE_SYNCING: return "syncing";
    case SOLAR_OS_MESHCORE_BLE_ONLINE: return "online";
    case SOLAR_OS_MESHCORE_BLE_BACKOFF: return "backoff";
    default: return "unknown";
    }
}

static esp_err_t ble_instance_start(const solar_os_meshcore_instance_config_t *config, const char *owner)
{ return solar_os_meshcore_ble_start(config->bda, config->addr_type, config->pin_supplied, config->pin, owner); }
static void ble_instance_poll(void)
{
    solar_os_meshcore_ble_loop_once();
    solar_os_meshcore_ble_note_stack_watermark((uint32_t)uxTaskGetStackHighWaterMark(NULL));
}
static void ble_instance_destroy(void)
{
    solar_os_meshcore_ble_stop();
    meshcore_ble_service_t *state = solar_os_meshcore_current_state(SOLAR_OS_MESHCORE_BACKEND_BLE, 0);
    if (state) {
        volatile unsigned char *bytes = (volatile unsigned char *)state;
        for (size_t i = 0; i < sizeof(*state); i++) bytes[i] = 0;
        solar_os_meshcore_current_state_free(SOLAR_OS_MESHCORE_BACKEND_BLE);
    }
}
static esp_err_t ble_instance_join(const char *target, solar_os_conversation_id_t *conversation)
{
    for (size_t i = 0; i < SOLAR_OS_MESHCORE_BLE_CHANNEL_CAPACITY; i++) {
        if (!service.channels[i].valid) continue;
        char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX]; (void)channel_provider_key(i, key, sizeof(key));
        if (strcmp(target, key) && strcmp(target, service.channels[i].name)) continue;
        solar_os_messaging_conversation_upsert_t request = {.provider = solar_os_meshcore_current_id(),
            .provider_key = key, .kind = SOLAR_OS_CONVERSATION_GROUP, .title = service.channels[i].name,
            .group_ref = MESHCORE_BLE_GROUP_REF_BASE | i,
            .security_flags = SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_SHARED_KEY | SOLAR_OS_SECURITY_SENDER_UNVERIFIED};
        esp_err_t error = solar_os_messaging_conversation_upsert(&request, conversation);
        if (error == ESP_OK) (void)solar_os_messaging_join_update(request.provider, key,
            service.status.connected ? SOLAR_OS_MESSAGING_JOIN_JOINED : SOLAR_OS_MESSAGING_JOIN_PENDING, NULL);
        return error;
    }
    return ESP_ERR_NOT_FOUND;
}
void solar_os_meshcore_ble_install_backend(void)
{
    static const solar_os_meshcore_backend_t ops = {
        .start = ble_instance_start, .poll = ble_instance_poll, .cancel = solar_os_meshcore_ble_cancel,
        .stop = solar_os_meshcore_ble_stop, .destroy = ble_instance_destroy, .join = ble_instance_join,
    };
    solar_os_meshcore_backend_register(SOLAR_OS_MESHCORE_BACKEND_BLE, &ops);
}
