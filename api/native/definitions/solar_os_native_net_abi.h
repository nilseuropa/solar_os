#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_NET_SERVICE "net"
#define SOLAR_OS_NATIVE_NET_ABI 1U
#define SOLAR_OS_NATIVE_NET_DNS 1U
#define SOLAR_OS_NATIVE_NET_PING 2U
#define SOLAR_OS_NATIVE_NET_HOSTS 4U
#define SOLAR_OS_NATIVE_NET_CHANNELS 8U
#define SOLAR_OS_NATIVE_NET_TLS_CHANNELS 16U
#define SOLAR_OS_NATIVE_NET_TCP 1U
#define SOLAR_OS_NATIVE_NET_UDP 2U
#define SOLAR_OS_NATIVE_NET_WEBSOCKET 3U
#define SOLAR_OS_NATIVE_NET_TLS 4U
#define SOLAR_OS_NATIVE_NET_TIMED_OUT 1U
#define SOLAR_OS_NATIVE_NET_CLOSED 2U
#define SOLAR_OS_NATIVE_NET_TRUNCATED 4U
#define SOLAR_OS_NATIVE_NET_FINAL 8U
#define SOLAR_OS_NATIVE_NET_PING_REPLY 1U
#define SOLAR_OS_NATIVE_NET_PING_TIMEOUT 2U
typedef struct {
    uint64_t message_bytes;
    uint32_t bytes, flags, opcode, port;
    char address[46];
} solar_os_native_net_receive_v1_t;
typedef struct {
    char owner[24];
    uint32_t channels, session_limit, global_channels, global_limit;
} solar_os_native_net_status_v1_t;
typedef struct {
    uint32_t count, timeout_ms, interval_ms, data_size;
} solar_os_native_ping_options_v1_t;
typedef struct {
    uint32_t type, sequence, bytes, elapsed_ms, ttl;
    char from[46];
} solar_os_native_ping_event_v1_t;
typedef struct {
    char address[46];
    uint32_t interrupted, transmitted, received, loss_percent, total_ms, minimum_ms, average_ms, maximum_ms;
} solar_os_native_ping_result_v1_t;
typedef void (*solar_os_native_ping_event_fn)(const solar_os_native_ping_event_v1_t *event, void *user);
typedef bool (*solar_os_native_host_visit_fn)(const char *address, const char *alias, void *user);

/* A context owns channels and a host-side atomic cancellation flag. Serialize
 * operations on one context; cancel is safe from another task. Reset the flag
 * only after the interrupted call has returned. Close refuses active calls.
 * Channel IDs are scoped to their creating context. Data calls are bounded by
 * the underlying OS limits (64 KiB, UDP 65507 bytes, waits <=60 seconds).
 * Ping/hosts callbacks are retained only during the call and run on the
 * calling task. The OS marshals its ping worker's events to that task.
 * Event/string pointers expire when the callback returns. Keep callback
 * code/data alive through the call. */
typedef struct {
    uint32_t abi_version, struct_size, capabilities;
    int (*create)(const char *owner, solar_os_native_handle_t *context);
    int (*close)(solar_os_native_handle_t context);
    int (*cancel)(solar_os_native_handle_t context, bool requested);
    int (*resolve)(const char *host, char *address, size_t capacity);
    int (*hosts)(solar_os_native_host_visit_fn visitor, void *user);
    int (*ping)(solar_os_native_handle_t context, const char *host,
        const solar_os_native_ping_options_v1_t *options, solar_os_native_ping_event_fn event,
        void *user, solar_os_native_ping_result_v1_t *result);
    int (*tcp_connect)(solar_os_native_handle_t context, const char *host, uint16_t port,
        uint32_t timeout_ms, uint32_t *channel);
    int (*udp_open)(solar_os_native_handle_t context, uint16_t local_port, uint32_t *channel);
    int (*websocket_connect)(solar_os_native_handle_t context, const char *url,
        const char *subprotocol, uint32_t timeout_ms, uint32_t *channel);
    int (*send)(solar_os_native_handle_t context, uint32_t channel, uint32_t type,
        const char *host, uint16_t port, const void *bytes, size_t size, bool text, uint32_t timeout_ms);
    int (*receive)(solar_os_native_handle_t context, uint32_t channel, uint32_t type,
        void *bytes, size_t capacity, uint32_t timeout_ms, solar_os_native_net_receive_v1_t *result);
    int (*channel_close)(solar_os_native_handle_t context, uint32_t channel);
    int (*close_all)(solar_os_native_handle_t context);
    int (*status)(solar_os_native_handle_t context, solar_os_native_net_status_v1_t *status);
    const char *(*websocket_opcode_name)(uint32_t opcode);
    /* Open a verified TLS byte stream using the firmware trust bundle and
     * hostname verification. Zero selects the default connect timeout.
     * Use TLS as the send/receive type. A failed or incomplete write may
     * close its channel. Reopen the connection before retrying a write. */
    int (*tls_connect)(solar_os_native_handle_t context, const char *host, uint16_t port,
        uint32_t timeout_ms, uint32_t *channel);
} solar_os_native_net_api_v1_t;
