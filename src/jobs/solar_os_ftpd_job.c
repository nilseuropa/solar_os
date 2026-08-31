#include "solar_os_ftpd_job.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"

#define FTPD_DEFAULT_PORT 21U
#define FTPD_TASK_STACK 8192U
#define FTPD_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define FTPD_SELECT_MS 100U
#define FTPD_DATA_TIMEOUT_MS 10000U
#define FTPD_STOP_WAIT_MS 2000U
#define FTPD_LINE_MAX 384U
#define FTPD_IO_CHUNK 2048U
#define FTPD_CREDENTIAL_MAX 64U

static const char *TAG = "solar_os_ftpd";

typedef struct {
    bool running;
    bool stop_requested;
    TaskHandle_t task;
    int listen_fd;
    int client_fd;
    int passive_fd;
    uint16_t port;
    uint32_t generation;
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    char username[FTPD_CREDENTIAL_MAX];
    char password[FTPD_CREDENTIAL_MAX];
    uint32_t connection_count;
    uint32_t transfer_count;
    uint64_t transferred_bytes;
    uint32_t auth_failure_count;
    esp_err_t last_error;
} ftpd_state_t;

typedef struct {
    bool authenticated;
    bool user_ok;
    char cwd[SOLAR_OS_STORAGE_PATH_MAX];
    char rename_from[SOLAR_OS_STORAGE_PATH_MAX];
} ftpd_client_t;

static ftpd_state_t ftpd = {
    .listen_fd = -1,
    .client_fd = -1,
    .passive_fd = -1,
};
static portMUX_TYPE ftpd_lock = portMUX_INITIALIZER_UNLOCKED;

static bool ftpd_should_stop(void)
{
    portENTER_CRITICAL(&ftpd_lock);
    const bool stop = ftpd.stop_requested;
    portEXIT_CRITICAL(&ftpd_lock);
    return stop;
}

static void ftpd_close(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

static bool ftpd_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    while (offset < len && !ftpd_should_stop()) {
        const ssize_t sent = send(fd, bytes + offset, len - offset, 0);
        if (sent > 0) {
            offset += (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return offset == len;
}

static bool ftpd_reply(int fd, int code, const char *format, ...)
{
    char line[FTPD_LINE_MAX];
    const int prefix = snprintf(line, sizeof(line), "%d ", code);
    if (prefix < 0 || (size_t)prefix >= sizeof(line)) {
        return false;
    }
    va_list args;
    va_start(args, format);
    const int body = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 2U,
                               format, args);
    va_end(args);
    if (body < 0 || (size_t)body >= sizeof(line) - (size_t)prefix - 2U) {
        return false;
    }
    const size_t used = (size_t)prefix + (size_t)body;
    line[used] = '\r';
    line[used + 1U] = '\n';
    return ftpd_send_all(fd, line, used + 2U);
}

static bool ftpd_read_line(int fd, char *line, size_t line_len)
{
    size_t used = 0;
    while (!ftpd_should_stop() && used + 1U < line_len) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = FTPD_SELECT_MS * 1000U};
        const int ready = select(fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        char ch = '\0';
        const ssize_t received = recv(fd, &ch, 1, 0);
        if (received != 1) {
            return false;
        }
        if (ch == '\n') {
            if (used > 0 && line[used - 1U] == '\r') {
                used--;
            }
            line[used] = '\0';
            return true;
        }
        line[used++] = ch;
    }
    line[used] = '\0';
    return false;
}

static bool ftpd_normalize_virtual(const char *cwd,
                                   const char *argument,
                                   char *out,
                                   size_t out_len)
{
    if (cwd == NULL || argument == NULL || out == NULL || out_len < 2U ||
        strchr(argument, '\r') != NULL || strchr(argument, '\n') != NULL) {
        return false;
    }
    char work[SOLAR_OS_STORAGE_PATH_MAX * 2U];
    const int written = argument[0] == '/' ? snprintf(work, sizeof(work), "%s", argument) :
        snprintf(work, sizeof(work), "%s%s%s", cwd,
                 strcmp(cwd, "/") == 0 ? "" : "/", argument);
    if (written < 0 || (size_t)written >= sizeof(work)) {
        return false;
    }
    char result[SOLAR_OS_STORAGE_PATH_MAX] = "/";
    size_t result_len = 1U;
    char *save = NULL;
    for (char *part = strtok_r(work, "/", &save); part != NULL;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            while (result_len > 1U && result[result_len - 1U] != '/') {
                result_len--;
            }
            if (result_len > 1U) {
                result_len--;
            }
            result[result_len] = '\0';
            continue;
        }
        const size_t part_len = strlen(part);
        const bool slash = result_len > 1U;
        if (result_len + (slash ? 1U : 0U) + part_len >= sizeof(result)) {
            return false;
        }
        if (slash) {
            result[result_len++] = '/';
        }
        memcpy(result + result_len, part, part_len + 1U);
        result_len += part_len;
    }
    strlcpy(out, result, out_len);
    return strlen(result) < out_len;
}

