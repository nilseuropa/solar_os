#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "solar_os_tcp_stream.h"
#include "solar_os_stream.h"
#include "solar_os_port.h"

static unsigned leases, releases;
static bool latency_busy;
esp_err_t solar_os_wifi_latency_acquire(const char *owner)
{ assert(!strncmp(owner, "tcp:", 4)); if (latency_busy) return ESP_ERR_INVALID_STATE; leases++; return ESP_OK; }
esp_err_t solar_os_wifi_latency_release(const char *owner)
{ assert(!strncmp(owner, "tcp:", 4)); releases++; return ESP_OK; }
uint64_t solar_os_time_uptime_ms(void) { return 0; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *value) { *value = 0; return ESP_OK; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src), count = len < size ? len : size - 1;
    if (size) { memcpy(dst, src, count); dst[count] = 0; }
    return len;
}

static uint16_t unused_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (struct sockaddr *)&address, sizeof(address)));
    socklen_t length = sizeof(address);
    assert(!getsockname(fd, (struct sockaddr *)&address, &length));
    close(fd);
    return ntohs(address.sin_port);
}

static int connect_to(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!connect(fd, (struct sockaddr *)&address, sizeof(address)));
    return fd;
}

static void outbound(void)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (struct sockaddr *)&address, sizeof(address)) && !listen(listener, 2));
    socklen_t size = sizeof(address);
    assert(!getsockname(listener, (struct sockaddr *)&address, &size));
    uint16_t port = ntohs(address.sin_port);
    assert(solar_os_tcp_stream_connect("out", "localhost", 0) == ESP_ERR_INVALID_ARG);
    assert(solar_os_tcp_stream_connect("out", "localhost", port) == ESP_OK);
    assert(solar_os_tcp_stream_connect("out", "localhost", port) == ESP_ERR_INVALID_STATE);
    solar_os_stream_open_options_t options = {.direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX};
    solar_os_stream_handle_t handle = SOLAR_OS_STREAM_HANDLE_INIT;
    const uint8_t bytes[] = {0, 255, '\r', '\n', 27, 42};
    uint8_t data[sizeof(bytes)];
    size_t n = 0;
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        assert(solar_os_stream_open_ex("out", "test", &options, &handle) == ESP_OK);
        assert(solar_os_tcp_stream_remove("out") == ESP_ERR_INVALID_STATE);
        for (unsigned attempt = 0; attempt < 10000 && !n; attempt++)
            assert(solar_os_stream_write(&handle, bytes, sizeof(bytes), 0, &n) == ESP_OK);
        assert(n == sizeof(bytes));
        int peer = accept(listener, NULL, NULL);
        assert(peer >= 0 && recv(peer, data, sizeof(data), MSG_WAITALL) == sizeof(data));
        assert(!memcmp(data, bytes, sizeof(data)));
        assert(send(peer, bytes, sizeof(bytes), 0) == sizeof(bytes));
        n = 0;
        for (unsigned attempt = 0; attempt < 10000 && !n; attempt++) {
            esp_err_t error = solar_os_stream_read(&handle, data, sizeof(data), 0, &n);
            assert(error == ESP_OK || error == ESP_ERR_TIMEOUT);
        }
        assert(n == sizeof(bytes) && !memcmp(data, bytes, sizeof(data)));
        close(peer);
        assert(solar_os_stream_read(&handle, data, sizeof(data), 0, &n) == ESP_FAIL);
        assert(solar_os_stream_write(&handle, bytes, sizeof(bytes), 0, &n) == ESP_FAIL);
        assert(solar_os_stream_close_ex(&handle) == ESP_OK);
        n = 0;
    }
    assert(solar_os_tcp_stream_remove("out") == ESP_OK && leases == releases);
    close(listener);
    assert(solar_os_tcp_stream_connect("refused", "127.0.0.1", port) == ESP_OK);
    assert(solar_os_stream_open_ex("refused", "test", &options, &handle) == ESP_OK);
    esp_err_t error = ESP_OK;
    for (unsigned attempt = 0; attempt < 10000 && error != ESP_FAIL; attempt++)
        error = solar_os_stream_read(&handle, data, sizeof(data), 0, &n);
    assert(error == ESP_FAIL && !n);
    assert(solar_os_stream_close_ex(&handle) == ESP_OK);
    assert(solar_os_tcp_stream_remove("refused") == ESP_OK && leases == releases);
}

