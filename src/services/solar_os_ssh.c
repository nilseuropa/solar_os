#include "solar_os_ssh.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "libssh2.h"
#include "solar_os_memory.h"
#include "solar_os_ssh_transport.h"
#include "solar_os_task.h"

#define SOLAR_OS_SSH_DEFAULT_PORT 22
#define SOLAR_OS_SSH_TASK_STACK 12288
#define SOLAR_OS_SSH_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define SOLAR_OS_SSH_EVENT_QUEUE_LEN 16
#define SOLAR_OS_SSH_TX_QUEUE_LEN 16
#define SOLAR_OS_SSH_TX_CHUNK_MAX 64
#define SOLAR_OS_SSH_TERM_TYPE "xterm-mono"
#define SOLAR_OS_SSH_INTERACTIVE_IDLE_MS 5

typedef struct {
    size_t len;
    char data[SOLAR_OS_SSH_TX_CHUNK_MAX];
} solar_os_ssh_tx_chunk_t;

struct solar_os_ssh_session {
    char host[SOLAR_OS_SSH_HOST_MAX];
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char password[SOLAR_OS_SSH_PASSWORD_MAX];
    uint16_t port;
    uint16_t cols;
    uint16_t rows;
    QueueHandle_t events;
    QueueHandle_t tx;
    StaticQueue_t *events_queue;
    uint8_t *events_storage;
    StaticQueue_t *tx_queue;
    uint8_t *tx_storage;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool task_done;
    volatile bool detached;
};

static const char *TAG = "solar_os_ssh";

static bool ssh_should_stop(const solar_os_ssh_session_t *session);
static void ssh_send_status(solar_os_ssh_session_t *session, const char *message);
static void ssh_send_error(solar_os_ssh_session_t *session, const char *message);

static bool ssh_transport_should_stop(void *user)
{
    return ssh_should_stop((const solar_os_ssh_session_t *)user);
}

static void ssh_transport_status(void *user, const char *message)
{
    ssh_send_status((solar_os_ssh_session_t *)user, message);
}

static void ssh_transport_error(void *user, const char *message)
{
    ssh_send_error((solar_os_ssh_session_t *)user, message);
}

static solar_os_ssh_transport_config_t ssh_transport_config(solar_os_ssh_session_t *session)
{
    return (solar_os_ssh_transport_config_t){
        .host = session->host,
        .port = session->port,
        .username = session->username,
        .password = session->password,
        .log_tag = TAG,
        .user = session,
        .should_stop = ssh_transport_should_stop,
        .status = ssh_transport_status,
        .error = ssh_transport_error,
        .report_publickey_success = true,
        .allow_unverified_host_key_without_storage = true,
    };
}

static void ssh_send_event(solar_os_ssh_session_t *session,
                           solar_os_ssh_event_type_t type,
                           const char *message,
                           const char *data,
                           size_t len)
{
    if (session == NULL || session->events == NULL) {
        return;
    }

    solar_os_ssh_event_t event = {
        .type = type,
    };

    if (message != NULL) {
        strlcpy(event.message, message, sizeof(event.message));
    }

    if (data != NULL && len > 0) {
        event.len = len < sizeof(event.data) ? len : sizeof(event.data);
        memcpy(event.data, data, event.len);
    }

    (void)xQueueSend(session->events, &event, pdMS_TO_TICKS(100));
}

static void ssh_send_status(solar_os_ssh_session_t *session, const char *message)
{
    ssh_send_event(session, SOLAR_OS_SSH_EVENT_STATUS, message, NULL, 0);
}

static void ssh_send_error(solar_os_ssh_session_t *session, const char *message)
{
    ssh_send_event(session, SOLAR_OS_SSH_EVENT_ERROR, message, NULL, 0);
}

static void ssh_send_libssh2_error(solar_os_ssh_session_t *session,
                                   LIBSSH2_SESSION *lib_session,
                                   const char *prefix,
                                   int code)
{
    solar_os_ssh_transport_config_t config = ssh_transport_config(session);
    solar_os_ssh_transport_send_libssh2_error(&config, lib_session, prefix, code);
}

static bool ssh_should_stop(const solar_os_ssh_session_t *session)
{
    return session == NULL || session->stop_requested;
}