static bool ftpd_map_path(const ftpd_client_t *client,
                          const char *argument,
                          char *virtual_path,
                          size_t virtual_len,
                          char *local_path,
                          size_t local_len)
{
    if (!ftpd_normalize_virtual(client->cwd,
                                argument != NULL && argument[0] != '\0' ? argument : client->cwd,
                                virtual_path,
                                virtual_len)) {
        return false;
    }
    const int written = strcmp(virtual_path, "/") == 0 ?
        snprintf(local_path, local_len, "%s", ftpd.root) :
        snprintf(local_path, local_len, "%s%s", ftpd.root, virtual_path);
    return written >= 0 && (size_t)written < local_len;
}

static bool ftpd_open_passive(int client_fd, bool extended)
{
    ftpd_close(&ftpd.passive_fd);
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ftpd_reply(client_fd, 425, "Cannot open passive socket.");
    }
    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        ftpd_close(&fd);
        return ftpd_reply(client_fd, 425, "Cannot open passive socket.");
    }
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
        ftpd_close(&fd);
        return ftpd_reply(client_fd, 425, "Cannot inspect passive socket.");
    }
    ftpd.passive_fd = fd;
    const uint16_t port = ntohs(addr.sin_port);
    if (extended) {
        return ftpd_reply(client_fd, 229, "Entering Extended Passive Mode (|||%u|).",
                          (unsigned)port);
    }
    struct sockaddr_in local = {0};
    socklen_t local_len = sizeof(local);
    if (getsockname(client_fd, (struct sockaddr *)&local, &local_len) != 0) {
        ftpd_close(&ftpd.passive_fd);
        return ftpd_reply(client_fd, 425, "Cannot inspect control socket.");
    }
    const uint32_t ip = ntohl(local.sin_addr.s_addr);
    return ftpd_reply(client_fd,
                      227,
                      "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
                      (unsigned)((ip >> 24U) & 0xffU),
                      (unsigned)((ip >> 16U) & 0xffU),
                      (unsigned)((ip >> 8U) & 0xffU),
                      (unsigned)(ip & 0xffU),
                      (unsigned)(port >> 8U),
                      (unsigned)(port & 0xffU));
}

static int ftpd_accept_data(void)
{
    const int listener = ftpd.passive_fd;
    if (listener < 0) {
        return -1;
    }
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(listener, &readfds);
    struct timeval timeout = {
        .tv_sec = FTPD_DATA_TIMEOUT_MS / 1000U,
        .tv_usec = (FTPD_DATA_TIMEOUT_MS % 1000U) * 1000U,
    };
    const int ready = select(listener + 1, &readfds, NULL, NULL, &timeout);
    int data_fd = ready > 0 ? accept(listener, NULL, NULL) : -1;
    ftpd_close(&ftpd.passive_fd);
    return data_fd;
}

