#include "solar_os_osc_job.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "solar_os_controls.h"
#include "solar_os_jobs.h"
#include "solar_os_net.h"
#include "solar_os_osc.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"
#include "solar_os_time.h"

#define OSC_JOB_DEFAULT_LISTEN_PORT 9000U
#define OSC_JOB_POLL_MS 10U
#define OSC_JOB_WORKER_STACK 6144U
#define OSC_JOB_PACKET_RATE_MAX 100U
#define OSC_JOB_HOST_MAX 64U

typedef struct {
    bool running;
    volatile bool worker_done;
    TaskHandle_t worker_task;
    int socket_fd;
    uint16_t listen_port;
    char target_host[OSC_JOB_HOST_MAX];
    char target_ip[SOLAR_OS_NET_ADDR_MAX];
    uint16_t target_port;
    char peer[16];
    uint32_t inbound_packets;
    uint32_t inbound_filtered;
    uint32_t inbound_rate_limited;
    uint32_t inbound_applied;
    uint32_t inbound_unknown;
    uint32_t inbound_rejected;
    uint32_t inbound_malformed;
    uint32_t outbound_messages;
    uint32_t outbound_failures;
    uint32_t source_failures;
    esp_err_t last_error;
} osc_job_state_t;

static osc_job_state_t osc_job;

static bool osc_parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0U ||
        parsed > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)parsed;
    return true;
}

static bool osc_parse_target(const char *text,
                             char *host,
                             size_t host_len,
                             uint16_t *port)
{
    const char *colon = text != NULL ? strrchr(text, ':') : NULL;
    if (colon == NULL || colon == text || colon[1] == '\0' ||
        strchr(text, ':') != colon ||
        strchr(text, '[') != NULL || strchr(text, ']') != NULL ||
        strchr(colon + 1U, ':') != NULL) {
        return false;
    }
    const size_t length = (size_t)(colon - text);
    if (length >= host_len || !osc_parse_port(colon + 1U, port)) {
        return false;
    }
    memcpy(host, text, length);
    host[length] = '\0';
    return true;
}

