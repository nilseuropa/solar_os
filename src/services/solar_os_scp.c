#include "solar_os_scp.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "libssh2.h"
#include "solar_os_memory.h"
#include "solar_os_ssh_transport.h"
#include "solar_os_task.h"

#define SOLAR_OS_SCP_DEFAULT_PORT 22
#define SOLAR_OS_SCP_TASK_STACK 24576
#define SOLAR_OS_SCP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define SOLAR_OS_SCP_EVENT_QUEUE_LEN 16
#define SOLAR_OS_SCP_BUFFER_SIZE 2048
#define SOLAR_OS_SCP_PROGRESS_STEP 4096
#define SOLAR_OS_SCP_PROTOCOL_LINE_MAX 256
#define SOLAR_OS_SCP_COMMAND_MAX (SOLAR_OS_STORAGE_PATH_MAX * 6 + 16)

struct solar_os_scp_session {
    solar_os_scp_direction_t direction;
    char host[SOLAR_OS_SSH_HOST_MAX];
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char password[SOLAR_OS_SSH_PASSWORD_MAX];
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    char remote_path[SOLAR_OS_STORAGE_PATH_MAX];
    uint16_t port;
    bool remote_glob;
    QueueHandle_t events;
    TaskHandle_t task;
    volatile bool stop_requested;
    volatile bool task_done;
    volatile bool detached;
};

static const char *TAG = "solar_os_scp";

static bool scp_should_stop(const solar_os_scp_session_t *session);
static void scp_send_status(solar_os_scp_session_t *session, const char *message);
static void scp_send_error(solar_os_scp_session_t *session, const char *message);

static const char *scp_direction_name(solar_os_scp_direction_t direction)
{
    return direction == SOLAR_OS_SCP_UPLOAD ? "upload" : "download";
}

static bool scp_transport_should_stop(void *user)
{
    return scp_should_stop((const solar_os_scp_session_t *)user);
}

static void scp_transport_status(void *user, const char *message)
{
    scp_send_status((solar_os_scp_session_t *)user, message);
}

static void scp_transport_error(void *user, const char *message)
{
    scp_send_error((solar_os_scp_session_t *)user, message);
}

static solar_os_ssh_transport_config_t scp_transport_config(solar_os_scp_session_t *session)
{
    return (solar_os_ssh_transport_config_t){
        .host = session->host,
        .port = session->port,
        .username = session->username,
        .password = session->password,
        .log_tag = TAG,
        .user = session,
        .should_stop = scp_transport_should_stop,
        .status = scp_transport_status,
        .error = scp_transport_error,
        .include_username_in_auth_status = true,
        .report_password_success = true,
        .report_publickey_success = true,
        .include_error_code = true,
        .log_resolve = true,
        .log_connect_fail = true,
        .log_handshake_complete = true,
        .log_host_key_match = true,
        .log_key_paths = true,
        .allow_unverified_host_key_without_storage = true,
    };
}

static void scp_send_event(solar_os_scp_session_t *session,
                           solar_os_scp_event_type_t type,
                           const char *message,
                           uint64_t transferred,
                           uint64_t total)
{
    if (session == NULL || session->events == NULL) {
        return;
    }

    solar_os_scp_event_t event = {
        .type = type,
        .transferred = transferred,
        .total = total,
    };
    if (message != NULL) {
        strlcpy(event.message, message, sizeof(event.message));
    }

    if (type == SOLAR_OS_SCP_EVENT_STATUS && event.message[0] != '\0') {
        SOLAR_OS_LOGI(TAG, "%s", event.message);
    } else if (type == SOLAR_OS_SCP_EVENT_ERROR && event.message[0] != '\0') {
        SOLAR_OS_LOGE(TAG, "%s", event.message);
    } else if (type == SOLAR_OS_SCP_EVENT_DONE && event.message[0] != '\0') {
        SOLAR_OS_LOGI(TAG, "done: %s", event.message);
    }

    (void)xQueueSend(session->events, &event, pdMS_TO_TICKS(50));
}

static void scp_send_status(solar_os_scp_session_t *session, const char *message)
{
    scp_send_event(session, SOLAR_OS_SCP_EVENT_STATUS, message, 0, 0);
}

static void scp_send_progress(solar_os_scp_session_t *session,
                              uint64_t transferred,
                              uint64_t total)
{
    scp_send_event(session, SOLAR_OS_SCP_EVENT_PROGRESS, NULL, transferred, total);
}

static void scp_send_error(solar_os_scp_session_t *session, const char *message)
{
    scp_send_event(session, SOLAR_OS_SCP_EVENT_ERROR, message, 0, 0);
}