static bool ftpd_begin_transfer(int client_fd, int *data_fd)
{
    if (ftpd.passive_fd < 0) {
        (void)ftpd_reply(client_fd, 425, "Use PASV or EPSV first.");
        return false;
    }
    if (!ftpd_reply(client_fd, 150, "Opening binary data connection.")) {
        ftpd_close(&ftpd.passive_fd);
        return false;
    }
    *data_fd = ftpd_accept_data();
    if (*data_fd < 0) {
        (void)ftpd_reply(client_fd, 425, "Data connection failed.");
        return false;
    }
    return true;
}

static bool ftpd_send_listing(int client_fd,
                              const char *path,
                              bool machine_readable)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        ftpd_close(&ftpd.passive_fd);
        return ftpd_reply(client_fd, 550, "Directory unavailable.");
    }
    int data_fd = -1;
    if (!ftpd_begin_transfer(client_fd, &data_fd)) {
        closedir(dir);
        return true;
    }
    bool ok = true;
    struct dirent *entry;
    while (ok && !ftpd_should_stop() && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        const int joined = snprintf(child, sizeof(child), "%s%s%s", path,
                                    path[strlen(path) - 1U] == '/' ? "" : "/",
                                    entry->d_name);
        struct stat st;
        if (joined < 0 || (size_t)joined >= sizeof(child) || stat(child, &st) != 0) {
            continue;
        }
        char line[FTPD_LINE_MAX];
        const int length = machine_readable ?
            snprintf(line, sizeof(line), "type=%s;size=%" PRIu64 "; %s\r\n",
                     S_ISDIR(st.st_mode) ? "dir" : "file",
                     S_ISDIR(st.st_mode) ? 0U : (uint64_t)st.st_size,
                     entry->d_name) :
            snprintf(line, sizeof(line), "%s 1 solaros solaros %" PRIu64
                     " Jan 01 00:00 %s\r\n",
                     S_ISDIR(st.st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
                     S_ISDIR(st.st_mode) ? 0U : (uint64_t)st.st_size,
                     entry->d_name);
        ok = length > 0 && (size_t)length < sizeof(line) &&
            ftpd_send_all(data_fd, line, (size_t)length);
    }
    closedir(dir);
    ftpd_close(&data_fd);
    if (!ok || ftpd_should_stop()) {
        return ftpd_should_stop() ? false :
            ftpd_reply(client_fd, 426, "Directory transfer aborted.");
    }
    return ftpd_reply(client_fd, 226, "Directory transfer complete.");
}

static bool ftpd_retrieve(int client_fd, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ftpd_close(&ftpd.passive_fd);
        return ftpd_reply(client_fd, 550, "File unavailable.");
    }
    int data_fd = -1;
    if (!ftpd_begin_transfer(client_fd, &data_fd)) {
        fclose(file);
        return true;
    }
    uint8_t buffer[FTPD_IO_CHUNK];
    bool ok = true;
    uint64_t bytes = 0;
    while (!ftpd_should_stop()) {
        const size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0 && !ftpd_send_all(data_fd, buffer, count)) {
            ok = false;
            break;
        }
        bytes += count;
        if (count < sizeof(buffer)) {
            ok = !ferror(file);
            break;
        }
    }
    fclose(file);
    ftpd_close(&data_fd);
    if (!ok || ftpd_should_stop()) {
        return ftpd_should_stop() ? false :
            ftpd_reply(client_fd, 426, "Transfer aborted.");
    }
    ftpd.transfer_count++;
    ftpd.transferred_bytes += bytes;
    return ftpd_reply(client_fd, 226, "Transfer complete.");
}

