#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mqtt_client.h"
#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_mqtt.h"
#include "solar_os_native_mqtt.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"

struct esp_mqtt_client {
    esp_event_handler_t handler;
    bool live, running;
};

struct hid_test_queue {
    solar_os_mqtt_message_t message;
    bool filled;
};

static struct esp_mqtt_client broker;
static struct hid_test_queue queue;
static const solar_os_native_mqtt_api_v1_t *api;
static bool fail_commit, fail_destroy;
static char logged_url[SOLAR_OS_MQTT_URL_MAX];
static const char private_url[] = "mqtts://account:fixture@broker:8883/endpoint?text=@other";
static const char public_url[] = "mqtts://broker:8883/endpoint?text=@other";
static pthread_mutex_t publish_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t publish_gate = PTHREAD_COND_INITIALIZER;
static bool publish_active, unblock_publish;
static atomic_bool disconnect_invoked;
static atomic_uint stops;

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{
    assert(level == SOLAR_OS_LOG_LEVEL_INFO && !strcmp(tag, "solar_os_mqtt") && !strcmp(format, "connecting to %s"));
    va_list args;
    va_start(args, format);
    const char *url = va_arg(args, const char *);
    snprintf(logged_url, sizeof(logged_url), "%s", url);
    va_end(args);
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t error) { (void)error; return "fixture error"; }
uint32_t esp_random(void) { return 42; }
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "mqtt"));
    if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    *out = 1;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{ (void)handle; (void)key; (void)value; (void)size; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{ assert(handle == 1 && key && value); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{ assert(handle == 1); return fail_commit ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

QueueHandle_t solar_os_queue_create(UBaseType_t length, UBaseType_t size)
{ assert(length == 8 && size == sizeof(queue.message)); return &queue; }
BaseType_t xQueueSend(QueueHandle_t handle, const void *record, TickType_t timeout)
{
    assert(handle == &queue && !timeout);
    if (queue.filled) return pdFAIL;
    memcpy(&queue.message, record, sizeof(queue.message));
    queue.filled = true;
    return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *record, TickType_t timeout)
{
    assert(handle == &queue && !timeout);
    if (!queue.filled) return pdFAIL;
    memcpy(record, &queue.message, sizeof(queue.message));
    queue.filled = false;
    return pdPASS;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t handle)
{ assert(handle == &queue); return queue.filled; }

bool solar_os_task_admit_managed(const char *name, size_t stack, int role, bool interactive,
    solar_os_task_managed_admission_t *out)
{
    assert(!strcmp(name, "mqtt_client") && stack && role == SOLAR_OS_TASK_ROLE_BACKGROUND && !interactive);
    out->admitted = true;
    return true;
}
void solar_os_task_note_managed_result(const char *name, size_t stack, int role,
    solar_os_task_managed_admission_t *in, bool started)
{ assert(name && stack && role == SOLAR_OS_TASK_ROLE_BACKGROUND && in->admitted && started); }

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    assert(!broker.live && !strcmp(config->broker.address.uri, private_url));
    assert(config->broker.verification.crt_bundle_attach == esp_crt_bundle_attach);
    broker = (struct esp_mqtt_client){.live = true};
    return &broker;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t event,
    esp_event_handler_t handler, void *user)
{
    assert(client == &broker && client->live && event == MQTT_EVENT_ANY && !user);
    client->handler = handler;
    return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client)
{
    assert(client->live && client->handler && !client->running);
    client->running = true;
    esp_mqtt_event_t event = {.client = client};
    client->handler(NULL, "mqtt", MQTT_EVENT_CONNECTED, &event);
    return ESP_OK;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client)
{
    assert(!pthread_mutex_lock(&publish_lock));
    assert(client->live && client->running && !publish_active);
    atomic_fetch_add(&stops, 1);
    client->running = false;
    assert(!pthread_mutex_unlock(&publish_lock));
    esp_mqtt_event_t event = {.client = client};
    client->handler(NULL, "mqtt", MQTT_EVENT_DISCONNECTED, &event);
    return ESP_OK;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client)
{
    assert(client->live && !client->running);
    if (fail_destroy) return ESP_ERR_TIMEOUT;
    client->live = false;
    return ESP_OK;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic, const char *data,
    int length, int qos, int retain)
{
    assert(client->live && client->running && !strcmp(topic, "input") && qos == 1 && !retain);
    if (!length) { assert(data[0] == 0); return 1; }
    assert(length == 3 && !memcmp(data, "a\0b", 3));
    assert(!pthread_mutex_lock(&publish_lock));
    publish_active = true;
    assert(!pthread_cond_broadcast(&publish_gate));
    while (!unblock_publish) assert(!pthread_cond_wait(&publish_gate, &publish_lock));
    publish_active = false;
    assert(!pthread_mutex_unlock(&publish_lock));
    return 2;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos)
{ assert(client->running && !strcmp(topic, "updates/#") && qos == 2); return 3; }

static void *publish(void *unused)
{
    (void)unused;
    int32_t id;
    assert(!api->publish("input", "a\0b", 3, 1, false, &id) && id == 2);
    return NULL;
}

static void *disconnect(void *unused)
{
    (void)unused;
    atomic_store(&disconnect_invoked, true);
    assert(api->disconnect() == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    return NULL;
}

int main(void)
{
    api = solar_os_native_mqtt_get_service("network.mqtt", 1, sizeof(*api));
    assert(api && !solar_os_native_mqtt_get_service("network.mqtt", 2, sizeof(*api)));
    assert(!api->connect(private_url, NULL, NULL));
    assert(!strcmp(logged_url, public_url));
    solar_os_mqtt_status_t status;
    assert(!api->status(&status) && status.connected && !strcmp(status.url, public_url));
    fail_commit = true;
    assert(api->connect("mqtts://uncommitted", "another", NULL) == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!api->status(&status) && status.connected && !status.username_set && !strcmp(status.url, public_url));
    fail_commit = false;

    int32_t id;
    assert(!api->subscribe("updates/#", 2, &id) && id == 3);
    const char unbounded_empty_argument = 'x';
    assert(!api->publish("input", &unbounded_empty_argument, 0, 1, false, &id) && id == 1);
    esp_mqtt_event_t event = {.client = &broker, .topic = "sample", .topic_len = 6,
        .data = "a\0b", .data_len = 3, .total_data_len = 9};
    broker.handler(NULL, "mqtt", MQTT_EVENT_DATA, &event);
    solar_os_mqtt_message_t message;
    assert(!api->read(&message, 0) && message.truncated && message.payload_len == 3);
    assert(!strcmp(message.topic, "sample") && !memcmp(message.payload, "a\0b", 3));
    assert(api->read(&message, 0) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && !message.payload_len);

    fail_destroy = true;
    pthread_t publisher, stopper;
    assert(!pthread_create(&publisher, NULL, publish, NULL));
    assert(!pthread_mutex_lock(&publish_lock));
    while (!publish_active) assert(!pthread_cond_wait(&publish_gate, &publish_lock));
    assert(!pthread_mutex_unlock(&publish_lock));
    assert(!pthread_create(&stopper, NULL, disconnect, NULL));
    while (!atomic_load(&disconnect_invoked)) nanosleep(&(struct timespec){.tv_nsec = 1000000L}, NULL);
    nanosleep(&(struct timespec){.tv_nsec = 10000000L}, NULL);
    assert(!atomic_load(&stops));
    assert(!pthread_mutex_lock(&publish_lock));
    unblock_publish = true;
    assert(!pthread_cond_broadcast(&publish_gate));
    assert(!pthread_mutex_unlock(&publish_lock));
    assert(!pthread_join(publisher, NULL) && !pthread_join(stopper, NULL));
    assert(broker.live && !broker.running && atomic_load(&stops) == 1);
    assert(!api->status(&status) && !status.connected && status.error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->publish("input", NULL, 0, 1, false, &id) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !id);
    fail_destroy = false;
    assert(!api->disconnect() && !broker.live && atomic_load(&stops) == 1);
    assert(!api->connect(NULL, NULL, NULL));
    assert(!api->disconnect() && !broker.live && atomic_load(&stops) == 2);
}
