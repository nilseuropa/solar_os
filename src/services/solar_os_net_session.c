#include "solar_os_net_session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "lwip/inet.h"
#include "solar_os_wifi.h"
#include "solar_os_tls.h"

#define SOLAR_OS_NET_WS_URL_MAX 384U
#define SOLAR_OS_NET_WS_PATH_MAX 256U
#define SOLAR_OS_NET_WS_DRAIN_BYTES 256U

typedef struct {
    solar_os_net_channel_kind_t kind;
    uint32_t generation;
    bool open;
    int fd;
    union {
        esp_transport_handle_t transport;
        solar_os_tls_connection_t *tls;
    };
    esp_transport_handle_t parent_transport;
} solar_os_net_channel_t;

struct solar_os_net_session {
    char owner[SOLAR_OS_NET_SESSION_OWNER_MAX];
    solar_os_net_session_cancel_fn should_cancel;
    void *cancel_user;
    solar_os_net_channel_t channels[SOLAR_OS_NET_SESSION_MAX_CHANNELS];
};

typedef struct {
    char host[SOLAR_OS_NET_HOST_MAX];
    char path[SOLAR_OS_NET_WS_PATH_MAX];
    uint16_t port;
    bool tls;
} solar_os_net_ws_endpoint_t;

static portMUX_TYPE net_channel_lock = portMUX_INITIALIZER_UNLOCKED;
static size_t net_global_open_channels;

static bool net_cancelled(const solar_os_net_session_t *session)
{
    return session != NULL && session->should_cancel != NULL &&
        session->should_cancel(session->cancel_user);
}