static bool ftpd_store(int client_fd, const char *path)
{
    struct stat active_info;
    if (stat(path, &active_info) == 0 && !S_ISREG(active_info.st_mode)) {
        ftpd_close(&ftpd.passive_fd);
        return ftpd_reply(client_fd, 550, "Target is not a regular file.");
    }
    char staged_path[SOLAR_OS_STORAGE_PATH_MAX];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t attempt = 0; attempt < 32U; attempt++) {
        const uint32_t token = esp_random();
        char staged_suffix[32];
        char backup_suffix[32];
        snprintf(staged_suffix, sizeof(staged_suffix), ".ftp-part-%08" PRIx32, token);
        snprintf(backup_suffix, sizeof(backup_suffix), ".ftp-bak-%08" PRIx32, token);
        err = solar_os_storage_sibling_path(path,
                                             staged_suffix,
                                             staged_path,
                                             sizeof(staged_path));
        if (err == ESP_OK) {
            err = solar_os_storage_sibling_path(path,
                                                 backup_suffix,
                                                 backup_path,
                                                 sizeof(backup_path));
        }
        if (err != ESP_OK) {
            break;
        }
        struct stat info;
        const bool staged_absent = stat(staged_path, &info) != 0 && errno == ENOENT;
        const bool backup_absent = stat(backup_path, &info) != 0 && errno == ENOENT;
        if (staged_absent && backup_absent) {
            err = ESP_OK;
            break;
        }
        err = ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ftpd_close(&ftpd.passive_fd);
        return ftpd_reply(client_fd, 550, "File path is too long.");
    }
    FILE *file = fopen(staged_path, "wb");
    if (file == NULL) {
        ftpd_close(&ftpd.passive_fd);
        return ftpd_reply(client_fd, 550, "File unavailable.");
    }
    int data_fd = -1;
    if (!ftpd_begin_transfer(client_fd, &data_fd)) {
        fclose(file);
        (void)solar_os_storage_remove(staged_path);
        return true;
    }
    uint8_t buffer[FTPD_IO_CHUNK];
    bool ok = true;
    uint64_t bytes = 0;
    while (!ftpd_should_stop()) {
        const ssize_t count = recv(data_fd, buffer, sizeof(buffer), 0);
        if (count > 0) {
            if (fwrite(buffer, 1, (size_t)count, file) != (size_t)count) {
                ok = false;
                break;
            }
            bytes += (uint64_t)count;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            ok = false;
            break;
        }
    }
    if (ok && !ftpd_should_stop() && solar_os_storage_sync_file(file) != ESP_OK) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    ftpd_close(&data_fd);
    if (!ok || ftpd_should_stop()) {
        (void)solar_os_storage_remove(staged_path);
        return ftpd_should_stop() ? false :
            ftpd_reply(client_fd, 426, "Transfer aborted.");
    }
    err = solar_os_storage_replace_file(staged_path, path, backup_path);
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(staged_path);
        return ftpd_reply(client_fd, 451, "Could not install uploaded file.");
    }
    ftpd.transfer_count++;
    ftpd.transferred_bytes += bytes;
    return ftpd_reply(client_fd, 226, "Transfer complete.");
}

static bool ftpd_command_path(const ftpd_client_t *client,
                              const char *argument,
                              char *virtual_path,
                              char *local_path)
{
    return ftpd_map_path(client,
                         argument,
                         virtual_path,
                         SOLAR_OS_STORAGE_PATH_MAX,
                         local_path,
                         SOLAR_OS_STORAGE_PATH_MAX);
}