static bool osc_parse_start(int argc,
                            char **argv,
                            uint16_t *listen_port,
                            char *target_host,
                            size_t target_host_len,
                            uint16_t *target_port,
                            char *peer,
                            size_t peer_len)
{
    *listen_port = OSC_JOB_DEFAULT_LISTEN_PORT;
    target_host[0] = '\0';
    *target_port = 0U;
    peer[0] = '\0';
    int first = argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], "osc") == 0 ? 1 : 0;
    for (int i = first; i < argc; i++) {
        if (argv[i] == NULL) {
            return false;
        }
        if (strncmp(argv[i], "listen=", 7U) == 0) {
            if (!osc_parse_port(argv[i] + 7U, listen_port)) {
                return false;
            }
        } else if (strncmp(argv[i], "target=", 7U) == 0) {
            if (target_host[0] != '\0' ||
                !osc_parse_target(argv[i] + 7U, target_host,
                                  target_host_len, target_port)) {
                return false;
            }
        } else if (strncmp(argv[i], "peer=", 5U) == 0) {
            struct in_addr address;
            if (peer[0] != '\0' || inet_pton(AF_INET, argv[i] + 5U, &address) != 1 ||
                inet_ntop(AF_INET, &address, peer, peer_len) == NULL) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

static esp_err_t osc_read_binding_value(const solar_os_osc_binding_info_t *binding,
                                        float *value)
{
    if (binding->config.source_type == SOLAR_OS_OSC_SOURCE_CONTROL) {
        solar_os_control_info_t control;
        const esp_err_t err = solar_os_control_find(binding->config.source, &control);
        if (err != ESP_OK) {
            return err;
        }
        if (!control.has_value) {
            return ESP_ERR_INVALID_STATE;
        }
        *value = (float)control.normalized /
            (float)SOLAR_OS_CONTROL_NORMALIZED_MAX;
        return ESP_OK;
    }

    solar_os_stream_info_t info;
    esp_err_t err = solar_os_stream_get_info(binding->config.source, &info);
    if (err != ESP_OK) {
        return err;
    }
    const solar_os_stream_type_t expected =
        binding->config.value_type == SOLAR_OS_OSC_VALUE_EVENT ?
            SOLAR_OS_STREAM_TYPE_EVENT : SOLAR_OS_STREAM_TYPE_SCALAR;
    if (info.type != expected) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_stream_handle_t handle =
        (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    err = solar_os_stream_open(binding->config.source, "osc", &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (expected == SOLAR_OS_STREAM_TYPE_SCALAR) {
        const solar_os_stream_read_options_t options = {
            .window_ms = binding->config.interval_ms,
        };
        err = solar_os_stream_read_scalar(&handle, &options, value);
    } else {
        uint8_t level = 0U;
        size_t read_len = 0U;
        err = solar_os_stream_read(&handle, &level, sizeof(level), 0U, &read_len);
        if (err == ESP_OK && read_len != sizeof(level)) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        *value = level != 0U ? 1.0f : 0.0f;
    }
    solar_os_stream_close(&handle);
    return err;
}

static void osc_send_binding(const solar_os_osc_binding_info_t *binding,
                             uint64_t now_ms,
                             float value,
                             bool send_integer)
{
    uint8_t packet[128];
    size_t packet_len = 0U;
    esp_err_t err = send_integer ?
        solar_os_osc_encode_int(binding->config.address,
                                value != 0.0f ? 1 : 0,
                                packet, sizeof(packet), &packet_len) :
        solar_os_osc_encode_float(binding->config.address, value,
                                  packet, sizeof(packet), &packet_len);
    if (err == ESP_OK && osc_job.target_host[0] == '\0') {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        const struct sockaddr_in target = {
            .sin_family = AF_INET,
            .sin_port = htons(osc_job.target_port),
            .sin_addr.s_addr = inet_addr(osc_job.target_ip),
        };
        const ssize_t sent = sendto(osc_job.socket_fd, packet, packet_len, 0,
                                    (const struct sockaddr *)&target,
                                    sizeof(target));
        err = sent == (ssize_t)packet_len ? ESP_OK : ESP_FAIL;
    }
    if (err == ESP_OK) {
        solar_os_osc_binding_note_sent(binding->id, now_ms);
        osc_job.outbound_messages++;
    } else {
        solar_os_osc_binding_note_error(binding->id, now_ms, err, true);
        osc_job.outbound_failures++;
        osc_job.last_error = err;
    }
}

static void osc_sample_bindings(uint64_t now_ms)
{
    const size_t count = solar_os_osc_binding_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_osc_binding_info_t binding;
        if (!solar_os_osc_binding_get(i, &binding) ||
            !solar_os_osc_binding_due(&binding, now_ms)) {
            continue;
        }
        float value = 0.0f;
        esp_err_t err = osc_read_binding_value(&binding, &value);
        if (err != ESP_OK) {
            solar_os_osc_binding_note_error(binding.id, now_ms, err, false);
            osc_job.source_failures++;
            continue;
        }
        bool send = false;
        bool send_integer = false;
        err = solar_os_osc_binding_prepare(binding.id, now_ms, value,
                                           &send, &send_integer);
        if (err == ESP_OK && send) {
            osc_send_binding(&binding, now_ms, value, send_integer);
        }
    }
}

static void osc_receive_packet(uint8_t *packet,
                               size_t packet_len,
                               bool truncated,
                               const char *source_address,
                               uint64_t now_ms,
                               uint64_t *window_second,
                               uint32_t *window_packets)
{
    osc_job.inbound_packets++;
    if (osc_job.peer[0] != '\0' && strcmp(osc_job.peer, source_address) != 0) {
        osc_job.inbound_filtered++;
        return;
    }
    const uint64_t second = now_ms / 1000U;
    if (*window_second != second) {
        *window_second = second;
        *window_packets = 0U;
    }
    if ((*window_packets)++ >= OSC_JOB_PACKET_RATE_MAX) {
        osc_job.inbound_rate_limited++;
        return;
    }
    solar_os_osc_dispatch_result_t result = {0};
    const esp_err_t err = truncated ? ESP_ERR_INVALID_SIZE :
        solar_os_osc_dispatch_packet(packet, packet_len, &result);
    osc_job.inbound_applied += result.applied;
    osc_job.inbound_unknown += result.unknown_paths;
    osc_job.inbound_rejected += result.rejected_values;
    if (err != ESP_OK) {
        osc_job.inbound_malformed++;
        osc_job.last_error = err;
        return;
    }
}

static void osc_job_worker(void *arg)
{
    (void)arg;
    uint8_t packet[SOLAR_OS_OSC_PACKET_MAX];
    uint64_t window_second = UINT64_MAX;
    uint32_t window_packets = 0U;
    while (osc_job.running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(osc_job.socket_fd, &read_fds);
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = OSC_JOB_POLL_MS * 1000U,
        };
        const int ready = select(osc_job.socket_fd + 1, &read_fds, NULL, NULL,
                                 &timeout);
        const uint64_t now_ms = solar_os_time_uptime_ms();
        if (ready > 0 && FD_ISSET(osc_job.socket_fd, &read_fds)) {
            struct sockaddr_in source;
            socklen_t source_len = sizeof(source);
            const ssize_t received = recvfrom(
                osc_job.socket_fd, packet, sizeof(packet), MSG_TRUNC,
                (struct sockaddr *)&source, &source_len);
            if (received > 0) {
                char source_address[16];
                if (inet_ntop(AF_INET, &source.sin_addr, source_address,
                              sizeof(source_address)) == NULL) {
                    strlcpy(source_address, "0.0.0.0", sizeof(source_address));
                }
                const size_t packet_len = (size_t)received > sizeof(packet) ?
                    sizeof(packet) : (size_t)received;
                osc_receive_packet(packet, packet_len,
                                   (size_t)received > sizeof(packet),
                                   source_address, now_ms,
                                   &window_second, &window_packets);
            } else if (received < 0 && osc_job.running) {
                osc_job.last_error = ESP_FAIL;
            }
        } else if (ready < 0 && osc_job.running) {
            osc_job.last_error = ESP_FAIL;
        }
        if (osc_job.running) {
            osc_sample_bindings(now_ms);
        }
    }
    (void)close(osc_job.socket_fd);
    osc_job.socket_fd = -1;
    osc_job.worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t osc_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    uint16_t listen_port = 0U;
    uint16_t target_port = 0U;
    char target_host[OSC_JOB_HOST_MAX];
    char peer[16];
    if (!osc_parse_start(argc, argv, &listen_port, target_host,
                         sizeof(target_host), &target_port, peer, sizeof(peer))) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&osc_job, 0, sizeof(osc_job));
    osc_job.socket_fd = -1;
    osc_job.running = true;
    osc_job.listen_port = listen_port;
    osc_job.target_port = target_port;
    strlcpy(osc_job.target_host, target_host, sizeof(osc_job.target_host));
    strlcpy(osc_job.peer, peer, sizeof(osc_job.peer));
    esp_err_t err = ESP_OK;
    if (target_host[0] != '\0') {
        err = solar_os_net_resolve_host(target_host, osc_job.target_ip,
                                        sizeof(osc_job.target_ip));
    }
    if (err == ESP_OK) {
        osc_job.socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        err = osc_job.socket_fd >= 0 ? ESP_OK : ESP_FAIL;
    }
    if (err == ESP_OK) {
        const struct sockaddr_in local = {
            .sin_family = AF_INET,
            .sin_port = htons(listen_port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        err = bind(osc_job.socket_fd, (const struct sockaddr *)&local,
                   sizeof(local)) == 0 ? ESP_OK : ESP_FAIL;
    }
    if (err != ESP_OK) {
        if (osc_job.socket_fd >= 0) {
            (void)close(osc_job.socket_fd);
        }
        memset(&osc_job, 0, sizeof(osc_job));
        return err;
    }
    if (solar_os_task_create_pinned_internal(
            osc_job_worker, "osc_worker", OSC_JOB_WORKER_STACK, NULL,
            tskIDLE_PRIORITY + 1, &osc_job.worker_task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        (void)close(osc_job.socket_fd);
        memset(&osc_job, 0, sizeof(osc_job));
        return ESP_ERR_NO_MEM;
    }
    char resource[SOLAR_OS_JOB_RESOURCE_NAME_MAX];
    snprintf(resource, sizeof(resource), "udp:%u", (unsigned)listen_port);
    (void)solar_os_jobs_note_resource("osc", SOLAR_OS_JOB_RESOURCE_NET,
                                      resource, "OSC listen");
    if (target_host[0] != '\0') {
        snprintf(resource, sizeof(resource), "%s:%u", target_host,
                 (unsigned)target_port);
        (void)solar_os_jobs_note_resource("osc", SOLAR_OS_JOB_RESOURCE_NET,
                                          resource, "OSC target");
    }
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io != NULL) {
        solar_os_shell_io_printf(io, "OSC listening on UDP %u%s%s\n",
                                 (unsigned)listen_port,
                                 peer[0] != '\0' ? "; peer " : "",
                                 peer);
    }
    return ESP_OK;
}

static void osc_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (!osc_job.running && osc_job.worker_task == NULL) {
        return;
    }
    osc_job.running = false;
    if (!solar_os_task_wait_done(osc_job.worker_task, &osc_job.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return;
    }
    osc_job.worker_task = NULL;
    osc_job.worker_done = false;
}

void solar_os_osc_job_print_status(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (osc_job.target_host[0] != '\0') {
        solar_os_shell_io_printf(io,
            "  OSC: listen=%u target=%s:%u peer=%s\n",
            (unsigned)osc_job.listen_port, osc_job.target_host,
            (unsigned)osc_job.target_port,
            osc_job.peer[0] != '\0' ? osc_job.peer : "any");
    } else {
        solar_os_shell_io_printf(io,
            "  OSC: listen=%u target=- peer=%s\n",
            (unsigned)osc_job.listen_port,
            osc_job.peer[0] != '\0' ? osc_job.peer : "any");
    }
    solar_os_shell_io_printf(io,
        "  inbound: packets=%u applied=%u unknown=%u rejected=%u malformed=%u filtered=%u limited=%u\n",
        (unsigned)osc_job.inbound_packets, (unsigned)osc_job.inbound_applied,
        (unsigned)osc_job.inbound_unknown, (unsigned)osc_job.inbound_rejected,
        (unsigned)osc_job.inbound_malformed, (unsigned)osc_job.inbound_filtered,
        (unsigned)osc_job.inbound_rate_limited);
    solar_os_shell_io_printf(io,
        "  outbound: sent=%u dropped=%u source-errors=%u bindings=%u\n",
        (unsigned)osc_job.outbound_messages,
        (unsigned)osc_job.outbound_failures,
        (unsigned)osc_job.source_failures,
        (unsigned)solar_os_osc_binding_count());
    const size_t count = solar_os_osc_binding_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_osc_binding_info_t binding;
        if (!solar_os_osc_binding_get(i, &binding)) {
            continue;
        }
        if (binding.has_value) {
            solar_os_shell_io_printf(
                io, "  binding %s: %s %s value=%.6g sent=%u last=%llums error=%s\n",
                binding.config.name,
                binding.source_available ? "ready" : "missing",
                binding.config.source, (double)binding.last_value,
                (unsigned)binding.sent,
                (unsigned long long)binding.last_send_ms,
                solar_os_shell_error_text(binding.last_error));
        } else {
            solar_os_shell_io_printf(
                io, "  binding %s: %s %s value=- sent=%u last=%llums error=%s\n",
                binding.config.name,
                binding.source_available ? "ready" : "missing",
                binding.config.source, (unsigned)binding.sent,
                (unsigned long long)binding.last_send_ms,
                solar_os_shell_error_text(binding.last_error));
        }
    }
}

const solar_os_job_t solar_os_osc_job = {
    .name = "osc",
    .summary = "OSC parameter control and outbound bindings",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = osc_job_start,
    .stop = osc_job_stop,
    .worker_stack_bytes = OSC_JOB_WORKER_STACK,
    .detail = solar_os_osc_job_print_status,
};