static esp_err_t net_validate_timeout(uint32_t timeout_ms)
{
    return timeout_ms <= SOLAR_OS_NET_MAX_TIMEOUT_MS ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t net_require_ip(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    return status.has_ip ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t net_errno_error(int value)
{
    switch (value) {
    case ENOMEM:
        return ESP_ERR_NO_MEM;
    case EINVAL:
        return ESP_ERR_INVALID_ARG;
    case ETIMEDOUT:
    case EAGAIN:
        return ESP_ERR_TIMEOUT;
    default:
        return ESP_FAIL;
    }
}

static int64_t net_deadline(uint32_t timeout_ms)
{
    return esp_timer_get_time() + (int64_t)timeout_ms * 1000;
}

static int net_remaining_ms(int64_t deadline_us)
{
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    const int64_t remaining_ms = (remaining_us + 999) / 1000;
    return remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
}

static int net_wait_fd(const solar_os_net_session_t *session,
                       int fd,
                       bool write_ready,
                       uint32_t timeout_ms)
{
    const int64_t deadline_us = net_deadline(timeout_ms);
    bool first = true;
    while (first || net_remaining_ms(deadline_us) > 0) {
        first = false;
        if (net_cancelled(session)) {
            return -2;
        }

        int slice_ms = net_remaining_ms(deadline_us);
        if (slice_ms > (int)SOLAR_OS_NET_POLL_SLICE_MS) {
            slice_ms = SOLAR_OS_NET_POLL_SLICE_MS;
        }
        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(fd, write_ready ? &writefds : &readfds);
        struct timeval timeout = {
            .tv_sec = slice_ms / 1000,
            .tv_usec = (slice_ms % 1000) * 1000,
        };
        const int ready = select(fd + 1,
                                 write_ready ? NULL : &readfds,
                                 write_ready ? &writefds : NULL,
                                 NULL,
                                 &timeout);
        if (ready > 0) {
            return 1;
        }
        if (ready < 0 && errno != EINTR) {
            return -1;
        }
        if (timeout_ms == 0) {
            break;
        }
    }
    return net_cancelled(session) ? -2 : 0;
}

static esp_err_t net_wait_error(int wait_result)
{
    if (wait_result == -2) {
        return ESP_ERR_INVALID_STATE;
    }
    if (wait_result == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return wait_result < 0 ? net_errno_error(errno) : ESP_OK;
}

static esp_err_t net_set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return net_errno_error(errno);
    }
    return ESP_OK;
}

static esp_err_t net_resolve_ipv4(const char *host,
                                  uint16_t port,
                                  struct sockaddr_in *address)
{
    if (host == NULL || host[0] == '\0' || port == 0 || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char resolved[SOLAR_OS_NET_ADDR_MAX];
    esp_err_t err = solar_os_net_resolve_host(host, resolved, sizeof(resolved));
    if (err != ESP_OK) {
        return err;
    }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    return inet_pton(AF_INET, resolved, &address->sin_addr) == 1 ?
        ESP_OK : ESP_ERR_NOT_FOUND;
}

static solar_os_net_channel_t *net_reserve_channel(solar_os_net_session_t *session,
                                                    solar_os_net_channel_kind_t kind,
                                                    uint32_t *out_handle)
{
    if (session == NULL || out_handle == NULL) {
        return NULL;
    }

    size_t index = SOLAR_OS_NET_SESSION_MAX_CHANNELS;
    portENTER_CRITICAL(&net_channel_lock);
    if (net_global_open_channels < SOLAR_OS_NET_GLOBAL_MAX_CHANNELS) {
        for (size_t i = 0; i < SOLAR_OS_NET_SESSION_MAX_CHANNELS; i++) {
            if (!session->channels[i].open) {
                index = i;
                session->channels[i].open = true;
                net_global_open_channels++;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&net_channel_lock);
    if (index == SOLAR_OS_NET_SESSION_MAX_CHANNELS) {
        return NULL;
    }

    solar_os_net_channel_t *channel = &session->channels[index];
    channel->kind = kind;
    channel->fd = -1;
    channel->transport = NULL;
    channel->parent_transport = NULL;
    channel->generation++;
    if (channel->generation == 0 ||
        channel->generation > UINT32_MAX / SOLAR_OS_NET_SESSION_MAX_CHANNELS) {
        channel->generation = 1;
    }
    *out_handle = (channel->generation - 1U) * SOLAR_OS_NET_SESSION_MAX_CHANNELS +
        (uint32_t)index + 1U;
    return channel;
}

static solar_os_net_channel_t *net_find_channel(solar_os_net_session_t *session,
                                                 uint32_t handle,
                                                 solar_os_net_channel_kind_t kind)
{
    if (session == NULL || handle == 0) {
        return NULL;
    }
    const uint32_t adjusted = handle - 1U;
    const size_t index = adjusted % SOLAR_OS_NET_SESSION_MAX_CHANNELS;
    const uint32_t generation = adjusted / SOLAR_OS_NET_SESSION_MAX_CHANNELS + 1U;
    solar_os_net_channel_t *channel = &session->channels[index];
    return channel->open && channel->generation == generation && channel->kind == kind ?
        channel : NULL;
}

static void net_release_channel(solar_os_net_channel_t *channel)
{
    if (channel == NULL || !channel->open) {
        return;
    }
    if (channel->kind == SOLAR_OS_NET_CHANNEL_WEBSOCKET) {
        if (channel->transport != NULL) {
            (void)esp_transport_close(channel->transport);
            (void)esp_transport_destroy(channel->transport);
        }
        if (channel->parent_transport != NULL) {
            (void)esp_transport_destroy(channel->parent_transport);
        }
    } else if (channel->kind == SOLAR_OS_NET_CHANNEL_TLS) {
        solar_os_tls_close(channel->tls);
    } else if (channel->fd >= 0) {
        (void)close(channel->fd);
    }
    channel->fd = -1;
    channel->transport = NULL;
    channel->parent_transport = NULL;
    channel->kind = 0;

    portENTER_CRITICAL(&net_channel_lock);
    channel->open = false;
    if (net_global_open_channels > 0) {
        net_global_open_channels--;
    }
    portEXIT_CRITICAL(&net_channel_lock);
}

static esp_err_t net_validate_transfer(const void *data, size_t data_len, size_t maximum)
{
    if ((data == NULL && data_len != 0) || data_len > maximum) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t solar_os_net_session_create(const char *owner,
                                      solar_os_net_session_cancel_fn should_cancel,
                                      void *cancel_user,
                                      solar_os_net_session_t **out_session)
{
    if (owner == NULL || owner[0] == '\0' || strlen(owner) >= SOLAR_OS_NET_SESSION_OWNER_MAX ||
        out_session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_session = NULL;
    solar_os_net_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(session->owner, owner, sizeof(session->owner));
    session->should_cancel = should_cancel;
    session->cancel_user = cancel_user;
    for (size_t i = 0; i < SOLAR_OS_NET_SESSION_MAX_CHANNELS; i++) {
        session->channels[i].fd = -1;
    }
    *out_session = session;
    return ESP_OK;
}

void solar_os_net_session_destroy(solar_os_net_session_t *session)
{
    if (session == NULL) {
        return;
    }
    solar_os_net_session_close_all(session);
    free(session);
}

esp_err_t solar_os_net_session_tcp_connect(solar_os_net_session_t *session,
                                           const char *host,
                                           uint16_t port,
                                           uint32_t timeout_ms,
                                           uint32_t *out_handle)
{
    esp_err_t err = net_validate_timeout(timeout_ms);
    if (err != ESP_OK || session == NULL || out_handle == NULL) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }
    if ((err = net_require_ip()) != ESP_OK || net_cancelled(session)) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    const int64_t deadline_us = net_deadline(timeout_ms);
    struct sockaddr_in address;
    if ((err = net_resolve_ipv4(host, port, &address)) != ESP_OK) {
        return err;
    }
    if (net_cancelled(session)) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_net_channel_t *channel = net_reserve_channel(session,
                                                          SOLAR_OS_NET_CHANNEL_TCP,
                                                          out_handle);
    if (channel == NULL) {
        return ESP_ERR_NO_MEM;
    }
    channel->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (channel->fd < 0 || (err = net_set_nonblocking(channel->fd)) != ESP_OK) {
        err = channel->fd < 0 ? net_errno_error(errno) : err;
        net_release_channel(channel);
        return err;
    }

    int connected = connect(channel->fd,
                            (const struct sockaddr *)&address,
                            sizeof(address));
    if (connected < 0 && errno != EINPROGRESS) {
        err = net_errno_error(errno);
        net_release_channel(channel);
        return err;
    }
    if (connected < 0) {
        const int remaining = net_remaining_ms(deadline_us);
        const int ready = net_wait_fd(session,
                                      channel->fd,
                                      true,
                                      remaining > 0 ? (uint32_t)remaining : 0U);
        if (ready != 1) {
            err = net_wait_error(ready);
            net_release_channel(channel);
            return err;
        }
        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (getsockopt(channel->fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 ||
            socket_error != 0) {
            err = net_errno_error(socket_error != 0 ? socket_error : errno);
            net_release_channel(channel);
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_net_session_udp_open(solar_os_net_session_t *session,
                                       uint16_t local_port,
                                       uint32_t *out_handle)
{
    if (session == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = net_require_ip();
    if (err != ESP_OK || net_cancelled(session)) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    solar_os_net_channel_t *channel = net_reserve_channel(session,
                                                          SOLAR_OS_NET_CHANNEL_UDP,
                                                          out_handle);
    if (channel == NULL) {
        return ESP_ERR_NO_MEM;
    }
    channel->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (channel->fd < 0 || (err = net_set_nonblocking(channel->fd)) != ESP_OK) {
        err = channel->fd < 0 ? net_errno_error(errno) : err;
        net_release_channel(channel);
        return err;
    }
    if (local_port != 0) {
        const struct sockaddr_in local = {
            .sin_family = AF_INET,
            .sin_port = htons(local_port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(channel->fd, (const struct sockaddr *)&local, sizeof(local)) != 0) {
            err = net_errno_error(errno);
            net_release_channel(channel);
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t net_parse_ws_url(const char *url, solar_os_net_ws_endpoint_t *endpoint)
{
    if (url == NULL || endpoint == NULL || strlen(url) >= SOLAR_OS_NET_WS_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    const char *cursor;
    if (strncmp(url, "ws://", 5) == 0) {
        endpoint->tls = false;
        endpoint->port = 80;
        cursor = url + 5;
    } else if (strncmp(url, "wss://", 6) == 0) {
        endpoint->tls = true;
        endpoint->port = 443;
        cursor = url + 6;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const char *host = cursor;
    while (*cursor != '\0' && *cursor != ':' && *cursor != '/') {
        if (*cursor == '@' || *cursor == '[' || *cursor == ']') {
            return ESP_ERR_NOT_SUPPORTED;
        }
        cursor++;
    }
    const size_t host_len = (size_t)(cursor - host);
    if (host_len == 0 || host_len >= sizeof(endpoint->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(endpoint->host, host, host_len);
    endpoint->host[host_len] = '\0';

    if (*cursor == ':') {
        cursor++;
        uint32_t port = 0;
        const char *port_start = cursor;
        while (*cursor >= '0' && *cursor <= '9') {
            port = port * 10U + (uint32_t)(*cursor - '0');
            if (port > UINT16_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            cursor++;
        }
        if (cursor == port_start || port == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        endpoint->port = (uint16_t)port;
    }
    if (*cursor != '\0' && *cursor != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    const char *path = *cursor == '\0' ? "/" : cursor;
    if (strchr(path, '#') != NULL || strlen(path) >= sizeof(endpoint->path)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(endpoint->path, path, sizeof(endpoint->path));
    return ESP_OK;
}

esp_err_t solar_os_net_session_websocket_connect(solar_os_net_session_t *session,
                                                 const char *url,
                                                 const char *subprotocol,
                                                 uint32_t timeout_ms,
                                                 uint32_t *out_handle)
{
    esp_err_t err = net_validate_timeout(timeout_ms);
    if (err != ESP_OK || session == NULL || out_handle == NULL ||
        (subprotocol != NULL && (strlen(subprotocol) > 64 || strchr(subprotocol, '\r') != NULL ||
                                 strchr(subprotocol, '\n') != NULL))) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }
    if ((err = net_require_ip()) != ESP_OK || net_cancelled(session)) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    solar_os_net_ws_endpoint_t endpoint;
    if ((err = net_parse_ws_url(url, &endpoint)) != ESP_OK) {
        return err;
    }

    solar_os_net_channel_t *channel = net_reserve_channel(session,
                                                          SOLAR_OS_NET_CHANNEL_WEBSOCKET,
                                                          out_handle);
    if (channel == NULL) {
        return ESP_ERR_NO_MEM;
    }
    channel->parent_transport = endpoint.tls ? esp_transport_ssl_init() :
        esp_transport_tcp_init();
    if (channel->parent_transport == NULL) {
        net_release_channel(channel);
        return ESP_ERR_NO_MEM;
    }
    if (endpoint.tls) {
        esp_transport_ssl_crt_bundle_attach(channel->parent_transport,
                                            esp_crt_bundle_attach);
    }
    channel->transport = esp_transport_ws_init(channel->parent_transport);
    if (channel->transport == NULL) {
        net_release_channel(channel);
        return ESP_ERR_NO_MEM;
    }
    const esp_transport_ws_config_t config = {
        .ws_path = endpoint.path,
        .sub_protocol = subprotocol,
        .user_agent = "SolarOS script",
        .propagate_control_frames = false,
    };
    if ((err = esp_transport_ws_set_config(channel->transport, &config)) != ESP_OK) {
        net_release_channel(channel);
        return err;
    }
    if (net_cancelled(session)) {
        net_release_channel(channel);
        return ESP_ERR_INVALID_STATE;
    }

    const int connected = esp_transport_connect(channel->transport,
                                                endpoint.host,
                                                endpoint.port,
                                                (int)timeout_ms);
    if (connected != 0 || net_cancelled(session)) {
        err = net_cancelled(session) ? ESP_ERR_INVALID_STATE :
            connected == 0 ? ESP_OK :
            errno == ETIMEDOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        net_release_channel(channel);
        return err;
    }
    return ESP_OK;
}

esp_err_t solar_os_net_session_tcp_send(solar_os_net_session_t *session,
                                        uint32_t handle,
                                        const void *data,
                                        size_t data_len,
                                        uint32_t timeout_ms)
{
    solar_os_net_channel_t *channel = net_find_channel(session, handle,
                                                       SOLAR_OS_NET_CHANNEL_TCP);
    esp_err_t err = net_validate_timeout(timeout_ms);
    if (channel == NULL || err != ESP_OK ||
        (err = net_validate_transfer(data, data_len, SOLAR_OS_NET_MAX_TRANSFER_BYTES)) != ESP_OK) {
        return channel == NULL ? ESP_ERR_INVALID_ARG : err;
    }
    size_t offset = 0;
    const int64_t deadline_us = net_deadline(timeout_ms);
    while (offset < data_len) {
        const int remaining = net_remaining_ms(deadline_us);
        const int ready = net_wait_fd(session,
                                      channel->fd,
                                      true,
                                      remaining > 0 ? (uint32_t)remaining : 0U);
        if (ready != 1) {
            return net_wait_error(ready);
        }
        const ssize_t sent = send(channel->fd,
                                  (const uint8_t *)data + offset,
                                  data_len - offset,
                                  0);
        if (sent < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (sent <= 0) {
            return sent == 0 ? ESP_ERR_INVALID_STATE : net_errno_error(errno);
        }
        offset += (size_t)sent;
    }
    return net_cancelled(session) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t solar_os_net_session_udp_send(solar_os_net_session_t *session,
                                        uint32_t handle,
                                        const char *host,
                                        uint16_t port,
                                        const void *data,
                                        size_t data_len,
                                        uint32_t timeout_ms)
{
    solar_os_net_channel_t *channel = net_find_channel(session, handle,
                                                       SOLAR_OS_NET_CHANNEL_UDP);
    esp_err_t err = net_validate_timeout(timeout_ms);
    if (channel == NULL || err != ESP_OK ||
        (err = net_validate_transfer(data, data_len, SOLAR_OS_NET_MAX_UDP_BYTES)) != ESP_OK) {
        return channel == NULL ? ESP_ERR_INVALID_ARG : err;
    }
    const int64_t deadline_us = net_deadline(timeout_ms);
    struct sockaddr_in address;
    if ((err = net_resolve_ipv4(host, port, &address)) != ESP_OK || net_cancelled(session)) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    const int remaining = net_remaining_ms(deadline_us);
    const int ready = net_wait_fd(session,
                                  channel->fd,
                                  true,
                                  remaining > 0 ? (uint32_t)remaining : 0U);
    if (ready != 1) {
        return net_wait_error(ready);
    }
    const ssize_t sent = sendto(channel->fd,
                                data,
                                data_len,
                                0,
                                (const struct sockaddr *)&address,
                                sizeof(address));
    if (sent < 0) {
        return net_errno_error(errno);
    }
    return (size_t)sent == data_len ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_net_session_websocket_send(solar_os_net_session_t *session,
                                              uint32_t handle,
                                              const void *data,
                                              size_t data_len,
                                              bool text,
                                              uint32_t timeout_ms)
{
    solar_os_net_channel_t *channel = net_find_channel(session, handle,
                                                       SOLAR_OS_NET_CHANNEL_WEBSOCKET);
    esp_err_t err = net_validate_timeout(timeout_ms);
    if (channel == NULL || err != ESP_OK ||
        (err = net_validate_transfer(data, data_len, SOLAR_OS_NET_MAX_TRANSFER_BYTES)) != ESP_OK) {
        return channel == NULL ? ESP_ERR_INVALID_ARG : err;
    }
    if (net_cancelled(session)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *copy = NULL;
    if (data_len != 0) {
        copy = malloc(data_len);
        if (copy == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, data, data_len);
    }
    const ws_transport_opcodes_t opcode = (text ? WS_TRANSPORT_OPCODES_TEXT :
        WS_TRANSPORT_OPCODES_BINARY) | WS_TRANSPORT_OPCODES_FIN;
    const int sent = esp_transport_ws_send_raw(channel->transport,
                                               opcode,
                                               (const char *)copy,
                                               (int)data_len,
                                               (int)timeout_ms);
    free(copy);
    if (net_cancelled(session)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sent < 0) {
        return errno == ETIMEDOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    return (size_t)sent == data_len ? ESP_OK : ESP_FAIL;
}

static esp_err_t net_prepare_receive(solar_os_net_session_t *session,
                                     void *buffer,
                                     size_t buffer_len,
                                     uint32_t timeout_ms,
                                     solar_os_net_receive_result_t *result)
{
    if (session == NULL || buffer == NULL || buffer_len == 0 ||
        buffer_len > SOLAR_OS_NET_MAX_TRANSFER_BYTES || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = net_validate_timeout(timeout_ms);
    if (err != ESP_OK) {
        return err;
    }
    memset(result, 0, sizeof(*result));
    return net_cancelled(session) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t solar_os_net_session_tcp_receive(solar_os_net_session_t *session,
                                           uint32_t handle,
                                           void *buffer,
                                           size_t buffer_len,
                                           uint32_t timeout_ms,
                                           solar_os_net_receive_result_t *result)
{
    solar_os_net_channel_t *channel = net_find_channel(session, handle,
                                                       SOLAR_OS_NET_CHANNEL_TCP);
    esp_err_t err = net_prepare_receive(session, buffer, buffer_len, timeout_ms, result);
    if (channel == NULL || err != ESP_OK) {
        return channel == NULL ? ESP_ERR_INVALID_ARG : err;
    }
    const int ready = net_wait_fd(session, channel->fd, false, timeout_ms);
    if (ready == 0) {
        result->timed_out = true;
        return ESP_OK;
    }
    if (ready != 1) {
        return net_wait_error(ready);
    }
    const ssize_t received = recv(channel->fd, buffer, buffer_len, 0);
    if (received < 0) {
        return net_errno_error(errno);
    }
    result->data_len = received > 0 ? (size_t)received : 0U;
    result->message_len = result->data_len;
    result->closed = received == 0;
    return ESP_OK;
}

esp_err_t solar_os_net_session_udp_receive(solar_os_net_session_t *session,
                                           uint32_t handle,
                                           void *buffer,
                                           size_t buffer_len,
                                           uint32_t timeout_ms,
                                           solar_os_net_receive_result_t *result)
{
    solar_os_net_channel_t *channel = net_find_channel(session, handle,
                                                       SOLAR_OS_NET_CHANNEL_UDP);
    esp_err_t err = net_prepare_receive(session, buffer, buffer_len, timeout_ms, result);
    if (channel == NULL || err != ESP_OK) {
        return channel == NULL ? ESP_ERR_INVALID_ARG : err;
    }
    const int ready = net_wait_fd(session, channel->fd, false, timeout_ms);
    if (ready == 0) {
        result->timed_out = true;
        return ESP_OK;
    }
    if (ready != 1) {
        return net_wait_error(ready);
    }
    struct sockaddr_in source;
    socklen_t source_len = sizeof(source);
    const ssize_t received = recvfrom(channel->fd,
                                      buffer,
                                      buffer_len,
                                      MSG_TRUNC,
                                      (struct sockaddr *)&source,
                                      &source_len);
    if (received < 0) {
        return net_errno_error(errno);
    }
    result->message_len = (size_t)received;
    result->data_len = result->message_len > buffer_len ? buffer_len : result->message_len;
    result->truncated = result->message_len > buffer_len;
    result->port = ntohs(source.sin_port);
    if (inet_ntop(AF_INET, &source.sin_addr, result->address, sizeof(result->address)) == NULL) {
        strlcpy(result->address, "0.0.0.0", sizeof(result->address));
    }
    return ESP_OK;
}

esp_err_t solar_os_net_session_websocket_receive(solar_os_net_session_t *session,
                                                 uint32_t handle,
                                                 void *buffer,
                                                 size_t buffer_len,
                                                 uint32_t timeout_ms,
                                                 solar_os_net_receive_result_t *result)
{
    solar_os_net_channel_t *channel = net_find_channel(session, handle,
                                                       SOLAR_OS_NET_CHANNEL_WEBSOCKET);
    esp_err_t err = net_prepare_receive(session, buffer, buffer_len, timeout_ms, result);
    if (channel == NULL || err != ESP_OK) {
        return channel == NULL ? ESP_ERR_INVALID_ARG : err;
    }

    const int64_t deadline_us = net_deadline(timeout_ms);
    const int ready = esp_transport_poll_read(channel->transport, (int)timeout_ms);
    if (net_cancelled(session)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ready == 0) {
        result->timed_out = true;
        return ESP_OK;
    }
    if (ready < 0) {
        return ESP_FAIL;
    }
    const int remaining_ms = net_remaining_ms(deadline_us);
    const int received = esp_transport_read(channel->transport,
                                            buffer,
                                            (int)buffer_len,
                                            remaining_ms);
    if (net_cancelled(session)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (received < 0) {
        return errno == ETIMEDOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    result->data_len = received > 0 ? (size_t)received : 0U;
    const int payload_len = esp_transport_ws_get_read_payload_len(channel->transport);
    result->message_len = payload_len > 0 ? (size_t)payload_len : result->data_len;
    result->opcode = (solar_os_net_ws_opcode_t)
        (esp_transport_ws_get_read_opcode(channel->transport) & 0x0f);
    result->final = esp_transport_ws_get_fin_flag(channel->transport);

    size_t remaining = result->message_len > result->data_len ?
        result->message_len - result->data_len : 0U;
    result->truncated = remaining != 0;
    uint8_t drain[SOLAR_OS_NET_WS_DRAIN_BYTES];
    while (remaining > 0) {
        if (net_cancelled(session)) {
            return ESP_ERR_INVALID_STATE;
        }
        const size_t chunk = remaining > sizeof(drain) ? sizeof(drain) : remaining;
        const int drain_timeout_ms = net_remaining_ms(deadline_us);
        const int drained = esp_transport_read(channel->transport,
                                               drain,
                                               (int)chunk,
                                               drain_timeout_ms);
        if (net_cancelled(session)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (drained <= 0) {
            return drained == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        remaining -= (size_t)drained;
    }
    result->closed = result->opcode == SOLAR_OS_NET_WS_CLOSE;
    return net_cancelled(session) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t solar_os_net_session_tls_connect(solar_os_net_session_t *session, const char *host,
    uint16_t port, uint32_t timeout_ms, uint32_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = 0;
    if (!session || !host || !*host || !port || net_validate_timeout(timeout_ms) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    const esp_err_t ip = net_require_ip();
    if (ip != ESP_OK) return ip;
    if (net_cancelled(session)) return ESP_ERR_INVALID_STATE;

    solar_os_net_channel_t *channel = net_reserve_channel(session, SOLAR_OS_NET_CHANNEL_TLS, out);
    if (!channel) return ESP_ERR_NO_MEM;
    const esp_err_t error = solar_os_tls_connect(host, port,
        timeout_ms ? timeout_ms : SOLAR_OS_NET_DEFAULT_CONNECT_TIMEOUT_MS,
        session->should_cancel, session->cancel_user, &channel->tls);
    if (error) {
        net_release_channel(channel);
        *out = 0;
    }
    return error;
}

esp_err_t solar_os_net_session_tls_send(solar_os_net_session_t *session, uint32_t id,
    const void *data, size_t length, uint32_t timeout_ms)
{
    solar_os_net_channel_t *channel = net_find_channel(session, id, SOLAR_OS_NET_CHANNEL_TLS);
    if (!channel || net_validate_timeout(timeout_ms) != ESP_OK ||
        net_validate_transfer(data, length, SOLAR_OS_NET_MAX_TRANSFER_BYTES) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    if (net_cancelled(session)) return ESP_ERR_INVALID_STATE;

    const int64_t deadline = net_deadline(timeout_ms);
    size_t offset = 0;
    while (offset < length) {
        size_t written;
        esp_err_t error = solar_os_tls_write(channel->tls, (const uint8_t *)data + offset, length - offset,
            (uint32_t)net_remaining_ms(deadline), session->should_cancel, session->cancel_user, &written);
        if (error || !written) {
            /* The caller cannot resume a partially submitted byte stream. */
            net_release_channel(channel);
            return error ? error : ESP_FAIL;
        }
        offset += written;
        if (offset < length && net_remaining_ms(deadline) == 0) {
            net_release_channel(channel);
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_net_session_tls_receive(solar_os_net_session_t *session, uint32_t id,
    void *buffer, size_t capacity, uint32_t timeout_ms, solar_os_net_receive_result_t *out)
{
    solar_os_net_channel_t *channel = net_find_channel(session, id, SOLAR_OS_NET_CHANNEL_TLS);
    if (!channel) return ESP_ERR_INVALID_ARG;
    esp_err_t error = net_prepare_receive(session, buffer, capacity, timeout_ms, out);
    if (error) return error;

    size_t received;
    error = solar_os_tls_read(channel->tls, buffer, capacity, timeout_ms,
        session->should_cancel, session->cancel_user, &received);
    if (error == ESP_ERR_TIMEOUT) {
        out->timed_out = true;
        return ESP_OK;
    }
    if (error) return error;
    out->data_len = received;
    out->message_len = received;
    out->closed = received == 0;
    return ESP_OK;
}

esp_err_t solar_os_net_session_close(solar_os_net_session_t *session,
                                     uint32_t handle)
{
    if (session == NULL || handle == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t adjusted = handle - 1U;
    const size_t index = adjusted % SOLAR_OS_NET_SESSION_MAX_CHANNELS;
    const uint32_t generation = adjusted / SOLAR_OS_NET_SESSION_MAX_CHANNELS + 1U;
    solar_os_net_channel_t *channel = &session->channels[index];
    if (!channel->open || channel->generation != generation) {
        return ESP_ERR_INVALID_ARG;
    }
    net_release_channel(channel);
    return ESP_OK;
}

void solar_os_net_session_close_all(solar_os_net_session_t *session)
{
    if (session == NULL) {
        return;
    }
    for (size_t i = 0; i < SOLAR_OS_NET_SESSION_MAX_CHANNELS; i++) {
        net_release_channel(&session->channels[i]);
    }
}

void solar_os_net_session_get_status(const solar_os_net_session_t *session,
                                     solar_os_net_session_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->session_limit = SOLAR_OS_NET_SESSION_MAX_CHANNELS;
    status->global_limit = SOLAR_OS_NET_GLOBAL_MAX_CHANNELS;
    if (session != NULL) {
        strlcpy(status->owner, session->owner, sizeof(status->owner));
        for (size_t i = 0; i < SOLAR_OS_NET_SESSION_MAX_CHANNELS; i++) {
            status->open_channels += session->channels[i].open ? 1U : 0U;
        }
    }
    portENTER_CRITICAL(&net_channel_lock);
    status->global_open_channels = net_global_open_channels;
    portEXIT_CRITICAL(&net_channel_lock);
}

const char *solar_os_net_ws_opcode_name(solar_os_net_ws_opcode_t opcode)
{
    switch (opcode) {
    case SOLAR_OS_NET_WS_CONTINUATION:
        return "continuation";
    case SOLAR_OS_NET_WS_TEXT:
        return "text";
    case SOLAR_OS_NET_WS_BINARY:
        return "binary";
    case SOLAR_OS_NET_WS_CLOSE:
        return "close";
    case SOLAR_OS_NET_WS_PING:
        return "ping";
    case SOLAR_OS_NET_WS_PONG:
        return "pong";
    default:
        return "unknown";
    }
}
