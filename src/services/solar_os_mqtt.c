#include "solar_os_mqtt.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"

#define MQTT_NVS_NAMESPACE "mqtt"
#define MQTT_NVS_URL_KEY "url"
#define MQTT_NVS_USERNAME_KEY "user"
#define MQTT_NVS_PASSWORD_KEY "password"
#define MQTT_MESSAGE_QUEUE_LEN 8
#define MQTT_CLIENT_TASK_STACK 6144
#define MQTT_BUFFER_SIZE 1024
#define MQTT_NETWORK_TIMEOUT_MS 10000
#define MQTT_RECONNECT_TIMEOUT_MS 5000

typedef struct {
    bool initialized;
    bool configured;
    bool running;
    bool connected;
    char url[SOLAR_OS_MQTT_URL_MAX];
    char username[SOLAR_OS_MQTT_USERNAME_MAX];
    char password[SOLAR_OS_MQTT_PASSWORD_MAX];
    char client_id[SOLAR_OS_MQTT_CLIENT_ID_MAX];
    char last_error[SOLAR_OS_MQTT_ERROR_MAX];
    esp_err_t last_esp_error;
    int last_msg_id;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    esp_mqtt_client_handle_t client;
    QueueHandle_t messages;
    SemaphoreHandle_t lock;
} solar_os_mqtt_state_t;

static solar_os_mqtt_state_t mqtt_state;
static const char *TAG = "solar_os_mqtt";

static void mqtt_lock(void)
{
    if (mqtt_state.lock != NULL) {
        (void)xSemaphoreTake(mqtt_state.lock, portMAX_DELAY);
    }
}

static void mqtt_unlock(void)
{
    if (mqtt_state.lock != NULL) {
        xSemaphoreGive(mqtt_state.lock);
    }
}

static bool mqtt_url_is_valid(const char *url)
{
    if (url == NULL || url[0] == '\0' || strlen(url) >= SOLAR_OS_MQTT_URL_MAX) {
        return false;
    }
    if (strncmp(url, "mqtt://", 7) != 0 && strncmp(url, "mqtts://", 8) != 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p != '\0'; p++) {
        if (!isprint(*p) || isspace(*p)) {
            return false;
        }
    }
    return true;
}

static bool mqtt_string_is_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (!isprint(*p)) {
            return false;
        }
    }
    return true;
}

static bool mqtt_topic_is_valid(const char *topic)
{
    if (!mqtt_string_is_valid(topic, SOLAR_OS_MQTT_TOPIC_MAX, false)) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)topic; *p != '\0'; p++) {
        if (*p == ' ') {
            return false;
        }
    }
    return true;
}

static void mqtt_set_error_locked(esp_err_t err, const char *message)
{
    mqtt_state.last_esp_error = err;
    strlcpy(mqtt_state.last_error, message != NULL ? message : esp_err_to_name(err),
            sizeof(mqtt_state.last_error));
}

static void mqtt_clear_error_locked(void)
{
    mqtt_state.last_esp_error = ESP_OK;
    mqtt_state.last_error[0] = '\0';
}

static esp_err_t mqtt_save_config_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MQTT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, MQTT_NVS_URL_KEY, mqtt_state.url);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, MQTT_NVS_USERNAME_KEY, mqtt_state.username);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, MQTT_NVS_PASSWORD_KEY, mqtt_state.password);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void mqtt_load_config_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MQTT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return;
    }

    char url[SOLAR_OS_MQTT_URL_MAX] = {0};
    char username[SOLAR_OS_MQTT_USERNAME_MAX] = {0};
    char password[SOLAR_OS_MQTT_PASSWORD_MAX] = {0};
    size_t len = sizeof(url);
    ret = nvs_get_str(nvs, MQTT_NVS_URL_KEY, url, &len);
    if (ret == ESP_OK && mqtt_url_is_valid(url)) {
        len = sizeof(username);
        esp_err_t user_ret = nvs_get_str(nvs, MQTT_NVS_USERNAME_KEY, username, &len);
        if (user_ret == ESP_ERR_NVS_NOT_FOUND) {
            username[0] = '\0';
            user_ret = ESP_OK;
        }
        len = sizeof(password);
        esp_err_t pass_ret = nvs_get_str(nvs, MQTT_NVS_PASSWORD_KEY, password, &len);
        if (pass_ret == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
            pass_ret = ESP_OK;
        }
        if (user_ret == ESP_OK && pass_ret == ESP_OK) {
            strlcpy(mqtt_state.url, url, sizeof(mqtt_state.url));
            strlcpy(mqtt_state.username, username, sizeof(mqtt_state.username));
            strlcpy(mqtt_state.password, password, sizeof(mqtt_state.password));
            mqtt_state.configured = true;
        }
    }
    nvs_close(nvs);
}

