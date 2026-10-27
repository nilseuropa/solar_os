#include "solar_os_tcp_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "lwip/netdb.h"
#include "solar_os_port.h"
#include "solar_os_wifi.h"

#define TCP_STREAM_MAX 4

typedef struct {
    char name[SOLAR_OS_PORT_NAME_MAX];
    char latency_owner[SOLAR_OS_PORT_OWNER_MAX];
    int listener, peer;
    bool failed, latency_lease, outbound, connecting;
    struct sockaddr_in remote;
} tcp_stream_t;

static tcp_stream_t endpoints[TCP_STREAM_MAX];
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(mutex); }

static bool nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool would_wait(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void close_peer(tcp_stream_t *s)
{
    s->connecting = false;
    if (s->peer >= 0) {
        (void)shutdown(s->peer, SHUT_RDWR);
        close(s->peer);
        s->peer = -1;
    }
    if (s->latency_lease) {
        (void)solar_os_wifi_latency_release(s->latency_owner);
        s->latency_lease = false;
    }
}

/* Never replace a disconnected peer within an owner's protocol session. */
static esp_err_t accept_peer(tcp_stream_t *s)
{
    if (s->failed) return ESP_FAIL;
    if (s->outbound && s->peer < 0) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int enabled = 1;
        if (fd < 0) return ESP_FAIL;
        if (!nonblocking(fd) || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                &enabled, sizeof(enabled)) < 0) {
            close(fd); s->failed = true; return ESP_FAIL;
        }
        s->peer = fd;
        s->latency_lease = solar_os_wifi_latency_acquire(s->latency_owner) == ESP_OK;
        if (connect(fd, (struct sockaddr *)&s->remote, sizeof(s->remote)) < 0) {
            if (errno != EINPROGRESS && !would_wait()) {
                s->failed = true; close_peer(s); return ESP_FAIL;
            }
            s->connecting = true;
        }
    }
    if (s->connecting) {
        fd_set write_fds, error_fds;
        FD_ZERO(&write_fds); FD_ZERO(&error_fds);
        FD_SET(s->peer, &write_fds); FD_SET(s->peer, &error_fds);
        struct timeval timeout = {0};
        int ready = select(s->peer + 1, NULL, &write_fds, &error_fds, &timeout);
        if (!ready || (ready < 0 && would_wait())) return ESP_ERR_TIMEOUT;
        int error = 0;
        socklen_t size = sizeof(error);
        if (ready < 0 || getsockopt(s->peer, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
            s->failed = true; close_peer(s); return ESP_FAIL;
        }
        s->connecting = false;
    }
    if (s->peer >= 0) {
        /* Another owner (for example Telnet) may initially hold the lease. */
        if (!s->latency_lease)
            s->latency_lease = solar_os_wifi_latency_acquire(s->latency_owner) == ESP_OK;
        return ESP_OK;
    }
    int fd = accept(s->listener, NULL, NULL);
    if (fd < 0) return would_wait() ? ESP_ERR_TIMEOUT : ESP_FAIL;
    int enabled = 1;
    if (!nonblocking(fd) || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
            &enabled, sizeof(enabled)) < 0) {
        close(fd);
        s->failed = true;
        return ESP_FAIL;
    }
    s->peer = fd;
    s->latency_lease = solar_os_wifi_latency_acquire(s->latency_owner) == ESP_OK;
    return ESP_OK;
}

static esp_err_t open_stream(void *user)
{
    tcp_stream_t *s = user;
    lock();
    s->failed = false;
    unlock();
    return ESP_OK;
}

static esp_err_t close_stream(void *user)
{
    tcp_stream_t *s = user;
    lock();
    close_peer(s);
    /* Discard queued connections from the previous owner as well. */
    for (unsigned i = 0; s->listener >= 0 && i < 4; i++) {
        int fd = accept(s->listener, NULL, NULL);
        if (fd < 0) break;
        close(fd);
    }
    s->failed = false;
    unlock();
    return ESP_OK;
}

static esp_err_t read_stream(void *user, uint8_t *data, size_t len,
    uint32_t timeout_ms, size_t *read_len)
{
    (void)timeout_ms;
    tcp_stream_t *s = user;
    if (!read_len || (!data && len)) return ESP_ERR_INVALID_ARG;
    *read_len = 0;
    if (!len) return ESP_OK;
    lock();
    esp_err_t err = accept_peer(s);
    if (err == ESP_OK) {
        ssize_t n = recv(s->peer, data, len, 0);
        if (n > 0) *read_len = (size_t)n;
        else if (n < 0 && would_wait()) err = ESP_ERR_TIMEOUT;
        else {
            s->failed = true;
            close_peer(s);
            err = ESP_FAIL;
        }
    }
    unlock();
    return err;
}

