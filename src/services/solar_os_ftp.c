#include "solar_os_ftp.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_random.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "solar_os_memory.h"
#include "solar_os_net.h"
#include "solar_os_storage.h"

#define FTP_COMMAND_MAX 384U
#define FTP_LINE_MAX 512U
#define FTP_IO_CHUNK 2048U

struct solar_os_ftp_session {
    int control_fd;
    struct sockaddr_in peer;
    uint32_t timeout_ms;
    solar_os_ftp_cancel_fn_t should_cancel;
    void *cancel_user;
    char reply[SOLAR_OS_FTP_REPLY_MAX];
};

static bool ftp_cancelled(const solar_os_ftp_session_t *session)
{
    return session != NULL && session->should_cancel != NULL &&
        session->should_cancel(session->cancel_user);
}

static esp_err_t ftp_staging_paths(const char *path,
                                   char *staged_path,
                                   size_t staged_len,
                                   char *backup_path,
                                   size_t backup_len)
{
    for (size_t attempt = 0; attempt < 32U; attempt++) {
        const uint32_t token = esp_random();
        char staged_suffix[32];
        char backup_suffix[32];
        snprintf(staged_suffix, sizeof(staged_suffix), ".ftp-part-%08" PRIx32, token);
        snprintf(backup_suffix, sizeof(backup_suffix), ".ftp-bak-%08" PRIx32, token);
        esp_err_t err = solar_os_storage_sibling_path(path,
                                                       staged_suffix,
                                                       staged_path,
                                                       staged_len);
        if (err == ESP_OK) {
            err = solar_os_storage_sibling_path(path,
                                                 backup_suffix,
                                                 backup_path,
                                                 backup_len);
        }
        if (err != ESP_OK) {
            return err;
        }
        struct stat info;
        const bool staged_absent = stat(staged_path, &info) != 0 && errno == ENOENT;
        const bool backup_absent = stat(backup_path, &info) != 0 && errno == ENOENT;
        if (staged_absent && backup_absent) {
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static bool ftp_safe_argument(const char *text)
{
    return text != NULL && strchr(text, '\r') == NULL && strchr(text, '\n') == NULL;
}

static void ftp_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

static esp_err_t ftp_socket_timeout(int fd, uint32_t timeout_ms)
{
    const struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ftp_send_all(solar_os_ftp_session_t *session,
                              int fd,
                              const void *data,
                              size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    while (offset < len) {
        if (ftp_cancelled(session)) {
            return ESP_ERR_INVALID_STATE;
        }
        const ssize_t sent = send(fd, bytes + offset, len - offset, 0);
        if (sent > 0) {
            offset += (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return errno == EAGAIN || errno == EWOULDBLOCK ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t ftp_read_line(solar_os_ftp_session_t *session,
                               int fd,
                               char *line,
                               size_t line_len)
{
    if (line == NULL || line_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t used = 0;
    while (used + 1U < line_len) {
        if (ftp_cancelled(session)) {
            return ESP_ERR_INVALID_STATE;
        }
        char ch = '\0';
        const ssize_t received = recv(fd, &ch, 1, 0);
        if (received == 1) {
            if (ch == '\n') {
                if (used > 0 && line[used - 1U] == '\r') {
                    used--;
                }
                line[used] = '\0';
                return ESP_OK;
            }
            line[used++] = ch;
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        line[used] = '\0';
        return received == 0 ? ESP_ERR_INVALID_RESPONSE :
            (errno == EAGAIN || errno == EWOULDBLOCK ? ESP_ERR_TIMEOUT : ESP_FAIL);
    }
    line[line_len - 1U] = '\0';
    return ESP_ERR_INVALID_SIZE;
}

static esp_err_t ftp_read_reply(solar_os_ftp_session_t *session, int *code_out)
{
    char line[FTP_LINE_MAX];
    esp_err_t err = ftp_read_line(session, session->control_fd, line, sizeof(line));
    if (err != ESP_OK) {
        return err;
    }
    if (!isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1]) ||
        !isdigit((unsigned char)line[2])) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
    strlcpy(session->reply, line, sizeof(session->reply));
    if (line[3] == '-') {
        for (;;) {
            err = ftp_read_line(session, session->control_fd, line, sizeof(line));
            if (err != ESP_OK) {
                return err;
            }
            strlcpy(session->reply, line, sizeof(session->reply));
            if (line[0] == (char)('0' + code / 100) &&
                line[1] == (char)('0' + (code / 10) % 10) &&
                line[2] == (char)('0' + code % 10) && line[3] == ' ') {
                break;
            }
        }
    }
    if (code_out != NULL) {
        *code_out = code;
    }
    return ESP_OK;
}

static esp_err_t ftp_command(solar_os_ftp_session_t *session,
                             int *code_out,
                             const char *format,
                             ...)
{
    char command[FTP_COMMAND_MAX];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(command, sizeof(command) - 2U, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof(command) - 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    command[written] = '\r';
    command[written + 1] = '\n';
    const esp_err_t err = ftp_send_all(session,
                                       session->control_fd,
                                       command,
                                       (size_t)written + 2U);
    return err == ESP_OK ? ftp_read_reply(session, code_out) : err;
}

static esp_err_t ftp_expect_class(esp_err_t err, int code, int expected_class)
{
    if (err != ESP_OK) {
        return err;
    }
    return code / 100 == expected_class ? ESP_OK : ESP_FAIL;
}

static esp_err_t ftp_connect_addr(const struct sockaddr_in *addr,
                                  uint32_t timeout_ms,
                                  int *fd_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    esp_err_t err = ftp_socket_timeout(fd, timeout_ms);
    if (err == ESP_OK && connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) != 0) {
        err = errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ?
            ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ftp_close_fd(&fd);
        return err;
    }
    *fd_out = fd;
    return ESP_OK;
}

static bool ftp_parse_epsv(const char *reply, uint16_t *port)
{
    const char *open = reply != NULL ? strchr(reply, '(') : NULL;
    const char *close = open != NULL ? strchr(open + 1, ')') : NULL;
    if (open == NULL || close == NULL || close <= open + 4) {
        return false;
    }
    const char delimiter = open[1];
    const char *last = close;
    while (last > open + 1 && last[-1] != delimiter) {
        last--;
    }
    if (last <= open + 1) {
        return false;
    }
    const char *start = last - 1;
    while (start > open + 1 && start[-1] != delimiter) {
        start--;
    }
    char value[8];
    const size_t len = (size_t)((last - 1) - start);
    if (len == 0 || len >= sizeof(value)) {
        return false;
    }
    memcpy(value, start, len);
    value[len] = '\0';
    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)parsed;
    return true;
}

static bool ftp_parse_pasv(const char *reply, struct sockaddr_in *addr)
{
    const char *p = reply != NULL ? strchr(reply, '(') : NULL;
    unsigned values[6];
    if (p == NULL || sscanf(p + 1,
                            "%u,%u,%u,%u,%u,%u",
                            &values[0], &values[1], &values[2],
                            &values[3], &values[4], &values[5]) != 6) {
        return false;
    }
    for (size_t i = 0; i < 6U; i++) {
        if (values[i] > 255U) {
            return false;
        }
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    const uint32_t ip = (values[0] << 24U) | (values[1] << 16U) |
        (values[2] << 8U) | values[3];
    addr->sin_addr.s_addr = htonl(ip);
    addr->sin_port = htons((uint16_t)(values[4] * 256U + values[5]));
    return true;
}

static esp_err_t ftp_open_data(solar_os_ftp_session_t *session, int *data_fd)
{
    int code = 0;
    struct sockaddr_in addr = session->peer;
    esp_err_t err = ftp_command(session, &code, "EPSV");
    uint16_t port = 0;
    if (err == ESP_OK && code / 100 == 2 && ftp_parse_epsv(session->reply, &port)) {
        addr.sin_port = htons(port);
    } else {
        struct sockaddr_in passive_addr = {0};
        err = ftp_command(session, &code, "PASV");
        if (err != ESP_OK || code / 100 != 2 ||
            !ftp_parse_pasv(session->reply, &passive_addr)) {
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
        addr.sin_port = passive_addr.sin_port;
    }
    return ftp_connect_addr(&addr, session->timeout_ms, data_fd);
}

static esp_err_t ftp_begin_data_command(solar_os_ftp_session_t *session,
                                        int *data_fd,
                                        const char *verb,
                                        const char *path)
{
    if (!ftp_safe_argument(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ftp_open_data(session, data_fd);
    if (err != ESP_OK) {
        return err;
    }
    int code = 0;
    err = path[0] != '\0' ? ftp_command(session, &code, "%s %s", verb, path) :
        ftp_command(session, &code, "%s", verb);
    if (err != ESP_OK || code / 100 != 1) {
        ftp_close_fd(data_fd);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    return ESP_OK;
}

static esp_err_t ftp_finish_data(solar_os_ftp_session_t *session,
                                 int *data_fd,
                                 esp_err_t transfer_err)
{
    const bool completion_pending = data_fd != NULL && *data_fd >= 0;
    ftp_close_fd(data_fd);
    if (!completion_pending || ftp_cancelled(session)) {
        return transfer_err;
    }
    int code = 0;
    const esp_err_t reply_err = ftp_read_reply(session, &code);
    if (transfer_err != ESP_OK) {
        return transfer_err;
    }
    return ftp_expect_class(reply_err, code, 2);
}

esp_err_t solar_os_ftp_connect(const solar_os_ftp_options_t *options,
                               solar_os_ftp_cancel_fn_t should_cancel,
                               void *cancel_user,
                               solar_os_ftp_session_t **session_out)
{
    if (options == NULL || options->host == NULL || options->host[0] == '\0' ||
        !ftp_safe_argument(options->username) || !ftp_safe_argument(options->password) ||
        session_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *session_out = NULL;
    char resolved[SOLAR_OS_NET_ADDR_MAX];
    esp_err_t err = solar_os_net_resolve_host(options->host, resolved, sizeof(resolved));
    if (err != ESP_OK) {
        return err;
    }
    solar_os_ftp_session_t *session = solar_os_memory_calloc(
        1, sizeof(*session), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "ftp.session");
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    session->control_fd = -1;
    session->timeout_ms = options->timeout_ms > 0 ? options->timeout_ms :
        SOLAR_OS_FTP_DEFAULT_TIMEOUT_MS;
    session->should_cancel = should_cancel;
    session->cancel_user = cancel_user;
    session->peer.sin_family = AF_INET;
    session->peer.sin_port = htons(options->port > 0 ? options->port :
                                   SOLAR_OS_FTP_DEFAULT_PORT);
    if (inet_pton(AF_INET, resolved, &session->peer.sin_addr) != 1) {
        solar_os_memory_free(session);
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = ftp_connect_addr(&session->peer, session->timeout_ms, &session->control_fd);
    int code = 0;
    if (err == ESP_OK) {
        err = ftp_read_reply(session, &code);
        err = ftp_expect_class(err, code, 2);
    }
    if (err == ESP_OK) {
        err = ftp_command(session, &code, "USER %s", options->username);
        if (err == ESP_OK && code == 331) {
            err = ftp_command(session, &code, "PASS %s", options->password);
            err = ftp_expect_class(err, code, 2);
        } else {
            err = ftp_expect_class(err, code, 2);
        }
    }
    if (err == ESP_OK) {
        err = ftp_command(session, &code, "TYPE I");
        err = ftp_expect_class(err, code, 2);
    }
    if (err != ESP_OK) {
        solar_os_ftp_disconnect(session);
        return err;
    }
    *session_out = session;
    return ESP_OK;
}

void solar_os_ftp_disconnect(solar_os_ftp_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->control_fd >= 0 && !ftp_cancelled(session)) {
        int code = 0;
        (void)ftp_command(session, &code, "QUIT");
    }
    ftp_close_fd(&session->control_fd);
    solar_os_memory_free(session);
}

const char *solar_os_ftp_last_reply(const solar_os_ftp_session_t *session)
{
    return session != NULL ? session->reply : "";
}

typedef struct {
    solar_os_ftp_entry_fn_t callback;
    void *user;
    bool mlsd;
    char pending[FTP_LINE_MAX];
    size_t pending_len;
    size_t emitted;
} ftp_list_parser_t;

static bool ftp_parse_uint64(const char *text, uint64_t *value)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    uint64_t result = 0;
    while (isdigit((unsigned char)*text)) {
        const uint8_t digit = (uint8_t)(*text++ - '0');
        if (result > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    if (*text != '\0') {
        return false;
    }
    *value = result;
    return true;
}

static bool ftp_emit_mlsd(ftp_list_parser_t *parser, char *line)
{
    char *name = strchr(line, ' ');
    if (name == NULL) {
        return true;
    }
    *name++ = '\0';
    while (*name == ' ') {
        name++;
    }
    if (*name == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return true;
    }
    solar_os_ftp_entry_t entry = {0};
    strlcpy(entry.name, name, sizeof(entry.name));
    char *save = NULL;
    for (char *fact = strtok_r(line, ";", &save); fact != NULL;
         fact = strtok_r(NULL, ";", &save)) {
        if (strncasecmp(fact, "type=", 5) == 0) {
            entry.is_directory = strcasecmp(fact + 5, "dir") == 0 ||
                strcasecmp(fact + 5, "cdir") == 0 ||
                strcasecmp(fact + 5, "pdir") == 0;
        } else if (strncasecmp(fact, "size=", 5) == 0) {
            (void)ftp_parse_uint64(fact + 5, &entry.size);
        }
    }
    const bool accepted = parser->callback == NULL ||
        parser->callback(&entry, parser->user);
    if (accepted) {
        parser->emitted++;
    }
    return accepted;
}

static bool ftp_emit_list(ftp_list_parser_t *parser, char *line)
{
    if (*line == '\0' || strncasecmp(line, "total ", 6) == 0) {
        return true;
    }
    solar_os_ftp_entry_t entry = {0};
    entry.is_directory = line[0] == 'd';
    char *fields[9] = {0};
    size_t count = 0;
    char *p = line;
    while (*p != '\0' && count < 8U) {
        while (*p == ' ') {
            p++;
        }
        fields[count++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    while (*p == ' ') {
        p++;
    }
    if (count < 8U || *p == '\0') {
        return true;
    }
    (void)ftp_parse_uint64(fields[4], &entry.size);
    strlcpy(entry.name, p, sizeof(entry.name));
    const bool accepted = parser->callback == NULL ||
        parser->callback(&entry, parser->user);
    if (accepted) {
        parser->emitted++;
    }
    return accepted;
}

static bool ftp_list_feed(ftp_list_parser_t *parser, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (parser->pending_len > 0 && parser->pending[parser->pending_len - 1U] == '\r') {
                parser->pending_len--;
            }
            parser->pending[parser->pending_len] = '\0';
            const bool keep = parser->mlsd ? ftp_emit_mlsd(parser, parser->pending) :
                ftp_emit_list(parser, parser->pending);
            parser->pending_len = 0;
            if (!keep) {
                return false;
            }
        } else if (parser->pending_len + 1U < sizeof(parser->pending)) {
            parser->pending[parser->pending_len++] = (char)data[i];
        } else {
            return false;
        }
    }
    return true;
}

static esp_err_t ftp_list_once(solar_os_ftp_session_t *session,
                               const char *verb,
                               const char *path,
                               ftp_list_parser_t *parser)
{
    int data_fd = -1;
    esp_err_t err = ftp_begin_data_command(session, &data_fd, verb, path);
    uint8_t buffer[FTP_IO_CHUNK];
    while (err == ESP_OK) {
        if (ftp_cancelled(session)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const ssize_t received = recv(data_fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            if (!ftp_list_feed(parser, buffer, (size_t)received)) {
                err = ESP_ERR_INVALID_SIZE;
            }
        } else if (received == 0) {
            break;
        } else if (errno != EINTR) {
            err = errno == EAGAIN || errno == EWOULDBLOCK ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
    }
    if (err == ESP_OK && parser->pending_len > 0) {
        parser->pending[parser->pending_len] = '\0';
        if (!(parser->mlsd ? ftp_emit_mlsd(parser, parser->pending) :
              ftp_emit_list(parser, parser->pending))) {
            err = ESP_ERR_INVALID_SIZE;
        }
        parser->pending_len = 0;
    }
    return ftp_finish_data(session, &data_fd, err);
}

esp_err_t solar_os_ftp_list(solar_os_ftp_session_t *session,
                            const char *path,
                            solar_os_ftp_entry_fn_t on_entry,
                            void *entry_user)
{
    if (session == NULL || !ftp_safe_argument(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    ftp_list_parser_t parser = {
        .callback = on_entry,
        .user = entry_user,
        .mlsd = true,
    };
    esp_err_t err = ftp_list_once(session, "MLSD", path, &parser);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (parser.emitted > 0) {
        return err;
    }
    memset(&parser.pending, 0, sizeof(parser.pending));
    parser.pending_len = 0;
    parser.mlsd = false;
    return ftp_list_once(session, "LIST", path, &parser);
}

esp_err_t solar_os_ftp_download(solar_os_ftp_session_t *session,
                                const char *remote_path,
                                const char *local_path,
                                solar_os_ftp_progress_fn_t progress,
                                void *progress_user)
{
    if (session == NULL || !ftp_safe_argument(remote_path) || local_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat active_info;
    if (stat(local_path, &active_info) == 0 && !S_ISREG(active_info.st_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    char staged_path[SOLAR_OS_STORAGE_PATH_MAX];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = ftp_staging_paths(local_path,
                                      staged_path,
                                      sizeof(staged_path),
                                      backup_path,
                                      sizeof(backup_path));
    if (err != ESP_OK) {
        return err;
    }
    FILE *file = fopen(staged_path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    int data_fd = -1;
    err = ftp_begin_data_command(session, &data_fd, "RETR", remote_path);
    uint8_t *buffer = solar_os_memory_alloc(FTP_IO_CHUNK,
                                             SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                             "ftp.download");
    if (buffer == NULL && err == ESP_OK) {
        err = ESP_ERR_NO_MEM;
    }
    uint64_t total = 0;
    while (err == ESP_OK) {
        if (ftp_cancelled(session)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const ssize_t received = recv(data_fd, buffer, FTP_IO_CHUNK, 0);
        if (received > 0) {
            if (fwrite(buffer, 1, (size_t)received, file) != (size_t)received) {
                err = ESP_FAIL;
                break;
            }
            total += (uint64_t)received;
            if (progress != NULL) {
                progress(total, progress_user);
            }
        } else if (received == 0) {
            break;
        } else if (errno != EINTR) {
            err = errno == EAGAIN || errno == EWOULDBLOCK ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
    }
    solar_os_memory_free(buffer);
    if (err == ESP_OK) {
        err = solar_os_storage_sync_file(file);
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    err = ftp_finish_data(session, &data_fd, err);
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(staged_path, local_path, backup_path);
    }
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(staged_path);
    }
    return err;
}

esp_err_t solar_os_ftp_upload(solar_os_ftp_session_t *session,
                              const char *local_path,
                              const char *remote_path,
                              solar_os_ftp_progress_fn_t progress,
                              void *progress_user)
{
    if (session == NULL || local_path == NULL || !ftp_safe_argument(remote_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(local_path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    int data_fd = -1;
    esp_err_t err = ftp_begin_data_command(session, &data_fd, "STOR", remote_path);
    uint8_t *buffer = solar_os_memory_alloc(FTP_IO_CHUNK,
                                             SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                             "ftp.upload");
    if (buffer == NULL && err == ESP_OK) {
        err = ESP_ERR_NO_MEM;
    }
    uint64_t total = 0;
    while (err == ESP_OK) {
        const size_t read_len = fread(buffer, 1, FTP_IO_CHUNK, file);
        if (read_len > 0) {
            err = ftp_send_all(session, data_fd, buffer, read_len);
            total += read_len;
            if (err == ESP_OK && progress != NULL) {
                progress(total, progress_user);
            }
        }
        if (read_len < FTP_IO_CHUNK) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
    }
    solar_os_memory_free(buffer);
    fclose(file);
    return ftp_finish_data(session, &data_fd, err);
}

static esp_err_t ftp_simple_path(solar_os_ftp_session_t *session,
                                 const char *verb,
                                 const char *path)
{
    if (session == NULL || !ftp_safe_argument(path) || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int code = 0;
    esp_err_t err = ftp_command(session, &code, "%s %s", verb, path);
    return ftp_expect_class(err, code, 2);
}

esp_err_t solar_os_ftp_mkdir(solar_os_ftp_session_t *session, const char *path)
{
    return ftp_simple_path(session, "MKD", path);
}

esp_err_t solar_os_ftp_rmdir(solar_os_ftp_session_t *session, const char *path)
{
    return ftp_simple_path(session, "RMD", path);
}

esp_err_t solar_os_ftp_remove(solar_os_ftp_session_t *session, const char *path)
{
    return ftp_simple_path(session, "DELE", path);
}

esp_err_t solar_os_ftp_rename(solar_os_ftp_session_t *session,
                              const char *old_path,
                              const char *new_path)
{
    if (session == NULL || !ftp_safe_argument(old_path) ||
        !ftp_safe_argument(new_path) || old_path[0] == '\0' || new_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int code = 0;
    esp_err_t err = ftp_command(session, &code, "RNFR %s", old_path);
    if (err != ESP_OK || code / 100 != 3) {
        return err == ESP_OK ? ESP_FAIL : err;
    }
    err = ftp_command(session, &code, "RNTO %s", new_path);
    return ftp_expect_class(err, code, 2);
}
