#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_OSC_SERVICE "osc"
#define SOLAR_OS_NATIVE_OSC_ABI 1U
#define SOLAR_OS_NATIVE_OSC_SENT 1U
#define SOLAR_OS_NATIVE_OSC_SOURCE_ERROR 2U
#define SOLAR_OS_NATIVE_OSC_SEND_ERROR 3U

#define SOLAR_OS_OSC_PACKET_MAX 512U
#define SOLAR_OS_OSC_ADDRESS_MAX 96U
#define SOLAR_OS_OSC_BINDING_NAME_MAX 24U
#define SOLAR_OS_OSC_SOURCE_NAME_MAX 48U
#define SOLAR_OS_OSC_BINDING_MAX 16U
#define SOLAR_OS_OSC_BUNDLE_DEPTH_MAX 2U
#define SOLAR_OS_OSC_PACKET_UPDATE_MAX 8U
#define SOLAR_OS_OSC_ARGUMENT_MAX 8U
#define SOLAR_OS_OSC_RATE_MIN_MILLIHZ 100U
#define SOLAR_OS_OSC_RATE_MAX_MILLIHZ 100000U

typedef uint32_t solar_os_osc_source_t;
#define SOLAR_OS_OSC_SOURCE_STREAM 0U
#define SOLAR_OS_OSC_SOURCE_CONTROL 1U

typedef uint32_t solar_os_osc_value_t;
#define SOLAR_OS_OSC_VALUE_SCALAR 0U
#define SOLAR_OS_OSC_VALUE_EVENT 1U

typedef uint32_t solar_os_osc_edge_t;
#define SOLAR_OS_OSC_EDGE_RISING 0U
#define SOLAR_OS_OSC_EDGE_FALLING 1U
#define SOLAR_OS_OSC_EDGE_BOTH 2U

typedef uint32_t solar_os_osc_direction_t;
#define SOLAR_OS_OSC_SEND 0U
#define SOLAR_OS_OSC_RECEIVE 1U

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
    solar_os_osc_direction_t direction;
    uint32_t argument;
} solar_os_native_osc_binding_config_v1_t;

typedef struct {
    uint32_t id;
    solar_os_native_osc_binding_config_v1_t config;
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
    int32_t last_error;
    bool target_available;
    bool target_claimed;
    uint32_t received;
    uint32_t write_errors;
    uint64_t last_receive_ms;
} solar_os_native_osc_binding_info_v1_t;

typedef struct {
    uint32_t messages;
    uint32_t applied;
    uint32_t unknown_paths;
    uint32_t rejected_values;
} solar_os_native_osc_dispatch_result_v1_t;

/* Named bindings are shared with the OSC job and shell. Creating or clearing a
 * binding changes the system registry. Packet dispatch is synchronous and
 * applies existing bindings and parameter permissions. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*bind)(const solar_os_native_osc_binding_config_v1_t *config, uint32_t *binding_id);
    int (*unbind)(const char *name);
    int (*clear)(void);
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_osc_binding_info_v1_t *info);
    int (*dispatch)(const uint8_t *packet, size_t bytes, solar_os_native_osc_dispatch_result_v1_t *result);
    int (*encode_floats)(const char *address, const float *values, size_t count,
        uint8_t *packet, size_t capacity, size_t *bytes);
    int (*encode_integer)(const char *address, int32_t value, uint8_t *packet, size_t capacity, size_t *bytes);
    const char *(*source_name)(solar_os_osc_source_t source);
    const char *(*value_name)(solar_os_osc_value_t value);
    const char *(*edge_name)(solar_os_osc_edge_t edge);
    /* Sender jobs sample a due binding, then report transport success or
     * a source/send error. One worker coordinates each binding. */
    bool (*due)(const solar_os_native_osc_binding_info_v1_t *info, uint64_t now_ms);
    int (*prepare)(uint32_t id, uint64_t now_ms, float value, bool *send, bool *integer);
    int (*report)(uint32_t id, uint64_t now_ms, uint32_t outcome, int error);
    /* Receiver shutdown releases write leases while preserving bindings. */
    int (*release_receivers)(void);
} solar_os_native_osc_api_v1_t;