static esp_err_t write_stream(void *user, const uint8_t *data, size_t len, size_t *written)
{
    tcp_stream_t *s = user;
    if (!written || (!data && len)) return ESP_ERR_INVALID_ARG;
    *written = 0;
    if (!len) return ESP_OK;
    lock();
    esp_err_t err = accept_peer(s);
    if (err == ESP_ERR_TIMEOUT) err = ESP_OK;
    else if (err == ESP_OK) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = send(s->peer, data, len, flags);
        if (n > 0) *written = (size_t)n;
        else if (!(n < 0 && would_wait())) {
            s->failed = true;
            close_peer(s);
            err = ESP_FAIL;
        }
    }
    unlock();
    return err;
}

esp_err_t solar_os_tcp_stream_listen(const char *name, uint16_t port)
{
    if (!name || !*name || strlen(name) >= SOLAR_OS_PORT_NAME_MAX || !port)
        return ESP_ERR_INVALID_ARG;
    lock();
    tcp_stream_t *s = NULL;
    for (size_t i = 0; i < TCP_STREAM_MAX; i++) {
        if (!strcmp(endpoints[i].name, name)) { unlock(); return ESP_ERR_INVALID_STATE; }
        if (!endpoints[i].name[0] && !s) s = &endpoints[i];
    }
    if (!s) { unlock(); return ESP_ERR_NO_MEM; }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { unlock(); return ESP_FAIL; }
    int enabled = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (!nonblocking(fd) || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        unlock();
        return ESP_FAIL;
    }
    *s = (tcp_stream_t){.listener = fd, .peer = -1};
    strlcpy(s->name, name, sizeof(s->name));
    snprintf(s->latency_owner, sizeof(s->latency_owner), "tcp:%s", name);
    char label[SOLAR_OS_PORT_LABEL_MAX];
    snprintf(label, sizeof(label), "Raw TCP listener :%u", (unsigned)port);
    solar_os_port_driver_t driver = {.name = s->name, .label = label,
        .capabilities = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE,
        .open = open_stream, .close = close_stream, .read = read_stream,
        .write = write_stream, .user = s};
    esp_err_t err = solar_os_port_register(&driver);
    if (err != ESP_OK) { close(fd); memset(s, 0, sizeof(*s)); }
    unlock();
    return err;
}

esp_err_t solar_os_tcp_stream_remove(const char *name)
{
    if (!name || !*name) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < TCP_STREAM_MAX; i++) {
        tcp_stream_t *s = &endpoints[i];
        if (strcmp(s->name, name)) continue;
        err = solar_os_port_unregister(name);
        if (err == ESP_OK) {
            close_peer(s);
            if (s->listener >= 0) close(s->listener);
            memset(s, 0, sizeof(*s));
        }
        break;
    }
    unlock();
    return err;
}

esp_err_t solar_os_tcp_stream_connect(const char *name, const char *host, uint16_t port)
{
    if (!name || !*name || strlen(name) >= SOLAR_OS_PORT_NAME_MAX ||
        !host || !*host || strlen(host) > 253 || !port) return ESP_ERR_INVALID_ARG;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *addresses = NULL;
    if (getaddrinfo(host, NULL, &hints, &addresses) != 0 || !addresses) return ESP_ERR_NOT_FOUND;
    struct sockaddr_in remote = *(const struct sockaddr_in *)addresses->ai_addr;
    freeaddrinfo(addresses);
    remote.sin_port = htons(port);
    lock();
    tcp_stream_t *s = NULL;
    for (size_t i = 0; i < TCP_STREAM_MAX; i++) {
        if (!strcmp(endpoints[i].name, name)) { unlock(); return ESP_ERR_INVALID_STATE; }
        if (!endpoints[i].name[0] && !s) s = &endpoints[i];
    }
    if (!s) { unlock(); return ESP_ERR_NO_MEM; }
    *s = (tcp_stream_t){.listener = -1, .peer = -1, .outbound = true, .remote = remote};
    strlcpy(s->name, name, sizeof(s->name));
    snprintf(s->latency_owner, sizeof(s->latency_owner), "tcp:%s", name);
    char label[SOLAR_OS_PORT_LABEL_MAX];
    snprintf(label, sizeof(label), "Raw TCP client :%u", (unsigned)port);
    solar_os_port_driver_t driver = {.name = s->name, .label = label,
        .capabilities = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE,
        .open = open_stream, .close = close_stream, .read = read_stream,
        .write = write_stream, .user = s};
    esp_err_t error = solar_os_port_register(&driver);
    if (error != ESP_OK) memset(s, 0, sizeof(*s));
    unlock();
    return error;
}