int main(void)
{
    uint16_t port = unused_port();
    assert(solar_os_tcp_stream_listen("tcp0", 0) == ESP_ERR_INVALID_ARG);
    assert(solar_os_tcp_stream_listen("bad.name", port) == ESP_ERR_INVALID_ARG);
    assert(solar_os_tcp_stream_listen("tcp0", port) == ESP_OK);
    assert(solar_os_tcp_stream_listen("tcp0", unused_port()) == ESP_ERR_INVALID_STATE);
    assert(solar_os_tcp_stream_listen("other", port) != ESP_OK);
    solar_os_stream_info_t info;
    assert(solar_os_stream_get_info("tcp0", &info) == ESP_OK);
    assert(info.type == SOLAR_OS_STREAM_TYPE_BYTES && info.direction == SOLAR_OS_STREAM_DIRECTION_DUPLEX &&
        info.sharing == SOLAR_OS_STREAM_SHARING_EXCLUSIVE);
    solar_os_stream_handle_t handle = SOLAR_OS_STREAM_HANDLE_INIT, duplicate = SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_stream_open_options_t options = {.direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX};
    assert(solar_os_stream_open_ex("tcp0", "test", &options, &handle) == ESP_OK);
    assert(solar_os_stream_open_ex("tcp0", "other", &options, &duplicate) == ESP_ERR_INVALID_STATE);
    assert(solar_os_tcp_stream_remove("tcp0") == ESP_ERR_INVALID_STATE);
    uint8_t sent[256], received[256];
    for (unsigned i = 0; i < sizeof(sent); i++) sent[i] = i;
    size_t count;
    assert(solar_os_stream_read(&handle, received, sizeof(received), 0, &count) == ESP_ERR_TIMEOUT && !count);
    latency_busy = true; /* Telnet can already own the latency lease. */
    int client = connect_to(port);
    assert(send(client, sent, sizeof(sent), 0) == sizeof(sent));
    size_t offset = 0;
    while (offset < sizeof(received)) {
        assert(solar_os_stream_read(&handle, received + offset, 7, 0, &count) == ESP_OK);
        assert(count && count <= 7);
        offset += count;
    }
    assert(!memcmp(sent, received, sizeof(sent)) && !leases);
    latency_busy = false;
    assert(solar_os_stream_write(&handle, sent, sizeof(sent), 0, &count) == ESP_OK && count == sizeof(sent));
    assert(recv(client, received, sizeof(received), MSG_WAITALL) == sizeof(received));
    assert(!memcmp(sent, received, sizeof(sent)) && leases == 1);
    assert(solar_os_stream_get_info("tcp0", &info) == ESP_OK &&
        info.read_units == 256 && info.written_units == 256);
    /* A receiver that stops reading must not block the owner task. */
    uint8_t block[8192] = {0};
    bool backpressure = false;
    for (unsigned i = 0; i < 4096; i++) {
        assert(solar_os_stream_write(&handle, block, sizeof(block), 0, &count) == ESP_OK);
        assert(count <= sizeof(block));
        if (!count) { backpressure = true; break; }
    }
    assert(backpressure);
    close(client);
    assert(solar_os_stream_read(&handle, received, sizeof(received), 0, &count) == ESP_FAIL);
    assert(leases == releases);
    client = connect_to(port);
    assert(solar_os_stream_read(&handle, received, sizeof(received), 0, &count) == ESP_FAIL);
    assert(solar_os_stream_close_ex(&handle) == ESP_OK);
    close(client);
    assert(solar_os_stream_open_ex("tcp0", "new-owner", &options, &handle) == ESP_OK);
    client = connect_to(port);
    assert(send(client, sent, sizeof(sent), 0) == sizeof(sent));
    assert(solar_os_stream_read(&handle, received, sizeof(received), 0, &count) == ESP_OK && count == sizeof(sent));
    assert(solar_os_stream_close_ex(&handle) == ESP_OK);
    close(client);
    assert(solar_os_tcp_stream_remove("tcp0") == ESP_OK);
    assert(solar_os_stream_get_info("tcp0", &info) == ESP_ERR_NOT_FOUND);
    assert(solar_os_tcp_stream_remove("tcp0") == ESP_ERR_NOT_FOUND);
    assert(leases == releases);
    /* Rebind and all endpoint slots remain usable after errors and removal. */
    for (unsigned i = 0; i < 4; i++) {
        char name[8]; snprintf(name, sizeof(name), "tcp%u", i);
        assert(solar_os_tcp_stream_listen(name, i ? unused_port() : port) == ESP_OK);
    }
    assert(solar_os_tcp_stream_listen("overflow", unused_port()) == ESP_ERR_NO_MEM);
    for (unsigned i = 0; i < 4; i++) {
        char name[8]; snprintf(name, sizeof(name), "tcp%u", i);
        assert(solar_os_tcp_stream_remove(name) == ESP_OK);
    }
    outbound();
    puts("Raw TCP listener/client, binary bytes, ownership, backpressure, EOF, reconnect, refusal and lease cleanup PASS");
    return 0;
}