static bool ftpd_handle_authenticated(int fd,
                                      ftpd_client_t *client,
                                      const char *command,
                                      const char *argument)
{
    char virtual_path[SOLAR_OS_STORAGE_PATH_MAX];
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (strcmp(command, "PWD") == 0 || strcmp(command, "XPWD") == 0) {
        return ftpd_reply(fd, 257, "\"%s\" is the current directory.", client->cwd);
    }
    if (strcmp(command, "CWD") == 0 || strcmp(command, "XCWD") == 0) {
        struct stat st;
        if (!ftpd_command_path(client, argument, virtual_path, local_path) ||
            stat(local_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return ftpd_reply(fd, 550, "Directory unavailable.");
        }
        strlcpy(client->cwd, virtual_path, sizeof(client->cwd));
        return ftpd_reply(fd, 250, "Directory changed.");
    }
    if (strcmp(command, "CDUP") == 0 || strcmp(command, "XCUP") == 0) {
        if (!ftpd_command_path(client, "..", virtual_path, local_path)) {
            return ftpd_reply(fd, 550, "Directory unavailable.");
        }
        strlcpy(client->cwd, virtual_path, sizeof(client->cwd));
        return ftpd_reply(fd, 250, "Directory changed.");
    }
    if (strcmp(command, "PASV") == 0 || strcmp(command, "EPSV") == 0) {
        return ftpd_open_passive(fd, strcmp(command, "EPSV") == 0);
    }
    if (strcmp(command, "TYPE") == 0) {
        return ftpd_reply(fd, 200, "Type set to I.");
    }
    if (strcmp(command, "NOOP") == 0) {
        return ftpd_reply(fd, 200, "OK.");
    }
    if (strcmp(command, "LIST") == 0 || strcmp(command, "MLSD") == 0) {
        struct stat st;
        if (!ftpd_command_path(client,
                               argument[0] != '\0' ? argument : client->cwd,
                               virtual_path,
                               local_path) ||
            stat(local_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ftpd_close(&ftpd.passive_fd);
            return ftpd_reply(fd, 550, "Directory unavailable.");
        }
        return ftpd_send_listing(fd, local_path, strcmp(command, "MLSD") == 0);
    }
    if (strcmp(command, "RETR") == 0 || strcmp(command, "STOR") == 0) {
        if (!ftpd_command_path(client, argument, virtual_path, local_path)) {
            ftpd_close(&ftpd.passive_fd);
            return ftpd_reply(fd, 550, "Invalid path.");
        }
        if (strcmp(command, "STOR") == 0 && strcmp(virtual_path, "/") == 0) {
            ftpd_close(&ftpd.passive_fd);
            return ftpd_reply(fd, 550, "Cannot replace the export root.");
        }
        return strcmp(command, "RETR") == 0 ? ftpd_retrieve(fd, local_path) :
            ftpd_store(fd, local_path);
    }
    if (strcmp(command, "SIZE") == 0) {
        struct stat st;
        if (!ftpd_command_path(client, argument, virtual_path, local_path) ||
            stat(local_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            return ftpd_reply(fd, 550, "File unavailable.");
        }
        return ftpd_reply(fd, 213, "%" PRIu64, (uint64_t)st.st_size);
    }
    if (strcmp(command, "MKD") == 0 || strcmp(command, "XMKD") == 0) {
        if (!ftpd_command_path(client, argument, virtual_path, local_path) ||
            strcmp(virtual_path, "/") == 0 ||
            mkdir(local_path, 0777) != 0) {
            return ftpd_reply(fd, 550, "Cannot create directory.");
        }
        return ftpd_reply(fd, 257, "\"%s\" created.", virtual_path);
    }
    if (strcmp(command, "RMD") == 0 || strcmp(command, "XRMD") == 0) {
        if (!ftpd_command_path(client, argument, virtual_path, local_path) ||
            strcmp(virtual_path, "/") == 0 ||
            rmdir(local_path) != 0) {
            return ftpd_reply(fd, 550, "Cannot remove directory.");
        }
        return ftpd_reply(fd, 250, "Directory removed.");
    }
    if (strcmp(command, "DELE") == 0) {
        if (!ftpd_command_path(client, argument, virtual_path, local_path) ||
            strcmp(virtual_path, "/") == 0 ||
            unlink(local_path) != 0) {
            return ftpd_reply(fd, 550, "Cannot remove file.");
        }
        return ftpd_reply(fd, 250, "File removed.");
    }
    if (strcmp(command, "RNFR") == 0) {
        struct stat st;
        if (!ftpd_command_path(client, argument, virtual_path, local_path) ||
            strcmp(virtual_path, "/") == 0 ||
            stat(local_path, &st) != 0) {
            return ftpd_reply(fd, 550, "Source unavailable.");
        }
        strlcpy(client->rename_from, local_path, sizeof(client->rename_from));
        return ftpd_reply(fd, 350, "Ready for destination name.");
    }
    if (strcmp(command, "RNTO") == 0) {
        if (client->rename_from[0] == '\0' ||
            !ftpd_command_path(client, argument, virtual_path, local_path) ||
            strcmp(virtual_path, "/") == 0 ||
            rename(client->rename_from, local_path) != 0) {
            client->rename_from[0] = '\0';
            return ftpd_reply(fd, 550, "Rename failed.");
        }
        client->rename_from[0] = '\0';
        return ftpd_reply(fd, 250, "Rename complete.");
    }
    return ftpd_reply(fd, 502, "Command not implemented.");
}

static bool ftpd_handle_command(int fd, ftpd_client_t *client, char *line)
{
    char *argument = line;
    while (*argument != '\0' && *argument != ' ') {
        *argument = (char)toupper((unsigned char)*argument);
        argument++;
    }
    if (*argument == ' ') {
        *argument++ = '\0';
        while (*argument == ' ') {
            argument++;
        }
    }
    if (strcmp(line, "QUIT") == 0) {
        (void)ftpd_reply(fd, 221, "Goodbye.");
        return false;
    }
    if (strcmp(line, "SYST") == 0) {
        return ftpd_reply(fd, 215, "UNIX Type: L8");
    }
    if (strcmp(line, "FEAT") == 0) {
        static const char features[] =
            "211-Features\r\n EPSV\r\n MLSD\r\n SIZE\r\n UTF8\r\n211 End\r\n";
        return ftpd_send_all(fd, features, sizeof(features) - 1U);
    }
    if (strcmp(line, "OPTS") == 0) {
        return ftpd_reply(fd, 200, "Option accepted.");
    }
    if (strcmp(line, "USER") == 0) {
        client->authenticated = false;
        client->user_ok = ftpd.username[0] != '\0' ?
            strcmp(argument, ftpd.username) == 0 :
            (strcasecmp(argument, "anonymous") == 0 || strcasecmp(argument, "ftp") == 0);
        if (!client->user_ok) {
            ftpd.auth_failure_count++;
            return ftpd_reply(fd, 530, "Login incorrect.");
        }
        return ftpd_reply(fd, 331, "Password required.");
    }
    if (strcmp(line, "PASS") == 0) {
        const bool password_ok = ftpd.username[0] == '\0' ||
            (client->user_ok && strcmp(argument, ftpd.password) == 0);
        if (!client->user_ok || !password_ok) {
            ftpd.auth_failure_count++;
            return ftpd_reply(fd, 530, "Login incorrect.");
        }
        client->authenticated = true;
        return ftpd_reply(fd, 230, "Login successful.");
    }
    if (!client->authenticated) {
        return ftpd_reply(fd, 530, "Please login with USER and PASS.");
    }
    return ftpd_handle_authenticated(fd, client, line, argument);
}

static void ftpd_serve_client(int fd)
{
    ftpd_client_t client = {.cwd = "/"};
    const char *mode = ftpd.username[0] != '\0' ? "password login" : "anonymous login";
    if (!ftpd_reply(fd, 220, "SolarOS FTP ready (%s, unencrypted).", mode)) {
        return;
    }
    char line[FTPD_LINE_MAX];
    while (!ftpd_should_stop() && ftpd_read_line(fd, line, sizeof(line))) {
        if (!ftpd_handle_command(fd, &client, line)) {
            break;
        }
    }
    ftpd_close(&ftpd.passive_fd);
}

static void ftpd_task(void *arg)
{
    (void)arg;
    SOLAR_OS_LOGI(TAG,
                  "started port=%u root=%s authentication=%s",
                  (unsigned)ftpd.port,
                  ftpd.root,
                  ftpd.username[0] != '\0' ? "password" : "anonymous");
    while (!ftpd_should_stop()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ftpd.listen_fd, &readfds);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = FTPD_SELECT_MS * 1000U};
        const int ready = select(ftpd.listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR && !ftpd_should_stop()) {
                ftpd.last_error = ESP_FAIL;
            }
            continue;
        }
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        const int client_fd = accept(ftpd.listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            continue;
        }
        portENTER_CRITICAL(&ftpd_lock);
        ftpd.client_fd = client_fd;
        portEXIT_CRITICAL(&ftpd_lock);
        ftpd.connection_count++;
        ftpd_serve_client(client_fd);
        ftpd_close(&ftpd.client_fd);
    }
    ftpd_close(&ftpd.passive_fd);
    ftpd_close(&ftpd.client_fd);
    ftpd_close(&ftpd.listen_fd);
    SOLAR_OS_LOGI(TAG,
                  "stopped connections=%" PRIu32 " transfers=%" PRIu32
                  " bytes=%" PRIu64 " auth-failures=%" PRIu32,
                  ftpd.connection_count,
                  ftpd.transfer_count,
                  ftpd.transferred_bytes,
                  ftpd.auth_failure_count);
    const uint32_t generation = ftpd.generation;
    const esp_err_t last_error = ftpd.last_error;
    portENTER_CRITICAL(&ftpd_lock);
    ftpd.running = false;
    ftpd.stop_requested = false;
    ftpd.task = NULL;
    memset(ftpd.username, 0, sizeof(ftpd.username));
    memset(ftpd.password, 0, sizeof(ftpd.password));
    portEXIT_CRITICAL(&ftpd_lock);
    (void)solar_os_jobs_mark_stopped(solar_os_ftpd_job.name, generation, last_error);
    solar_os_task_delete_internal(NULL);
}

