#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_net.h"

#define SOLAR_OS_NET_SESSION_OWNER_MAX 24U
#define SOLAR_OS_NET_SESSION_MAX_CHANNELS 4U
#define SOLAR_OS_NET_GLOBAL_MAX_CHANNELS 8U
#define SOLAR_OS_NET_MAX_TRANSFER_BYTES (64U * 1024U)
#define SOLAR_OS_NET_MAX_UDP_BYTES 65507U
#define SOLAR_OS_NET_DEFAULT_CONNECT_TIMEOUT_MS 10000U
#define SOLAR_OS_NET_DEFAULT_IO_TIMEOUT_MS 1000U
#define SOLAR_OS_NET_MAX_TIMEOUT_MS 60000U
#define SOLAR_OS_NET_POLL_SLICE_MS 50U

typedef struct solar_os_net_session solar_os_net_session_t;
typedef bool (*solar_os_net_session_cancel_fn)(void *user);

typedef enum {
    SOLAR_OS_NET_CHANNEL_TCP = 1,
    SOLAR_OS_NET_CHANNEL_UDP,
    SOLAR_OS_NET_CHANNEL_WEBSOCKET,
} solar_os_net_channel_kind_t;

typedef enum {
    SOLAR_OS_NET_WS_CONTINUATION = 0,
    SOLAR_OS_NET_WS_TEXT = 1,
    SOLAR_OS_NET_WS_BINARY = 2,
    SOLAR_OS_NET_WS_CLOSE = 8,
    SOLAR_OS_NET_WS_PING = 9,
    SOLAR_OS_NET_WS_PONG = 10,
} solar_os_net_ws_opcode_t;

typedef struct {
    size_t data_len;
    size_t message_len;
    bool timed_out;
    bool closed;
    bool truncated;
    bool final;
    solar_os_net_ws_opcode_t opcode;
    char address[SOLAR_OS_NET_ADDR_MAX];
    uint16_t port;
} solar_os_net_receive_result_t;

typedef struct {
    char owner[SOLAR_OS_NET_SESSION_OWNER_MAX];
    size_t open_channels;
    size_t session_limit;
    size_t global_open_channels;
    size_t global_limit;
} solar_os_net_session_status_t;

/*
 * A session is owned by one caller and is not thread-safe. Handles are
 * opaque, generation-checked, and valid only in the session that created them.
 * Destroying a session closes every handle, including on interpreter errors.
 *
 * TCP and UDP waits poll should_cancel at most every
 * SOLAR_OS_NET_POLL_SLICE_MS. WebSocket DNS/TLS/upgrade and framed I/O use the
 * ESP transport timeout and check cancellation immediately before and after
 * that bounded call. Receive timeouts are normal results; connect/send
 * timeouts return ESP_ERR_TIMEOUT.
 */
esp_err_t solar_os_net_session_create(const char *owner,
                                      solar_os_net_session_cancel_fn should_cancel,
                                      void *cancel_user,
                                      solar_os_net_session_t **out_session);
void solar_os_net_session_destroy(solar_os_net_session_t *session);

esp_err_t solar_os_net_session_tcp_connect(solar_os_net_session_t *session,
                                           const char *host,
                                           uint16_t port,
                                           uint32_t timeout_ms,
                                           uint32_t *out_handle);
esp_err_t solar_os_net_session_udp_open(solar_os_net_session_t *session,
                                       uint16_t local_port,
                                       uint32_t *out_handle);
esp_err_t solar_os_net_session_websocket_connect(solar_os_net_session_t *session,
                                                 const char *url,
                                                 const char *subprotocol,
                                                 uint32_t timeout_ms,
                                                 uint32_t *out_handle);

esp_err_t solar_os_net_session_tcp_send(solar_os_net_session_t *session,
                                        uint32_t handle,
                                        const void *data,
                                        size_t data_len,
                                        uint32_t timeout_ms);
esp_err_t solar_os_net_session_udp_send(solar_os_net_session_t *session,
                                        uint32_t handle,
                                        const char *host,
                                        uint16_t port,
                                        const void *data,
                                        size_t data_len,
                                        uint32_t timeout_ms);
esp_err_t solar_os_net_session_websocket_send(solar_os_net_session_t *session,
                                              uint32_t handle,
                                              const void *data,
                                              size_t data_len,
                                              bool text,
                                              uint32_t timeout_ms);

esp_err_t solar_os_net_session_tcp_receive(solar_os_net_session_t *session,
                                           uint32_t handle,
                                           void *buffer,
                                           size_t buffer_len,
                                           uint32_t timeout_ms,
                                           solar_os_net_receive_result_t *result);
esp_err_t solar_os_net_session_udp_receive(solar_os_net_session_t *session,
                                           uint32_t handle,
                                           void *buffer,
                                           size_t buffer_len,
                                           uint32_t timeout_ms,
                                           solar_os_net_receive_result_t *result);
esp_err_t solar_os_net_session_websocket_receive(solar_os_net_session_t *session,
                                                 uint32_t handle,
                                                 void *buffer,
                                                 size_t buffer_len,
                                                 uint32_t timeout_ms,
                                                 solar_os_net_receive_result_t *result);

esp_err_t solar_os_net_session_close(solar_os_net_session_t *session,
                                     uint32_t handle);
void solar_os_net_session_close_all(solar_os_net_session_t *session);
void solar_os_net_session_get_status(const solar_os_net_session_t *session,
                                     solar_os_net_session_status_t *status);
const char *solar_os_net_ws_opcode_name(solar_os_net_ws_opcode_t opcode);
