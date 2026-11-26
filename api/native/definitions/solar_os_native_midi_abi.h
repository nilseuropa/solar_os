#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_MIDI_SERVICE "midi"
#define SOLAR_OS_NATIVE_MIDI_ABI 1U
#define SOLAR_OS_MIDI_DATA_MAX 127U

typedef struct {
    uint8_t status, data1, data2, length;
} solar_os_midi_message_t;

typedef struct {
    uint8_t running_status, active_status;
    uint8_t data[2];
    uint8_t data_count, sysex;
} solar_os_midi_decoder_t;

typedef enum {
    SOLAR_OS_MIDI_DECODE_NONE,
    SOLAR_OS_MIDI_DECODE_MESSAGE,
    SOLAR_OS_MIDI_DECODE_UNSUPPORTED,
} solar_os_midi_decode_result_t;

typedef struct {
    bool running;
    char bus_name[16];
    uint32_t rx_bytes, rx_messages, tx_bytes, tx_messages;
    uint32_t parser_unsupported, subscriber_drops, tx_drops;
    int32_t last_error;
} solar_os_midi_status_t;

typedef struct {
    char id[48];
    uint8_t channel, controller;
    bool has_value;
    uint8_t value;
    uint32_t updates;
} solar_os_midi_cc_stream_info_t;

/* Use jobs to start or stop the MIDI worker. Subscribers have independent,
 * bounded receive queues and are closed with the owning application.
 * Receive returns TIMEOUT when there is no queued message. Send queues a
 * validated message for the bus worker.
 *
 * CC streams share the registry with stream and controls. Channels are
 * 1 through 16 and controller numbers are 0 through 127. A busy reader
 * can defer removal. Clear reports how many streams it removed.
 *
 * The codec uses caller-owned state and also works without an active device.
 * Initialize with decoder_reset. Give each independent byte source its own
 * state. Only DECODE_MESSAGE yields a decoded message. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*subscribe)(const char *owner, solar_os_native_handle_t *subscription);
    int (*unsubscribe)(solar_os_native_handle_t subscription);
    int (*receive)(solar_os_native_handle_t subscription, solar_os_midi_message_t *message);
    int (*send)(const solar_os_midi_message_t *message);
    int (*status)(solar_os_midi_status_t *status);
    int (*cc_add)(uint8_t channel, uint8_t controller);
    int (*cc_remove)(uint8_t channel, uint8_t controller);
    int (*cc_clear)(size_t *removed);
    size_t (*cc_count)(void);
    int (*cc_info)(size_t index, solar_os_midi_cc_stream_info_t *info);
    void (*decoder_reset)(solar_os_midi_decoder_t *decoder);
    solar_os_midi_decode_result_t (*decode_byte)(solar_os_midi_decoder_t *decoder, uint8_t byte,
        solar_os_midi_message_t *message);
    size_t (*message_length)(uint8_t status);
    bool (*message_valid)(const solar_os_midi_message_t *message);
    size_t (*encode)(const solar_os_midi_message_t *message, uint8_t output[3]);
    size_t (*format_monitor)(const solar_os_midi_message_t *message, char *output, size_t capacity);
} solar_os_native_midi_api_v1_t;