static QueueHandle_t ssh_create_queue(UBaseType_t len,
                                      UBaseType_t item_size,
                                      StaticQueue_t **queue_out,
                                      uint8_t **storage_out)
{
    if (queue_out == NULL || storage_out == NULL || len == 0 || item_size == 0) {
        return NULL;
    }

    StaticQueue_t *queue = solar_os_memory_calloc(1,
                                                  sizeof(*queue),
                                                  SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
                                                  "ssh.queue");
    uint8_t *storage = solar_os_memory_calloc(len,
                                              item_size,
                                              SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                              "ssh.queue-data");

    if (queue == NULL || storage == NULL) {
        solar_os_memory_free(queue);
        solar_os_memory_free(storage);
        return NULL;
    }

    QueueHandle_t handle = xQueueCreateStatic(len, item_size, storage, queue);
    if (handle == NULL) {
        solar_os_memory_free(queue);
        solar_os_memory_free(storage);
        return NULL;
    }

    *queue_out = queue;
    *storage_out = storage;
    return handle;
}

static void ssh_session_destroy(solar_os_ssh_session_t *session)
{
    if (session == NULL) {
        return;
    }

    if (session->events != NULL) {
        vQueueDelete(session->events);
        session->events = NULL;
    }
    if (session->tx != NULL) {
        vQueueDelete(session->tx);
        session->tx = NULL;
    }
    solar_os_memory_free(session->events_storage);
    solar_os_memory_free(session->events_queue);
    solar_os_memory_free(session->tx_storage);
    solar_os_memory_free(session->tx_queue);
    memset(session->password, 0, sizeof(session->password));
    solar_os_memory_free(session);
}

static int ssh_wait_socket(solar_os_ssh_session_t *session,
                           int socket_fd,
                           LIBSSH2_SESSION *lib_session)
{
    solar_os_ssh_transport_config_t config = ssh_transport_config(session);
    return solar_os_ssh_transport_wait_socket(&config, socket_fd, lib_session);
}