static void scp_send_libssh2_error(solar_os_scp_session_t *session,
                                   LIBSSH2_SESSION *lib_session,
                                   const char *prefix,
                                   int code)
{
    solar_os_ssh_transport_config_t config = scp_transport_config(session);
    solar_os_ssh_transport_send_libssh2_error(&config, lib_session, prefix, code);
}

static bool scp_should_stop(const solar_os_scp_session_t *session)
{
    return session == NULL || session->stop_requested;
}

static void scp_session_destroy(solar_os_scp_session_t *session)
{
    if (session == NULL) {
        return;
    }

    if (session->events != NULL) {
        vQueueDelete(session->events);
        session->events = NULL;
    }
    memset(session->password, 0, sizeof(session->password));
    solar_os_memory_free(session);
}

static int scp_wait_socket(solar_os_scp_session_t *session,
                           int socket_fd,
                           LIBSSH2_SESSION *lib_session)
{
    solar_os_ssh_transport_config_t config = scp_transport_config(session);
    return solar_os_ssh_transport_wait_socket(&config, socket_fd, lib_session);
}

static LIBSSH2_CHANNEL *scp_send_channel_open(solar_os_scp_session_t *session,
                                              LIBSSH2_SESSION *lib_session,
                                              int socket_fd,
                                              int mode,
                                              uint64_t size)
{
    LIBSSH2_CHANNEL *channel = NULL;
    scp_send_status(session, "opening remote file");

    while (!scp_should_stop(session)) {
        channel = libssh2_scp_send64(lib_session,
                                     session->remote_path,
                                     mode & 0777,
                                     (libssh2_int64_t)size,
                                     0,
                                     0);
        if (channel != NULL) {
            return channel;
        }
        if (libssh2_session_last_errno(lib_session) != LIBSSH2_ERROR_EAGAIN) {
            scp_send_libssh2_error(session, lib_session, "remote open failed", -1);
            return NULL;
        }
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }

    return NULL;
}

static LIBSSH2_CHANNEL *scp_recv_channel_open(solar_os_scp_session_t *session,
                                              LIBSSH2_SESSION *lib_session,
                                              int socket_fd,
                                              libssh2_struct_stat *fileinfo)
{
    LIBSSH2_CHANNEL *channel = NULL;
    scp_send_status(session, "opening remote file");

    while (!scp_should_stop(session)) {
        channel = libssh2_scp_recv2(lib_session, session->remote_path, fileinfo);
        if (channel != NULL) {
            return channel;
        }
        if (libssh2_session_last_errno(lib_session) != LIBSSH2_ERROR_EAGAIN) {
            scp_send_libssh2_error(session, lib_session, "remote open failed", -1);
            return NULL;
        }
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }

    return NULL;
}

static bool scp_channel_write_all(solar_os_scp_session_t *session,
                                  LIBSSH2_SESSION *lib_session,
                                  LIBSSH2_CHANNEL *channel,
                                  int socket_fd,
                                  const char *data,
                                  size_t len)
{
    size_t offset = 0;
    while (offset < len && !scp_should_stop(session)) {
        const ssize_t written = libssh2_channel_write(channel, data + offset, len - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written == LIBSSH2_ERROR_EAGAIN) {
            (void)scp_wait_socket(session, socket_fd, lib_session);
            continue;
        }

        scp_send_libssh2_error(session, lib_session, "channel write failed", (int)written);
        return false;
    }

    return !scp_should_stop(session);
}

static bool scp_append_text(char *out, size_t out_len, size_t *pos, const char *text)
{
    const size_t len = strlen(text);
    if (*pos + len >= out_len) {
        return false;
    }
    memcpy(out + *pos, text, len);
    *pos += len;
    out[*pos] = '\0';
    return true;
}

static bool scp_append_char(char *out, size_t out_len, size_t *pos, char ch)
{
    if (*pos + 1 >= out_len) {
        return false;
    }
    out[(*pos)++] = ch;
    out[*pos] = '\0';
    return true;
}

static bool scp_remote_path_has_wildcards(const char *path)
{
    return path != NULL && (strchr(path, '*') != NULL || strchr(path, '?') != NULL);
}

