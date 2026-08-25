#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_OSC_PACKET_MAX 512U
#define SOLAR_OS_OSC_ADDRESS_MAX 96U
#define SOLAR_OS_OSC_BINDING_NAME_MAX 24U
#define SOLAR_OS_OSC_SOURCE_NAME_MAX 24U
#define SOLAR_OS_OSC_BINDING_MAX 16U
#define SOLAR_OS_OSC_BUNDLE_DEPTH_MAX 2U
#define SOLAR_OS_OSC_PACKET_UPDATE_MAX 8U
#define SOLAR_OS_OSC_RATE_MIN_MILLIHZ 100U
#define SOLAR_OS_OSC_RATE_MAX_MILLIHZ 100000U

typedef enum {
    SOLAR_OS_OSC_SOURCE_STREAM = 0,
    SOLAR_OS_OSC_SOURCE_CONTROL,
} solar_os_osc_source_t;

typedef enum {
    SOLAR_OS_OSC_VALUE_SCALAR = 0,
    SOLAR_OS_OSC_VALUE_EVENT,
} solar_os_osc_value_t;

typedef enum {
    SOLAR_OS_OSC_EDGE_RISING = 0,
    SOLAR_OS_OSC_EDGE_FALLING,
    SOLAR_OS_OSC_EDGE_BOTH,
} solar_os_osc_edge_t;

typedef struct {
    char name[SOLAR_OS_OSC_BINDING_NAME_MAX];
    solar_os_osc_source_t source_type;
    solar_os_osc_value_t value_type;
    char source[SOLAR_OS_OSC_SOURCE_NAME_MAX];
    char address[SOLAR_OS_OSC_ADDRESS_MAX];
    uint32_t interval_ms;
    float delta;
    bool send_always;
    solar_os_osc_edge_t edge;
} solar_os_osc_binding_config_t;

typedef struct {
    uint32_t id;
    solar_os_osc_binding_config_t config;
    bool source_available;
    bool has_value;
    float last_value;
    bool has_sent_value;
    float last_sent_value;
    uint64_t last_sample_ms;
    uint64_t last_send_ms;
    uint32_t sent;
    uint32_t send_errors;
    uint32_t source_errors;
    esp_err_t last_error;
} solar_os_osc_binding_info_t;

typedef struct {
    uint32_t messages;
    uint32_t applied;
    uint32_t unknown_paths;
    uint32_t rejected_values;
} solar_os_osc_dispatch_result_t;

esp_err_t solar_os_osc_bind(const solar_os_osc_binding_config_t *config,
                            uint32_t *binding_id);
esp_err_t solar_os_osc_unbind(const char *name);
void solar_os_osc_clear(void);
size_t solar_os_osc_binding_count(void);
bool solar_os_osc_binding_get(size_t index, solar_os_osc_binding_info_t *info);
bool solar_os_osc_binding_due(const solar_os_osc_binding_info_t *info,
                              uint64_t now_ms);
esp_err_t solar_os_osc_binding_prepare(uint32_t id,
                                       uint64_t now_ms,
                                       float value,
                                       bool *send,
                                       bool *send_integer);
void solar_os_osc_binding_note_error(uint32_t id,
                                     uint64_t now_ms,
                                     esp_err_t error,
                                     bool send_error);
void solar_os_osc_binding_note_sent(uint32_t id, uint64_t now_ms);

esp_err_t solar_os_osc_dispatch_packet(
    const uint8_t *packet,
    size_t packet_len,
    solar_os_osc_dispatch_result_t *result);
esp_err_t solar_os_osc_encode_float(const char *address,
                                    float value,
                                    uint8_t *packet,
                                    size_t packet_capacity,
                                    size_t *packet_len);
esp_err_t solar_os_osc_encode_int(const char *address,
                                  int32_t value,
                                  uint8_t *packet,
                                  size_t packet_capacity,
                                  size_t *packet_len);

const char *solar_os_osc_source_name(solar_os_osc_source_t source);
const char *solar_os_osc_value_name(solar_os_osc_value_t value);
const char *solar_os_osc_edge_name(solar_os_osc_edge_t edge);