static void mqtt_default_client_id(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }
    snprintf(buffer, len, "solaros-%08" PRIx32, esp_random());
}

static bool mqtt_url_is_tls(const char *url)
{
    return url != NULL && strncmp(url, "mqtts://", 8) == 0;
}

static void mqtt_queue_message(const esp_mqtt_event_t *event)
{
    if (mqtt_state.messages == NULL || event == NULL || event->topic == NULL) {
        return;
    }

    solar_os_mqtt_message_t message = {
        .qos = event->qos,
        .retain = event->retain,
    };

    const size_t topic_len = event->topic_len > 0 ? (size_t)event->topic_len : 0;
    const size_t topic_copy = topic_len >= sizeof(message.topic) ?
        sizeof(message.topic) - 1 :
        topic_len;
    if (topic_copy > 0) {
        memcpy(message.topic, event->topic, topic_copy);
        message.topic[topic_copy] = '\0';
    }
    if (topic_len >= sizeof(message.topic)) {
        message.truncated = true;
    }

    const size_t data_len = event->data_len > 0 ? (size_t)event->data_len : 0;
    const size_t data_copy = data_len >= sizeof(message.payload) ?
        sizeof(message.payload) - 1 :
        data_len;
    if (data_copy > 0 && event->data != NULL) {
        memcpy(message.payload, event->data, data_copy);
    }
    message.payload[data_copy] = '\0';
    message.payload_len = data_copy;
    if (event->total_data_len > (int)data_copy ||
        event->current_data_offset != 0 ||
        data_len >= sizeof(message.payload)) {
        message.truncated = true;
    }

    if (xQueueSend(mqtt_state.messages, &message, 0) != pdPASS) {
        mqtt_lock();
        mqtt_state.dropped_count++;
        mqtt_unlock();
        return;
    }

    mqtt_lock();
    mqtt_state.rx_count++;
    mqtt_unlock();
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_t *event = (esp_mqtt_event_t *)event_data;

    mqtt_lock();
    mqtt_state.last_msg_id = event != NULL ? event->msg_id : 0;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_state.connected = true;
        mqtt_clear_error_locked();
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_state.connected = false;
        break;
    case MQTT_EVENT_ERROR:
        mqtt_state.connected = false;
        if (event != NULL && event->error_handle != NULL) {
            mqtt_state.last_esp_error = event->error_handle->esp_tls_last_esp_err;
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                snprintf(mqtt_state.last_error,
                         sizeof(mqtt_state.last_error),
                         "refused %d",
                         (int)event->error_handle->connect_return_code);
            } else {
                snprintf(mqtt_state.last_error,
                         sizeof(mqtt_state.last_error),
                         "transport errno %d",
                         event->error_handle->esp_transport_sock_errno);
            }
        } else {
            mqtt_set_error_locked(ESP_FAIL, "mqtt error");
        }
        break;
    case MQTT_EVENT_PUBLISHED:
        mqtt_state.tx_count++;
        break;
    default:
        break;
    }
    mqtt_unlock();

    if (event_id == MQTT_EVENT_DATA) {
        mqtt_queue_message(event);
    }
}

static esp_err_t mqtt_stop_client(void)
{
    esp_err_t ret = ESP_OK;

    mqtt_lock();
    esp_mqtt_client_handle_t client = mqtt_state.client;
    mqtt_state.client = NULL;
    mqtt_state.running = false;
    mqtt_state.connected = false;
    mqtt_unlock();

    if (client != NULL) {
        ret = esp_mqtt_client_stop(client);
        const esp_err_t destroy_ret = esp_mqtt_client_destroy(client);
        if (ret != ESP_OK || destroy_ret != ESP_OK) {
            if (destroy_ret != ESP_OK) {
                ret = destroy_ret;
            }
            mqtt_lock();
            mqtt_set_error_locked(ret, esp_err_to_name(ret));
            mqtt_unlock();
        }
    }
    return ret;
}