static bool scp_quote_remote_glob_path(const char *path, char *out, size_t out_len)
{
    if (path == NULL || path[0] == '\0' || out == NULL || out_len == 0) {
        return false;
    }

    size_t pos = 0;
    bool in_quote = false;
    out[0] = '\0';

    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        const char ch = *cursor;
        if (ch == '*' || ch == '?') {
            if (in_quote) {
                if (!scp_append_char(out, out_len, &pos, '\'')) {
                    return false;
                }
                in_quote = false;
            }
            if (!scp_append_char(out, out_len, &pos, ch)) {
                return false;
            }
            continue;
        }

        if (!in_quote) {
            if (!scp_append_char(out, out_len, &pos, '\'')) {
                return false;
            }
            in_quote = true;
        }
        if (ch == '\'') {
            if (!scp_append_text(out, out_len, &pos, "'\\''")) {
                return false;
            }
        } else if (!scp_append_char(out, out_len, &pos, ch)) {
            return false;
        }
    }

    if (in_quote && !scp_append_char(out, out_len, &pos, '\'')) {
        return false;
    }
    return pos > 0;
}

static bool scp_build_remote_glob_command(const char *path, char *command, size_t command_len)
{
    char quoted[SOLAR_OS_SCP_COMMAND_MAX];
    if (!scp_quote_remote_glob_path(path, quoted, sizeof(quoted))) {
        return false;
    }

    const int written = snprintf(command, command_len, "scp -f %s", quoted);
    return written >= 0 && (size_t)written < command_len;
}

static LIBSSH2_CHANNEL *scp_exec_channel_open(solar_os_scp_session_t *session,
                                              LIBSSH2_SESSION *lib_session,
                                              int socket_fd,
                                              const char *command,
                                              const char *status)
{
    LIBSSH2_CHANNEL *channel = NULL;
    scp_send_status(session, status);

    while (!scp_should_stop(session)) {
        channel = libssh2_channel_open_session(lib_session);
        if (channel != NULL) {
            break;
        }
        if (libssh2_session_last_errno(lib_session) != LIBSSH2_ERROR_EAGAIN) {
            scp_send_libssh2_error(session, lib_session, "channel open failed", -1);
            return NULL;
        }
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }

    if (channel == NULL) {
        return NULL;
    }

    int rc = 0;
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_exec(channel, command)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }
    if (scp_should_stop(session)) {
        (void)libssh2_channel_free(channel);
        return NULL;
    }
    if (rc != 0) {
        scp_send_libssh2_error(session, lib_session, "remote scp exec failed", rc);
        (void)libssh2_channel_free(channel);
        return NULL;
    }

    return channel;
}

static bool scp_channel_read_byte(solar_os_scp_session_t *session,
                                  LIBSSH2_SESSION *lib_session,
                                  LIBSSH2_CHANNEL *channel,
                                  int socket_fd,
                                  char *out,
                                  bool *eof)
{
    if (eof != NULL) {
        *eof = false;
    }

    while (!scp_should_stop(session)) {
        const ssize_t read_len = libssh2_channel_read(channel, out, 1);
        if (read_len == 1) {
            return true;
        }
        if (read_len == LIBSSH2_ERROR_EAGAIN) {
            (void)scp_wait_socket(session, socket_fd, lib_session);
            continue;
        }
        if (read_len == 0) {
            if (libssh2_channel_eof(channel)) {
                if (eof != NULL) {
                    *eof = true;
                }
                return false;
            }
            (void)scp_wait_socket(session, socket_fd, lib_session);
            continue;
        }

        scp_send_libssh2_error(session, lib_session, "channel read failed", (int)read_len);
        return false;
    }

    return false;
}

static bool scp_channel_read_line(solar_os_scp_session_t *session,
                                  LIBSSH2_SESSION *lib_session,
                                  LIBSSH2_CHANNEL *channel,
                                  int socket_fd,
                                  char *line,
                                  size_t line_len,
                                  bool *eof)
{
    if (line == NULL || line_len == 0) {
        return false;
    }
    if (eof != NULL) {
        *eof = false;
    }

    size_t len = 0;
    while (len + 1 < line_len) {
        char ch = '\0';
        bool hit_eof = false;
        if (!scp_channel_read_byte(session, lib_session, channel, socket_fd, &ch, &hit_eof)) {
            if (hit_eof && len == 0) {
                if (eof != NULL) {
                    *eof = true;
                }
            } else if (!scp_should_stop(session)) {
                scp_send_error(session, "unexpected remote EOF");
            }
            return false;
        }

        line[len++] = ch;
        if (ch == '\n') {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                len--;
            }
            line[len] = '\0';
            return true;
        }
    }

    line[len] = '\0';
    scp_send_error(session, "remote response too long");
    return false;
}