static void ssh_idle_delay(void)
{
    TickType_t ticks = pdMS_TO_TICKS(SOLAR_OS_SSH_INTERACTIVE_IDLE_MS);
    if (ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

static void ssh_request_env(solar_os_ssh_session_t *session,
                            LIBSSH2_SESSION *lib_session,
                            LIBSSH2_CHANNEL *channel,
                            int socket_fd,
                            const char *name,
                            const char *value)
{
    int rc = 0;
    while (!ssh_should_stop(session)) {
        rc = libssh2_channel_setenv_ex(channel,
                                       name,
                                       (unsigned int)strlen(name),
                                       value,
                                       (unsigned int)strlen(value));
        if (rc != LIBSSH2_ERROR_EAGAIN) {
            break;
        }
        (void)ssh_wait_socket(session, socket_fd, lib_session);
    }
    if (ssh_should_stop(session)) {
        return;
    }
    if (rc != 0 && rc != LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED &&
        rc != LIBSSH2_ERROR_REQUEST_DENIED) {
        SOLAR_OS_LOGD(TAG, "remote env %s rejected: %d", name, rc);
    }
}

static LIBSSH2_CHANNEL *ssh_open_shell(solar_os_ssh_session_t *session,
                                       LIBSSH2_SESSION *lib_session,
                                       int socket_fd)
{
    ssh_send_status(session, "opening shell");
    LIBSSH2_CHANNEL *channel = NULL;
    while (!ssh_should_stop(session)) {
        channel = libssh2_channel_open_session(lib_session);
        if (channel != NULL) {
            break;
        }
        if (libssh2_session_last_errno(lib_session) != LIBSSH2_ERROR_EAGAIN) {
            ssh_send_libssh2_error(session, lib_session, "channel open failed", -1);
            return NULL;
        }
        (void)ssh_wait_socket(session, socket_fd, lib_session);
    }

    if (channel == NULL) {
        return NULL;
    }

    (void)libssh2_channel_handle_extended_data2(channel, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE);
    ssh_request_env(session, lib_session, channel, socket_fd, "NO_COLOR", "1");
    ssh_request_env(session, lib_session, channel, socket_fd, "CLICOLOR", "0");
    ssh_request_env(session, lib_session, channel, socket_fd, "CLICOLOR_FORCE", "0");

    int rc;
    while (!ssh_should_stop(session) &&
           (rc = libssh2_channel_request_pty_ex(channel,
                                                SOLAR_OS_SSH_TERM_TYPE,
                                                sizeof(SOLAR_OS_SSH_TERM_TYPE) - 1,
                                                NULL,
                                                0,
                                                session->cols > 0 ? session->cols : 80,
                                                session->rows > 0 ? session->rows : 24,
                                                0,
                                                0)) == LIBSSH2_ERROR_EAGAIN) {
        (void)ssh_wait_socket(session, socket_fd, lib_session);
    }
    if (ssh_should_stop(session) || rc != 0) {
        ssh_send_libssh2_error(session, lib_session, "PTY request failed", rc);
        libssh2_channel_free(channel);
        return NULL;
    }

    while (!ssh_should_stop(session) &&
           (rc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)ssh_wait_socket(session, socket_fd, lib_session);
    }
    if (ssh_should_stop(session) || rc != 0) {
        ssh_send_libssh2_error(session, lib_session, "shell request failed", rc);
        libssh2_channel_free(channel);
        return NULL;
    }

    return channel;
}

static bool ssh_write_channel(solar_os_ssh_session_t *session,
                              LIBSSH2_SESSION *lib_session,
                              LIBSSH2_CHANNEL *channel,
                              int socket_fd,
                              const char *data,
                              size_t len)
{
    size_t offset = 0;
    while (offset < len && !ssh_should_stop(session)) {
        const ssize_t written = libssh2_channel_write(channel, data + offset, len - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written == LIBSSH2_ERROR_EAGAIN) {
            (void)ssh_wait_socket(session, socket_fd, lib_session);
            continue;
        }

        ssh_send_libssh2_error(session, lib_session, "channel write failed", (int)written);
        return false;
    }

    return !ssh_should_stop(session);
}

static bool ssh_pump_channel(solar_os_ssh_session_t *session,
                             LIBSSH2_SESSION *lib_session,
                             LIBSSH2_CHANNEL *channel,
                             int socket_fd,
                             bool *had_activity)
{
    (void)lib_session;
    (void)socket_fd;
    char buffer[SOLAR_OS_SSH_EVENT_DATA_MAX];

    while (!ssh_should_stop(session)) {
        const ssize_t read_len = libssh2_channel_read(channel, buffer, sizeof(buffer));
        if (read_len > 0) {
            if (had_activity != NULL) {
                *had_activity = true;
            }
            ssh_send_event(session,
                           SOLAR_OS_SSH_EVENT_OUTPUT,
                           NULL,
                           buffer,
                           (size_t)read_len);
            continue;
        }
        if (read_len == LIBSSH2_ERROR_EAGAIN) {
            break;
        }
        if (read_len == 0 || libssh2_channel_eof(channel)) {
            return false;
        }

        ssh_send_libssh2_error(session, lib_session, "channel read failed", (int)read_len);
        return false;
    }

    return !ssh_should_stop(session);
}

static void ssh_session_task(void *arg)
{
    solar_os_ssh_session_t *session = (solar_os_ssh_session_t *)arg;
    solar_os_ssh_transport_t transport = {
        .socket_fd = -1,
    };
    solar_os_ssh_transport_config_t transport_config = ssh_transport_config(session);
    int socket_fd = -1;
    LIBSSH2_SESSION *lib_session = NULL;
    LIBSSH2_CHANNEL *channel = NULL;
    bool connected = false;

    if (solar_os_ssh_transport_open(&transport_config, &transport) != ESP_OK ||
        ssh_should_stop(session)) {
        goto done;
    }
    socket_fd = transport.socket_fd;
    lib_session = transport.session;

    channel = ssh_open_shell(session, lib_session, socket_fd);
    if (channel == NULL) {
        goto done;
    }

    connected = true;
    ssh_send_event(session, SOLAR_OS_SSH_EVENT_CONNECTED, "connected", NULL, 0);

    while (!ssh_should_stop(session)) {
        solar_os_ssh_tx_chunk_t tx;
        bool had_activity = false;
        while (xQueueReceive(session->tx, &tx, 0) == pdTRUE) {
            had_activity = true;
            if (!ssh_write_channel(session, lib_session, channel, socket_fd, tx.data, tx.len)) {
                goto done;
            }
        }

        if (!ssh_pump_channel(session, lib_session, channel, socket_fd, &had_activity)) {
            break;
        }
        if (!had_activity) {
            ssh_idle_delay();
        }
    }

done:
    if (channel != NULL) {
        if (session != NULL && session->stop_requested) {
            (void)libssh2_channel_send_eof(channel);
        }
        (void)libssh2_channel_close(channel);
        (void)libssh2_channel_free(channel);
    }
    solar_os_ssh_transport_close(&transport, "SolarOS shutdown");

    if (session != NULL) {
        ssh_send_event(session,
                       SOLAR_OS_SSH_EVENT_DISCONNECTED,
                       connected ? "disconnected" : "not connected",
                       NULL,
                       0);
        const bool detached = session->detached;
        session->task_done = true;
        if (detached) {
            ssh_session_destroy(session);
        }
    }

    UBaseType_t stack_free_words = uxTaskGetStackHighWaterMark(NULL);
    SOLAR_OS_LOGI(TAG,
                  "SSH task stopped stack_min_free=%u bytes",
                  (unsigned)stack_free_words);
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_ssh_start(const solar_os_ssh_config_t *config,
                             solar_os_ssh_session_t **session_out)
{
    if (config == NULL ||
        session_out == NULL ||
        config->host == NULL ||
        config->host[0] == '\0' ||
        config->username == NULL ||
        config->username[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_ssh_session_t *session = solar_os_memory_calloc(1,
                                                             sizeof(*session),
                                                             SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                             "ssh.session");
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(session->host, config->host, sizeof(session->host));
    strlcpy(session->username, config->username, sizeof(session->username));
    strlcpy(session->password, config->password != NULL ? config->password : "", sizeof(session->password));
    session->port = config->port > 0 ? config->port : SOLAR_OS_SSH_DEFAULT_PORT;
    session->cols = config->cols;
    session->rows = config->rows;

    session->events = ssh_create_queue(SOLAR_OS_SSH_EVENT_QUEUE_LEN,
                                       sizeof(solar_os_ssh_event_t),
                                       &session->events_queue,
                                       &session->events_storage);
    session->tx = ssh_create_queue(SOLAR_OS_SSH_TX_QUEUE_LEN,
                                   sizeof(solar_os_ssh_tx_chunk_t),
                                   &session->tx_queue,
                                   &session->tx_storage);
    if (session->events == NULL || session->tx == NULL) {
        solar_os_ssh_stop(session);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = solar_os_task_create_pinned_internal(
        ssh_session_task,
        "solar_os_ssh",
        SOLAR_OS_SSH_TASK_STACK,
        session,
        SOLAR_OS_SSH_TASK_PRIORITY,
        &session->task,
        tskNO_AFFINITY);
    if (created != pdPASS) {
        solar_os_ssh_stop(session);
        return ESP_ERR_NO_MEM;
    }

    *session_out = session;
    return ESP_OK;
}

bool solar_os_ssh_stop(solar_os_ssh_session_t *session)
{
    if (session == NULL) {
        return true;
    }

    session->stop_requested = true;
    if (!solar_os_task_wait_done(session->task,
                                 &session->task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        session->detached = true;
        SOLAR_OS_LOGW(TAG, "SSH task did not stop within %u ms; detached for worker cleanup",
                 (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return false;
    }

    ssh_session_destroy(session);
    return true;
}

esp_err_t solar_os_ssh_send(solar_os_ssh_session_t *session, const char *data, size_t len)
{
    if (session == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (session->tx == NULL || session->stop_requested || session->task_done) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t offset = 0;
    while (offset < len) {
        solar_os_ssh_tx_chunk_t chunk = {0};
        chunk.len = len - offset;
        if (chunk.len > sizeof(chunk.data)) {
            chunk.len = sizeof(chunk.data);
        }
        memcpy(chunk.data, data + offset, chunk.len);

        if (xQueueSend(session->tx, &chunk, 0) != pdTRUE) {
            return ESP_ERR_NO_MEM;
        }
        offset += chunk.len;
    }

    return ESP_OK;
}

bool solar_os_ssh_poll(solar_os_ssh_session_t *session, solar_os_ssh_event_t *event)
{
    if (session == NULL || event == NULL || session->events == NULL) {
        return false;
    }

    return xQueueReceive(session->events, event, 0) == pdTRUE;
}
