#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);
typedef enum {
    MQTT_EVENT_ANY = -1, MQTT_EVENT_ERROR, MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_DATA, MQTT_EVENT_PUBLISHED,
} esp_mqtt_event_id_t;

#define MQTT_ERROR_TYPE_CONNECTION_REFUSED 1

typedef struct {
    int error_type, connect_return_code, esp_transport_sock_errno;
    esp_err_t esp_tls_last_esp_err;
} esp_mqtt_error_codes_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    esp_mqtt_error_codes_t *error_handle;
    const char *topic, *data;
    int topic_len, data_len, total_data_len, current_data_offset, msg_id, qos;
    bool retain;
} esp_mqtt_event_t;

typedef struct {
    struct {
        struct { const char *uri; } address;
        struct { esp_err_t (*crt_bundle_attach)(void *); } verification;
    } broker;
    struct {
        const char *username, *client_id;
        struct { const char *password; } authentication;
    } credentials;
    struct { int timeout_ms, reconnect_timeout_ms; } network;
    struct { int stack_size; } task;
    struct { int size, out_size; } buffer;
} esp_mqtt_client_config_t;

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, esp_mqtt_event_id_t event,
    esp_event_handler_t callback, void *user);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t client, const char *topic, const char *data,
    int length, int qos, int retain);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client, const char *topic, int qos);