static bool scp_read_remote_message(solar_os_scp_session_t *session,
                                    LIBSSH2_SESSION *lib_session,
                                    LIBSSH2_CHANNEL *channel,
                                    int socket_fd,
                                    char *message,
                                    size_t message_len)
{
    if (message == NULL || message_len == 0) {
        return false;
    }

    size_t len = 0;
    while (len + 1 < message_len) {
        char ch = '\0';
        bool eof = false;
        if (!scp_channel_read_byte(session, lib_session, channel, socket_fd, &ch, &eof)) {
            break;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            message[len++] = ch;
        }
    }

    message[len] = '\0';
    return true;
}

static bool scp_report_remote_status(solar_os_scp_session_t *session,
                                     LIBSSH2_SESSION *lib_session,
                                     LIBSSH2_CHANNEL *channel,
                                     int socket_fd,
                                     const char *fallback)
{
    char message[SOLAR_OS_SCP_EVENT_MESSAGE_MAX];
    if (!scp_read_remote_message(session,
                                 lib_session,
                                 channel,
                                 socket_fd,
                                 message,
                                 sizeof(message))) {
        strlcpy(message, fallback, sizeof(message));
    }
    scp_send_error(session, message[0] != '\0' ? message : fallback);
    return false;
}

static bool scp_write_protocol_byte(solar_os_scp_session_t *session,
                                    LIBSSH2_SESSION *lib_session,
                                    LIBSSH2_CHANNEL *channel,
                                    int socket_fd,
                                    char code)
{
    return scp_channel_write_all(session, lib_session, channel, socket_fd, &code, 1);
}

static void scp_write_protocol_fatal(solar_os_scp_session_t *session,
                                     LIBSSH2_SESSION *lib_session,
                                     LIBSSH2_CHANNEL *channel,
                                     int socket_fd,
                                     const char *message)
{
    char response[SOLAR_OS_SCP_EVENT_MESSAGE_MAX + 2];
    response[0] = '\2';
    const int written = snprintf(response + 1, sizeof(response) - 1, "%s\n", message);
    if (written > 0) {
        (void)scp_channel_write_all(session,
                                    lib_session,
                                    channel,
                                    socket_fd,
                                    response,
                                    (size_t)written + 1);
    }
}

static bool scp_parse_file_header(char *line, int *mode, uint64_t *size, char **name)
{
    if (line == NULL || line[0] != 'C' || mode == NULL || size == NULL || name == NULL) {
        return false;
    }

    char *mode_text = line + 1;
    char *space = strchr(mode_text, ' ');
    if (space == NULL || space == mode_text) {
        return false;
    }
    *space = '\0';

    errno = 0;
    char *end = NULL;
    const long parsed_mode = strtol(mode_text, &end, 8);
    if (errno != 0 || end == mode_text || *end != '\0' || parsed_mode < 0) {
        return false;
    }

    char *size_text = space + 1;
    space = strchr(size_text, ' ');
    if (space == NULL || space == size_text || size_text[0] == '-') {
        return false;
    }
    *space = '\0';

    errno = 0;
    const unsigned long long parsed_size = strtoull(size_text, &end, 10);
    if (errno != 0 || end == size_text || *end != '\0') {
        return false;
    }

    char *filename = space + 1;
    if (filename[0] == '\0') {
        return false;
    }

    *mode = (int)parsed_mode;
    *size = (uint64_t)parsed_size;
    *name = filename;
    return true;
}

static bool scp_remote_filename_is_safe(const char *name)
{
    return name != NULL &&
        name[0] != '\0' &&
        strcmp(name, ".") != 0 &&
        strcmp(name, "..") != 0 &&
        strchr(name, '/') == NULL &&
        strchr(name, '\\') == NULL;
}

static bool scp_join_local_path(char *out, size_t out_len, const char *dir, const char *name)
{
    if (out == NULL || dir == NULL || name == NULL || out_len == 0) {
        return false;
    }

    const size_t dir_len = strlen(dir);
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s%s",
                                 dir,
                                 dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/",
                                 name);
    return written >= 0 && (size_t)written < out_len;
}

