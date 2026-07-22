#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_chat.h"

#define SOLAR_OS_CHAT_TRANSPORT_CURSOR_MAX 128

typedef struct {
    solar_os_chat_event_t event;
    /* Opaque resume token owned by chat-sync; empty for push-only transports. */
    char cursor[SOLAR_OS_CHAT_TRANSPORT_CURSOR_MAX];
} solar_os_chat_transport_event_t;

typedef struct {
    bool initialized;
    bool running;
    bool connected;
    char last_error[SOLAR_OS_CHAT_ERROR_MAX];
    esp_err_t last_esp_error;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    size_t queued_events;
} solar_os_chat_transport_status_t;

typedef struct {
    const char *name;
    esp_err_t (*init)(void);
    esp_err_t (*connect)(const solar_os_chat_config_t *config,
                         const char *cursor);
    esp_err_t (*request_stop)(void);
    esp_err_t (*reap)(void);
    esp_err_t (*disconnect)(void);
    esp_err_t (*submit)(const solar_os_chat_command_t *command);
    esp_err_t (*read_event)(solar_os_chat_transport_event_t *event,
                            uint32_t timeout_ms);
    esp_err_t (*get_status)(solar_os_chat_transport_status_t *status);
} solar_os_chat_transport_t;