esp_err_t solar_os_mqtt_init(void)
{
    if (mqtt_state.initialized) {
        return ESP_OK;
    }

    mqtt_state.lock = xSemaphoreCreateMutex();
    if (mqtt_state.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    mqtt_state.messages = solar_os_queue_create(MQTT_MESSAGE_QUEUE_LEN,
                                                 sizeof(solar_os_mqtt_message_t));
    if (mqtt_state.messages == NULL) {
        vSemaphoreDelete(mqtt_state.lock);
        mqtt_state.lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    mqtt_state.initialized = true;
    mqtt_default_client_id(mqtt_state.client_id, sizeof(mqtt_state.client_id));
    mqtt_lock();
    mqtt_load_config_locked();
    mqtt_unlock();
    return ESP_OK;
}

esp_err_t solar_os_mqtt_connect(const char *url, const char *username, const char *password)
{
    esp_err_t ret = solar_os_mqtt_init();
    if (ret != ESP_OK) {
        return ret;
    }

    mqtt_lock();
    if (url != NULL && url[0] != '\0') {
        if (!mqtt_url_is_valid(url)) {
            mqtt_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(mqtt_state.url, url, sizeof(mqtt_state.url));
        mqtt_state.configured = true;
    }
    if (username != NULL) {
        if (!mqtt_string_is_valid(username, SOLAR_OS_MQTT_USERNAME_MAX, true)) {
            mqtt_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(mqtt_state.username, username, sizeof(mqtt_state.username));
    }
    if (password != NULL) {
        if (!mqtt_string_is_valid(password, SOLAR_OS_MQTT_PASSWORD_MAX, true)) {
            mqtt_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(mqtt_state.password, password, sizeof(mqtt_state.password));
    }
    if (!mqtt_state.configured || !mqtt_url_is_valid(mqtt_state.url)) {
        mqtt_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    ret = mqtt_save_config_locked();
    if (ret != ESP_OK) {
        mqtt_set_error_locked(ret, esp_err_to_name(ret));
        mqtt_unlock();
        return ret;
    }

    char log_url[SOLAR_OS_MQTT_URL_MAX];
    strlcpy(log_url, mqtt_state.url, sizeof(log_url));
    mqtt_unlock();

    ret = mqtt_stop_client();
    if (ret != ESP_OK) {
        return ret;
    }

    const bool tls = mqtt_url_is_tls(log_url);
    esp_mqtt_client_config_t config = {
        .broker = {
            .address = {
                .uri = mqtt_state.url,
            },
            .verification = {
                .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
            },
        },
        .credentials = {
            .username = mqtt_state.username[0] != '\0' ? mqtt_state.username : NULL,
            .client_id = mqtt_state.client_id,
            .authentication = {
                .password = mqtt_state.password[0] != '\0' ? mqtt_state.password : NULL,
            },
        },
        .network = {
            .timeout_ms = MQTT_NETWORK_TIMEOUT_MS,
            .reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
        },
        .task = {
            .stack_size = MQTT_CLIENT_TASK_STACK,
        },
        .buffer = {
            .size = MQTT_BUFFER_SIZE,
            .out_size = MQTT_BUFFER_SIZE,
        },
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (client == NULL) {
        mqtt_lock();
        mqtt_set_error_locked(ESP_ERR_NO_MEM, "client init failed");
        mqtt_unlock();
        return ESP_ERR_NO_MEM;
    }

    ret = esp_mqtt_client_register_event(client,
                                         MQTT_EVENT_ANY,
                                         mqtt_event_handler,
                                         NULL);
    if (ret == ESP_OK) {
        mqtt_lock();
        mqtt_state.client = client;
        mqtt_state.running = true;
        mqtt_state.connected = false;
        mqtt_clear_error_locked();
        mqtt_unlock();
        solar_os_task_managed_admission_t admission;
        if (!solar_os_task_admit_managed("mqtt_client",
                                         MQTT_CLIENT_TASK_STACK,
                                         SOLAR_OS_TASK_ROLE_BACKGROUND,
                                         false,
                                         &admission)) {
            ret = ESP_ERR_NO_MEM;
        } else {
            ret = esp_mqtt_client_start(client);
            solar_os_task_note_managed_result("mqtt_client",
                                              MQTT_CLIENT_TASK_STACK,
                                              SOLAR_OS_TASK_ROLE_BACKGROUND,
                                              &admission,
                                              ret == ESP_OK);
        }
    }
    if (ret != ESP_OK) {
        mqtt_lock();
        if (mqtt_state.client == client) {
            mqtt_state.client = NULL;
            mqtt_state.running = false;
            mqtt_state.connected = false;
        }
        mqtt_set_error_locked(ret, esp_err_to_name(ret));
        mqtt_unlock();
        (void)esp_mqtt_client_stop(client);
        (void)esp_mqtt_client_destroy(client);
        return ret;
    }

    SOLAR_OS_LOGI(TAG, "connecting to %s", log_url);
    return ESP_OK;
}

esp_err_t solar_os_mqtt_disconnect(void)
{
    esp_err_t ret = solar_os_mqtt_init();
    if (ret != ESP_OK) {
        return ret;
    }

    return mqtt_stop_client();
}

esp_err_t solar_os_mqtt_publish(const char *topic,
                                const void *payload,
                                size_t payload_len,
                                int qos,
                                bool retain,
                                int *msg_id)
{
    if (!mqtt_topic_is_valid(topic) || payload_len > INT32_MAX || qos < 0 || qos > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > 0 && payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_mqtt_init();
    if (ret != ESP_OK) {
        return ret;
    }

    mqtt_lock();
    if (mqtt_state.client == NULL || !mqtt_state.connected) {
        mqtt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_mqtt_client_handle_t client = mqtt_state.client;
    mqtt_unlock();

    const int id = esp_mqtt_client_publish(client,
                                           topic,
                                           (const char *)payload,
                                           (int)payload_len,
                                           qos,
                                           retain ? 1 : 0);
    if (id < 0) {
        return ESP_FAIL;
    }
    if (msg_id != NULL) {
        *msg_id = id;
    }
    return ESP_OK;
}

esp_err_t solar_os_mqtt_subscribe(const char *topic, int qos, int *msg_id)
{
    if (!mqtt_topic_is_valid(topic) || qos < 0 || qos > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_mqtt_init();
    if (ret != ESP_OK) {
        return ret;
    }

    mqtt_lock();
    if (mqtt_state.client == NULL || !mqtt_state.connected) {
        mqtt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_mqtt_client_handle_t client = mqtt_state.client;
    mqtt_unlock();

    const int id = esp_mqtt_client_subscribe(client, topic, qos);
    if (id < 0) {
        return ESP_FAIL;
    }
    if (msg_id != NULL) {
        *msg_id = id;
    }
    return ESP_OK;
}

esp_err_t solar_os_mqtt_read_message(solar_os_mqtt_message_t *message, uint32_t timeout_ms)
{
    esp_err_t ret = solar_os_mqtt_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (message == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueReceive(mqtt_state.messages,
                      message,
                      pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t solar_os_mqtt_get_status(solar_os_mqtt_status_t *status)
{
    esp_err_t ret = solar_os_mqtt_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mqtt_lock();
    memset(status, 0, sizeof(*status));
    status->initialized = mqtt_state.initialized;
    status->configured = mqtt_state.configured;
    status->running = mqtt_state.running;
    status->connected = mqtt_state.connected;
    status->username_set = mqtt_state.username[0] != '\0';
    status->password_set = mqtt_state.password[0] != '\0';
    strlcpy(status->url, mqtt_state.url, sizeof(status->url));
    strlcpy(status->username, mqtt_state.username, sizeof(status->username));
    strlcpy(status->client_id, mqtt_state.client_id, sizeof(status->client_id));
    strlcpy(status->last_error, mqtt_state.last_error, sizeof(status->last_error));
    status->last_esp_error = mqtt_state.last_esp_error;
    status->last_msg_id = mqtt_state.last_msg_id;
    status->rx_count = mqtt_state.rx_count;
    status->tx_count = mqtt_state.tx_count;
    status->dropped_count = mqtt_state.dropped_count;
    status->queued_messages = mqtt_state.messages != NULL ? uxQueueMessagesWaiting(mqtt_state.messages) : 0;
    mqtt_unlock();
    return ESP_OK;
}