static void scp_close_channel(solar_os_scp_session_t *session,
                              LIBSSH2_SESSION *lib_session,
                              LIBSSH2_CHANNEL *channel,
                              int socket_fd)
{
    if (channel == NULL) {
        return;
    }

    int rc;
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_send_eof(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_wait_eof(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }
    while (!scp_should_stop(session) &&
           (rc = libssh2_channel_wait_closed(channel)) == LIBSSH2_ERROR_EAGAIN) {
        (void)scp_wait_socket(session, socket_fd, lib_session);
    }

    (void)libssh2_channel_free(channel);
}

static char *scp_alloc_buffer(void)
{
    return solar_os_memory_alloc(SOLAR_OS_SCP_BUFFER_SIZE,
                                 SOLAR_OS_MEMORY_TRANSIENT,
                                 "scp.buffer");
}

static esp_err_t scp_upload(solar_os_scp_session_t *session,
                            LIBSSH2_SESSION *lib_session,
                            int socket_fd)
{
    struct stat st;
    if (stat(session->local_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        SOLAR_OS_LOGE(TAG, "local file unavailable: %s errno=%d", session->local_path, errno);
        scp_send_error(session, "local file unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(session->local_path, "rb");
    if (file == NULL) {
        SOLAR_OS_LOGE(TAG, "local file open failed: %s errno=%d", session->local_path, errno);
        scp_send_error(session, "local file open failed");
        return ESP_FAIL;
    }

    const uint64_t total = st.st_size > 0 ? (uint64_t)st.st_size : 0ULL;
    SOLAR_OS_LOGI(TAG,
             "upload open local=%s remote=%s bytes=%" PRIu64,
             session->local_path,
             session->remote_path,
             total);
    LIBSSH2_CHANNEL *channel =
        scp_send_channel_open(session, lib_session, socket_fd, st.st_mode, total);
    if (channel == NULL) {
        fclose(file);
        return ESP_FAIL;
    }

    char *buffer = scp_alloc_buffer();
    if (buffer == NULL) {
        scp_close_channel(session, lib_session, channel, socket_fd);
        fclose(file);
        scp_send_error(session, "transfer buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    scp_send_status(session, "uploading");
    uint64_t transferred = 0;
    uint64_t next_progress = 0;
    esp_err_t ret = ESP_OK;

    while (!scp_should_stop(session)) {
        const size_t read_len = fread(buffer, 1, SOLAR_OS_SCP_BUFFER_SIZE, file);
        if (read_len > 0) {
            if (!scp_channel_write_all(session, lib_session, channel, socket_fd, buffer, read_len)) {
                ret = ESP_FAIL;
                break;
            }
            transferred += read_len;
            if (transferred >= next_progress || transferred == total) {
                scp_send_progress(session, transferred, total);
                next_progress = transferred + SOLAR_OS_SCP_PROGRESS_STEP;
            }
        }

        if (read_len < SOLAR_OS_SCP_BUFFER_SIZE) {
            if (ferror(file)) {
                scp_send_error(session, "local file read failed");
                ret = ESP_FAIL;
            }
            break;
        }
    }

    solar_os_memory_free(buffer);
    fclose(file);
    scp_close_channel(session, lib_session, channel, socket_fd);
    return scp_should_stop(session) ? ESP_ERR_INVALID_STATE : ret;
}

static esp_err_t scp_download(solar_os_scp_session_t *session,
                              LIBSSH2_SESSION *lib_session,
                              int socket_fd)
{
    libssh2_struct_stat fileinfo = {0};
    LIBSSH2_CHANNEL *channel =
        scp_recv_channel_open(session, lib_session, socket_fd, &fileinfo);
    if (channel == NULL) {
        return ESP_FAIL;
    }

    FILE *file = fopen(session->local_path, "wb");
    if (file == NULL) {
        SOLAR_OS_LOGE(TAG, "local file create failed: %s errno=%d", session->local_path, errno);
        scp_close_channel(session, lib_session, channel, socket_fd);
        scp_send_error(session, "local file create failed");
        return ESP_FAIL;
    }

    char *buffer = scp_alloc_buffer();
    if (buffer == NULL) {
        fclose(file);
        scp_close_channel(session, lib_session, channel, socket_fd);
        scp_send_error(session, "transfer buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    const uint64_t total = fileinfo.st_size > 0 ? (uint64_t)fileinfo.st_size : 0ULL;
    SOLAR_OS_LOGI(TAG,
             "download open remote=%s local=%s bytes=%" PRIu64,
             session->remote_path,
             session->local_path,
             total);
    scp_send_status(session, "downloading");
    uint64_t transferred = 0;
    uint64_t next_progress = 0;
    esp_err_t ret = ESP_OK;

    while (!scp_should_stop(session) && transferred < total) {
        size_t want = SOLAR_OS_SCP_BUFFER_SIZE;
        if (total - transferred < want) {
            want = (size_t)(total - transferred);
        }

        const ssize_t read_len = libssh2_channel_read(channel, buffer, want);
        if (read_len > 0) {
            if (fwrite(buffer, 1, (size_t)read_len, file) != (size_t)read_len) {
                scp_send_error(session, "local file write failed");
                ret = ESP_FAIL;
                break;
            }
            transferred += (uint64_t)read_len;
            if (transferred >= next_progress || transferred == total) {
                scp_send_progress(session, transferred, total);
                next_progress = transferred + SOLAR_OS_SCP_PROGRESS_STEP;
            }
            continue;
        }

        if (read_len == LIBSSH2_ERROR_EAGAIN) {
            (void)scp_wait_socket(session, socket_fd, lib_session);
            continue;
        }

        scp_send_libssh2_error(session, lib_session, "channel read failed", (int)read_len);
        ret = ESP_FAIL;
        break;
    }

    if (fclose(file) != 0 && ret == ESP_OK) {
        scp_send_error(session, "local file close failed");
        ret = ESP_FAIL;
    }
    solar_os_memory_free(buffer);
    scp_close_channel(session, lib_session, channel, socket_fd);

    if (ret != ESP_OK || scp_should_stop(session)) {
        (void)unlink(session->local_path);
    }
    return scp_should_stop(session) ? ESP_ERR_INVALID_STATE : ret;
}

static esp_err_t scp_download_glob(solar_os_scp_session_t *session,
                                   LIBSSH2_SESSION *lib_session,
                                   int socket_fd)
{
    struct stat local_st;
    if (stat(session->local_path, &local_st) != 0 || !S_ISDIR(local_st.st_mode)) {
        SOLAR_OS_LOGE(TAG, "local target is not a directory: %s errno=%d", session->local_path, errno);
        scp_send_error(session, "local target must be a directory");
        return ESP_ERR_INVALID_ARG;
    }

    char command[SOLAR_OS_SCP_COMMAND_MAX];
    if (!scp_build_remote_glob_command(session->remote_path, command, sizeof(command))) {
        scp_send_error(session, "remote path too long");
        return ESP_ERR_INVALID_ARG;
    }

    SOLAR_OS_LOGI(TAG,
             "download glob open remote=%s local_dir=%s",
             session->remote_path,
             session->local_path);
    LIBSSH2_CHANNEL *channel =
        scp_exec_channel_open(session, lib_session, socket_fd, command, "opening remote files");
    if (channel == NULL) {
        return ESP_FAIL;
    }

    char *buffer = scp_alloc_buffer();
    if (buffer == NULL) {
        scp_close_channel(session, lib_session, channel, socket_fd);
        scp_send_error(session, "transfer buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    size_t received_files = 0;

    if (!scp_write_protocol_byte(session, lib_session, channel, socket_fd, '\0')) {
        ret = ESP_FAIL;
        goto done;
    }

    while (!scp_should_stop(session)) {
        char line[SOLAR_OS_SCP_PROTOCOL_LINE_MAX];
        bool eof = false;
        if (!scp_channel_read_line(session,
                                   lib_session,
                                   channel,
                                   socket_fd,
                                   line,
                                   sizeof(line),
                                   &eof)) {
            if (eof && received_files > 0) {
                ret = ESP_OK;
            } else if (eof) {
                scp_send_error(session, "no remote files matched");
                ret = ESP_ERR_NOT_FOUND;
            } else {
                ret = ESP_FAIL;
            }
            break;
        }

        if (line[0] == '\1' || line[0] == '\2') {
            scp_send_error(session, line[1] != '\0' ? line + 1 : "remote scp failed");
            ret = ESP_FAIL;
            break;
        }

        if (line[0] == 'T') {
            if (!scp_write_protocol_byte(session, lib_session, channel, socket_fd, '\0')) {
                ret = ESP_FAIL;
                break;
            }
            continue;
        }

        if (line[0] == 'E') {
            if (!scp_write_protocol_byte(session, lib_session, channel, socket_fd, '\0')) {
                ret = ESP_FAIL;
                break;
            }
            continue;
        }

        if (line[0] == 'D') {
            scp_send_error(session, "remote directories are not supported");
            scp_write_protocol_fatal(session,
                                     lib_session,
                                     channel,
                                     socket_fd,
                                     "remote directories are not supported");
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        int mode = 0;
        uint64_t total = 0;
        char *remote_name = NULL;
        if (!scp_parse_file_header(line, &mode, &total, &remote_name)) {
            scp_send_error(session, "invalid remote file header");
            scp_write_protocol_fatal(session,
                                     lib_session,
                                     channel,
                                     socket_fd,
                                     "invalid remote file header");
            ret = ESP_FAIL;
            break;
        }

        if (!scp_remote_filename_is_safe(remote_name)) {
            scp_send_error(session, "unsafe remote filename");
            scp_write_protocol_fatal(session,
                                     lib_session,
                                     channel,
                                     socket_fd,
                                     "unsafe remote filename");
            ret = ESP_FAIL;
            break;
        }

        char local_path[SOLAR_OS_STORAGE_PATH_MAX];
        if (!scp_join_local_path(local_path, sizeof(local_path), session->local_path, remote_name)) {
            scp_send_error(session, "local file path too long");
            scp_write_protocol_fatal(session,
                                     lib_session,
                                     channel,
                                     socket_fd,
                                     "local file path too long");
            ret = ESP_FAIL;
            break;
        }

        FILE *file = fopen(local_path, "wb");
        if (file == NULL) {
            SOLAR_OS_LOGE(TAG, "local file create failed: %s errno=%d", local_path, errno);
            scp_send_error(session, "local file create failed");
            scp_write_protocol_fatal(session,
                                     lib_session,
                                     channel,
                                     socket_fd,
                                     "local file create failed");
            ret = ESP_FAIL;
            break;
        }

        char status[SOLAR_OS_SCP_EVENT_MESSAGE_MAX];
        snprintf(status, sizeof(status), "downloading %s", remote_name);
        scp_send_status(session, status);
        SOLAR_OS_LOGI(TAG,
                 "download glob file remote=%s local=%s mode=0%o bytes=%" PRIu64,
                 remote_name,
                 local_path,
                 mode,
                 total);

        if (!scp_write_protocol_byte(session, lib_session, channel, socket_fd, '\0')) {
            fclose(file);
            (void)unlink(local_path);
            ret = ESP_FAIL;
            break;
        }

        bool file_ok = true;
        uint64_t transferred = 0;
        uint64_t next_progress = 0;

        while (!scp_should_stop(session) && transferred < total) {
            size_t want = SOLAR_OS_SCP_BUFFER_SIZE;
            if (total - transferred < want) {
                want = (size_t)(total - transferred);
            }

            const ssize_t read_len = libssh2_channel_read(channel, buffer, want);
            if (read_len > 0) {
                if (fwrite(buffer, 1, (size_t)read_len, file) != (size_t)read_len) {
                    scp_send_error(session, "local file write failed");
                    file_ok = false;
                    ret = ESP_FAIL;
                    break;
                }
                transferred += (uint64_t)read_len;
                if (transferred >= next_progress || transferred == total) {
                    scp_send_progress(session, transferred, total);
                    next_progress = transferred + SOLAR_OS_SCP_PROGRESS_STEP;
                }
                continue;
            }

            if (read_len == LIBSSH2_ERROR_EAGAIN) {
                (void)scp_wait_socket(session, socket_fd, lib_session);
                continue;
            }

            if (read_len == 0 && !libssh2_channel_eof(channel)) {
                (void)scp_wait_socket(session, socket_fd, lib_session);
                continue;
            }

            if (read_len == 0) {
                scp_send_error(session, "unexpected remote EOF");
            } else {
                scp_send_libssh2_error(session, lib_session, "channel read failed", (int)read_len);
            }
            file_ok = false;
            ret = ESP_FAIL;
            break;
        }

        if (fclose(file) != 0 && file_ok) {
            scp_send_error(session, "local file close failed");
            file_ok = false;
            ret = ESP_FAIL;
        }
        if (!file_ok || scp_should_stop(session)) {
            (void)unlink(local_path);
            break;
        }

        char status_code = '\0';
        eof = false;
        if (!scp_channel_read_byte(session,
                                   lib_session,
                                   channel,
                                   socket_fd,
                                   &status_code,
                                   &eof)) {
            scp_send_error(session, eof ? "unexpected remote EOF" : "remote file status failed");
            (void)unlink(local_path);
            ret = ESP_FAIL;
            break;
        }
        if (status_code == '\1' || status_code == '\2') {
            (void)scp_report_remote_status(session,
                                           lib_session,
                                           channel,
                                           socket_fd,
                                           "remote scp failed");
            (void)unlink(local_path);
            ret = ESP_FAIL;
            break;
        }
        if (status_code != '\0') {
            scp_send_error(session, "invalid remote file status");
            (void)unlink(local_path);
            ret = ESP_FAIL;
            break;
        }

        if (!scp_write_protocol_byte(session, lib_session, channel, socket_fd, '\0')) {
            (void)unlink(local_path);
            ret = ESP_FAIL;
            break;
        }

        received_files++;
    }

done:
    solar_os_memory_free(buffer);
    scp_close_channel(session, lib_session, channel, socket_fd);
    return scp_should_stop(session) ? ESP_ERR_INVALID_STATE : ret;
}

static void scp_session_task(void *arg)
{
    solar_os_scp_session_t *session = (solar_os_scp_session_t *)arg;
    solar_os_ssh_transport_t transport = {
        .socket_fd = -1,
    };
    solar_os_ssh_transport_config_t transport_config = scp_transport_config(session);
    int socket_fd = -1;
    LIBSSH2_SESSION *lib_session = NULL;
    bool ok = false;

    SOLAR_OS_LOGI(TAG,
             "task start: %s %s@%s:%" PRIu16 " local=%s remote=%s",
             scp_direction_name(session->direction),
             session->username,
             session->host,
             session->port,
             session->local_path,
             session->remote_path);

    if (solar_os_ssh_transport_open(&transport_config, &transport) != ESP_OK ||
        scp_should_stop(session)) {
        goto done;
    }
    socket_fd = transport.socket_fd;
    lib_session = transport.session;

    esp_err_t transfer_ret = ESP_FAIL;
    if (session->direction == SOLAR_OS_SCP_UPLOAD) {
        transfer_ret = scp_upload(session, lib_session, socket_fd);
    } else if (session->remote_glob && scp_remote_path_has_wildcards(session->remote_path)) {
        transfer_ret = scp_download_glob(session, lib_session, socket_fd);
    } else {
        transfer_ret = scp_download(session, lib_session, socket_fd);
    }
    ok = transfer_ret == ESP_OK;

done:
    solar_os_ssh_transport_close(&transport, "SolarOS scp shutdown");

    if (session != NULL) {
        SOLAR_OS_LOGI(TAG, "task complete: %s", ok ? "ok" : "failed");
        scp_send_event(session,
                       SOLAR_OS_SCP_EVENT_DONE,
                       ok ? "done" : "failed",
                       0,
                       0);
        const bool detached = session->detached;
        session->task_done = true;
        if (detached) {
            scp_session_destroy(session);
        }
    }

    SOLAR_OS_LOGI(TAG, "SCP task stopped");
    vTaskDelete(NULL);
}

esp_err_t solar_os_scp_start(const solar_os_scp_config_t *config,
                             solar_os_scp_session_t **session_out)
{
    if (config == NULL ||
        session_out == NULL ||
        config->host == NULL ||
        config->host[0] == '\0' ||
        config->username == NULL ||
        config->username[0] == '\0' ||
        config->local_path == NULL ||
        config->local_path[0] == '\0' ||
        config->remote_path == NULL ||
        config->remote_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_scp_session_t *session = solar_os_memory_calloc(1,
                                                             sizeof(*session),
                                                             SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                             "scp.session");
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }

    session->direction = config->direction;
    strlcpy(session->host, config->host, sizeof(session->host));
    strlcpy(session->username, config->username, sizeof(session->username));
    strlcpy(session->password, config->password != NULL ? config->password : "", sizeof(session->password));
    strlcpy(session->local_path, config->local_path, sizeof(session->local_path));
    strlcpy(session->remote_path, config->remote_path, sizeof(session->remote_path));
    session->port = config->port > 0 ? config->port : SOLAR_OS_SCP_DEFAULT_PORT;
    session->remote_glob = config->remote_glob;

    SOLAR_OS_LOGI(TAG,
             "start request: %s %s@%s:%" PRIu16 " local=%s remote=%s password=%s",
             scp_direction_name(session->direction),
             session->username,
             session->host,
             session->port,
             session->local_path,
             session->remote_path,
             session->password[0] != '\0' ? "yes" : "no");

    session->events = xQueueCreate(SOLAR_OS_SCP_EVENT_QUEUE_LEN, sizeof(solar_os_scp_event_t));
    if (session->events == NULL) {
        solar_os_scp_stop(session);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = solar_os_task_create_pinned(scp_session_task,
                                                      "solar_os_scp",
                                                      SOLAR_OS_SCP_TASK_STACK,
                                                      session,
                                                      SOLAR_OS_SCP_TASK_PRIORITY,
                                                      &session->task,
                                                      tskNO_AFFINITY);
    if (created != pdPASS) {
        solar_os_scp_stop(session);
        return ESP_ERR_NO_MEM;
    }

    *session_out = session;
    return ESP_OK;
}

bool solar_os_scp_stop(solar_os_scp_session_t *session)
{
    if (session == NULL) {
        return true;
    }

    session->stop_requested = true;
    if (!solar_os_task_wait_done(session->task,
                                 &session->task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        session->detached = true;
        SOLAR_OS_LOGW(TAG, "SCP task did not stop within %u ms; detached for worker cleanup",
                 (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return false;
    }

    scp_session_destroy(session);
    return true;
}

bool solar_os_scp_poll(solar_os_scp_session_t *session, solar_os_scp_event_t *event)
{
    if (session == NULL || event == NULL || session->events == NULL) {
        return false;
    }

    return xQueueReceive(session->events, event, 0) == pdTRUE;
}
