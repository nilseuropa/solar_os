#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_bus_types.h"
#include "solar_os_midi_codec.h"
#include "solar_os_stream.h"

#define SOLAR_OS_MIDI_OWNER_MAX 24
#define SOLAR_OS_MIDI_CC_STREAM_MAX 16U

typedef struct {
    int index;
    uint32_t token;
} solar_os_midi_subscription_t;

#define SOLAR_OS_MIDI_SUBSCRIPTION_INIT { .index = -1, .token = 0 }

typedef struct {
    bool running;
    char bus_name[SOLAR_OS_BUS_NAME_MAX];
    uint32_t rx_bytes;
    uint32_t rx_messages;
    uint32_t tx_bytes;
    uint32_t tx_messages;
    uint32_t parser_unsupported;
    uint32_t subscriber_drops;
    uint32_t tx_drops;
    esp_err_t last_error;
} solar_os_midi_status_t;

typedef struct {
    char id[SOLAR_OS_STREAM_ID_MAX];
    uint8_t channel;
    uint8_t controller;
    bool has_value;
    uint8_t value;
    uint32_t updates;
} solar_os_midi_cc_stream_info_t;

esp_err_t solar_os_midi_subscribe(const char *owner,
                                  solar_os_midi_subscription_t *subscription);
esp_err_t solar_os_midi_unsubscribe(solar_os_midi_subscription_t *subscription);
esp_err_t solar_os_midi_receive(solar_os_midi_subscription_t *subscription,
                                solar_os_midi_message_t *message);

esp_err_t solar_os_midi_send(const solar_os_midi_message_t *message);
void solar_os_midi_get_status(solar_os_midi_status_t *status);

esp_err_t solar_os_midi_cc_stream_add(uint8_t channel, uint8_t controller);
esp_err_t solar_os_midi_cc_stream_remove(uint8_t channel, uint8_t controller);
esp_err_t solar_os_midi_cc_stream_clear(size_t *removed);
size_t solar_os_midi_cc_stream_count(void);
bool solar_os_midi_cc_stream_get(size_t index,
                                 solar_os_midi_cc_stream_info_t *info);

/* MIDI job integration. */
esp_err_t solar_os_midi_worker_start(const char *bus_name);
void solar_os_midi_worker_stop(void);
void solar_os_midi_worker_publish(const solar_os_midi_message_t *message);
bool solar_os_midi_worker_take_tx(solar_os_midi_message_t *message);
void solar_os_midi_worker_note_rx_bytes(size_t count);
void solar_os_midi_worker_note_tx(size_t count);
void solar_os_midi_worker_note_unsupported(void);
void solar_os_midi_worker_note_error(esp_err_t error);