static bool ftpd_parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool ftpd_credential_valid(const char *value)
{
    if (value == NULL || value[0] == '\0' || strlen(value) >= FTPD_CREDENTIAL_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        if (!isprint(*p) || *p == '\r' || *p == '\n') {
            return false;
        }
    }
    return true;
}

static esp_err_t ftpd_parse_args(int argc,
                                 char **argv,
                                 const char **root,
                                 uint16_t *port,
                                 const char **username,
                                 const char **password)
{
    if (argc < 2 || argv == NULL || root == NULL || port == NULL ||
        username == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int index = strcmp(argv[0], solar_os_ftpd_job.name) == 0 ? 1 : 0;
    if (index >= argc || argv[index] == NULL || argv[index][0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *root = argv[index++];
    *port = FTPD_DEFAULT_PORT;
    *username = NULL;
    *password = NULL;
    bool port_set = false;
    while (index < argc) {
        if (strcmp(argv[index], "--user") == 0 && index + 1 < argc &&
            *username == NULL && ftpd_credential_valid(argv[index + 1])) {
            *username = argv[index + 1];
            index += 2;
        } else if (strcmp(argv[index], "--password") == 0 && index + 1 < argc &&
                   *password == NULL && ftpd_credential_valid(argv[index + 1])) {
            *password = argv[index + 1];
            index += 2;
        } else if (!port_set && ftpd_parse_port(argv[index], port)) {
            port_set = true;
            index++;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return (*username == NULL) == (*password == NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t ftpd_open_listener(uint16_t port, int *listener)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return ESP_FAIL;
    }
    *listener = fd;
    return ESP_OK;
}

static esp_err_t ftpd_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    const char *root_arg = NULL;
    const char *username = NULL;
    const char *password = NULL;
    uint16_t port = FTPD_DEFAULT_PORT;
    esp_err_t err = ftpd_parse_args(argc, argv, &root_arg, &port, &username, &password);
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&ftpd_lock);
    const bool busy = ftpd.running || ftpd.task != NULL;
    portEXIT_CRITICAL(&ftpd_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_storage_resolve_path(root_arg, root, sizeof(root));
    struct stat st;
    if (err != ESP_OK || stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    size_t root_len = strlen(root);
    while (root_len > 1U && root[root_len - 1U] == '/') {
        root[--root_len] = '\0';
    }
    int listener = -1;
    err = ftpd_open_listener(port, &listener);
    uint32_t generation = 0;
    if (err == ESP_OK) {
        err = solar_os_jobs_get_generation(solar_os_ftpd_job.name, &generation);
    }
    if (err != ESP_OK) {
        ftpd_close(&listener);
        return err;
    }
    portENTER_CRITICAL(&ftpd_lock);
    ftpd.running = true;
    ftpd.stop_requested = false;
    ftpd.listen_fd = listener;
    ftpd.client_fd = -1;
    ftpd.passive_fd = -1;
    ftpd.port = port;
    ftpd.generation = generation;
    strlcpy(ftpd.root, root, sizeof(ftpd.root));
    ftpd.username[0] = '\0';
    ftpd.password[0] = '\0';
    if (username != NULL) {
        strlcpy(ftpd.username, username, sizeof(ftpd.username));
        strlcpy(ftpd.password, password, sizeof(ftpd.password));
    }
    ftpd.connection_count = 0;
    ftpd.transfer_count = 0;
    ftpd.transferred_bytes = 0;
    ftpd.auth_failure_count = 0;
    ftpd.last_error = ESP_OK;
    portEXIT_CRITICAL(&ftpd_lock);
    TaskHandle_t task = NULL;
    if (solar_os_task_create_pinned_internal(ftpd_task,
                                             "ftpd_job",
                                             FTPD_TASK_STACK,
                                             &ftpd,
                                             FTPD_TASK_PRIORITY,
                                             &task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        ftpd_close(&listener);
        portENTER_CRITICAL(&ftpd_lock);
        ftpd.running = false;
        ftpd.listen_fd = -1;
        memset(ftpd.username, 0, sizeof(ftpd.username));
        memset(ftpd.password, 0, sizeof(ftpd.password));
        portEXIT_CRITICAL(&ftpd_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&ftpd_lock);
    ftpd.task = task;
    portEXIT_CRITICAL(&ftpd_lock);
    char resource[16];
    snprintf(resource, sizeof(resource), "tcp:%u", (unsigned)port);
    (void)solar_os_jobs_note_resource(solar_os_ftpd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      resource,
                                      "listen");
    (void)solar_os_jobs_note_resource(solar_os_ftpd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_FILE,
                                      ftpd.root,
                                      "export");
    return ESP_OK;
}

static void ftpd_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&ftpd_lock);
    if (!ftpd.running && ftpd.task == NULL) {
        portEXIT_CRITICAL(&ftpd_lock);
        return;
    }
    ftpd.stop_requested = true;
    const int listener = ftpd.listen_fd;
    const int client = ftpd.client_fd;
    const int passive = ftpd.passive_fd;
    portEXIT_CRITICAL(&ftpd_lock);
    if (listener >= 0) {
        (void)shutdown(listener, SHUT_RDWR);
    }
    if (client >= 0) {
        (void)shutdown(client, SHUT_RDWR);
    }
    if (passive >= 0) {
        (void)shutdown(passive, SHUT_RDWR);
    }
    for (uint32_t i = 0; i < FTPD_STOP_WAIT_MS / 25U; i++) {
        portENTER_CRITICAL(&ftpd_lock);
        const bool stopped = ftpd.task == NULL;
        portEXIT_CRITICAL(&ftpd_lock);
        if (stopped) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

const solar_os_job_t solar_os_ftpd_job = {
    .name = "ftpd",
    .summary = "FTP file server",
    .start = ftpd_start,
    .stop = ftpd_stop,
    .event = NULL,
    .worker_stack_bytes = FTPD_TASK_STACK,
};
