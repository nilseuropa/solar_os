#include "solar_os_ftp_app.h"

#include <ctype.h>
#include <dirent.h>
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

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_app_registry.h"
#include "solar_os_ftp.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_net.h"
#include "solar_os_queue.h"
#include "solar_os_shell.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define FTP_APP_PANEL_MIN_WIDTH 18U
#define FTP_APP_MESSAGE_MAX 128U
#define FTP_APP_INPUT_MAX 80U
#define FTP_APP_TASK_STACK 12288U
#define FTP_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define FTP_APP_QUEUE_LEN 2U

SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(FTP_APP_TASK_STACK);

static const char *TAG = "solar_os_ftp_app";

typedef struct {
    char name[SOLAR_OS_FTP_NAME_MAX];
    uint64_t size;
    bool is_directory;
    bool parent;
} ftp_app_entry_t;

typedef struct {
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    ftp_app_entry_t *entries;
    size_t count;
    size_t capacity;
    size_t cursor;
    size_t top;
} ftp_app_pane_t;

typedef enum {
    FTP_APP_INPUT_NONE,
    FTP_APP_INPUT_MKDIR,
    FTP_APP_INPUT_DELETE,
} ftp_app_input_t;

typedef enum {
    FTP_APP_WORK_CONNECT,
    FTP_APP_WORK_LIST,
    FTP_APP_WORK_COPY,
    FTP_APP_WORK_MOVE,
    FTP_APP_WORK_DELETE,
    FTP_APP_WORK_MKDIR,
    FTP_APP_WORK_VIEW,
    FTP_APP_WORK_STOP,
} ftp_app_work_kind_t;

typedef struct {
    ftp_app_work_kind_t kind;
    bool remote_source;
    bool is_directory;
    uint64_t size;
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char destination[SOLAR_OS_STORAGE_PATH_MAX];
    char remote_directory[SOLAR_OS_STORAGE_PATH_MAX];
} ftp_app_request_t;

typedef struct {
    ftp_app_work_kind_t kind;
    esp_err_t error;
    ftp_app_entry_t *remote_entries;
    size_t remote_count;
    char remote_path[SOLAR_OS_STORAGE_PATH_MAX];
    char view_path[SOLAR_OS_STORAGE_PATH_MAX];
    char detail[SOLAR_OS_FTP_REPLY_MAX];
} ftp_app_result_t;

typedef struct {
    uint64_t done;
    uint64_t total;
    bool total_known;
    char item[SOLAR_OS_FTP_NAME_MAX];
} ftp_app_progress_event_t;

typedef struct {
    ftp_app_work_kind_t kind;
    uint64_t done;
    uint64_t total;
    bool active;
    bool total_known;
    char item[SOLAR_OS_FTP_NAME_MAX];
} ftp_app_progress_t;

typedef struct {
    uint64_t done;
    uint64_t total;
    uint64_t file_base;
    char item[SOLAR_OS_FTP_NAME_MAX];
} ftp_app_worker_progress_t;

typedef struct {
    solar_os_tui_t tui;
    ftp_app_pane_t panes[2];
    uint8_t active;
    ftp_app_input_t input_mode;
    char input[FTP_APP_INPUT_MAX];
    size_t input_len;
    char message[FTP_APP_MESSAGE_MAX];
    char host[SOLAR_OS_NET_HOST_MAX];
    char username[64];
    char password[64];
    uint16_t port;
    QueueHandle_t requests;
    QueueHandle_t results;
    QueueHandle_t progress_events;
    TaskHandle_t worker;
    volatile bool cancel_requested;
    volatile bool worker_done;
    bool busy;
    ftp_app_progress_t progress;
    uint32_t temporary_serial;
    char temporary_view[SOLAR_OS_STORAGE_PATH_MAX];
} ftp_app_state_t;

static void *ftp_app_state;
#define ftp_app (*(ftp_app_state_t *)ftp_app_state)

static void ftp_app_render(void);
static void ftp_app_release_cleanup(void);

static void *ftp_app_realloc(void *pointer, size_t size)
{
    return solar_os_memory_realloc(pointer,
                                   size,
                                   SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                   "ftp.app.entries");
}

static void ftp_app_pane_clear(ftp_app_pane_t *pane)
{
    if (pane != NULL) {
        solar_os_memory_free(pane->entries);
        memset(pane, 0, sizeof(*pane));
    }
}

static esp_err_t ftp_app_pane_add(ftp_app_pane_t *pane,
                                  const char *name,
                                  bool is_directory,
                                  bool parent,
                                  uint64_t size)
{
    if (pane->count == pane->capacity) {
        const size_t capacity = pane->capacity > 0 ? pane->capacity * 2U : 32U;
        ftp_app_entry_t *entries = ftp_app_realloc(
            pane->entries, capacity * sizeof(*entries));
        if (entries == NULL) {
            return ESP_ERR_NO_MEM;
        }
        pane->entries = entries;
        pane->capacity = capacity;
    }
    ftp_app_entry_t *entry = &pane->entries[pane->count++];
    memset(entry, 0, sizeof(*entry));
    strlcpy(entry->name, name, sizeof(entry->name));
    entry->is_directory = is_directory;
    entry->parent = parent;
    entry->size = size;
    return ESP_OK;
}

static int ftp_app_name_compare(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        const int ca = tolower((unsigned char)*a++);
        const int cb = tolower((unsigned char)*b++);
        if (ca != cb) {
            return ca - cb;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int ftp_app_entry_compare(const void *a, const void *b)
{
    const ftp_app_entry_t *left = (const ftp_app_entry_t *)a;
    const ftp_app_entry_t *right = (const ftp_app_entry_t *)b;
    if (left->parent != right->parent) {
        return left->parent ? -1 : 1;
    }
    if (left->is_directory != right->is_directory) {
        return left->is_directory ? -1 : 1;
    }
    return ftp_app_name_compare(left->name, right->name);
}

static bool ftp_app_join(char *out,
                         size_t out_len,
                         const char *directory,
                         const char *name)
{
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s%s",
                                 directory,
                                 strcmp(directory, "/") == 0 ||
                                         directory[strlen(directory) - 1U] == '/' ?
                                     "" : "/",
                                 name);
    return written >= 0 && (size_t)written < out_len;
}

static void ftp_app_parent(const char *path, char *parent, size_t parent_len)
{
    strlcpy(parent, path, parent_len);
    size_t len = strlen(parent);
    while (len > 1U && parent[len - 1U] == '/') {
        parent[--len] = '\0';
    }
    while (len > 1U && parent[len - 1U] != '/') {
        parent[--len] = '\0';
    }
    while (len > 1U && parent[len - 1U] == '/') {
        parent[--len] = '\0';
    }
}

static const char *ftp_app_basename(const char *path)
{
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    return slash != NULL ? slash + 1 : path != NULL ? path : "";
}

static esp_err_t ftp_app_load_local(ftp_app_pane_t *pane, const char *path)
{
    char normalized[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_normalize_path(path, normalized, sizeof(normalized));
    if (err != ESP_OK) {
        return err;
    }
    DIR *directory = opendir(normalized);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ftp_app_pane_clear(pane);
    strlcpy(pane->path, normalized, sizeof(pane->path));
    if (strcmp(normalized, "/") != 0) {
        err = ftp_app_pane_add(pane, "..", true, true, 0);
    }
    struct dirent *item;
    while (err == ESP_OK && (item = readdir(directory)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        struct stat st;
        if (!ftp_app_join(child, sizeof(child), normalized, item->d_name) ||
            stat(child, &st) != 0) {
            continue;
        }
        err = ftp_app_pane_add(pane,
                               item->d_name,
                               S_ISDIR(st.st_mode),
                               false,
                               S_ISDIR(st.st_mode) ? 0U : (uint64_t)st.st_size);
    }
    closedir(directory);
    if (err == ESP_OK && pane->count > 1U) {
        qsort(pane->entries, pane->count, sizeof(pane->entries[0]), ftp_app_entry_compare);
    }
    return err;
}

static bool ftp_app_remote_collect(const solar_os_ftp_entry_t *entry, void *user)
{
    ftp_app_pane_t *pane = (ftp_app_pane_t *)user;
    return ftp_app_pane_add(pane,
                            entry->name,
                            entry->is_directory,
                            false,
                            entry->size) == ESP_OK;
}

static esp_err_t ftp_app_list_remote(solar_os_ftp_session_t *session,
                                     const char *path,
                                     ftp_app_pane_t *pane)
{
    memset(pane, 0, sizeof(*pane));
    strlcpy(pane->path, path, sizeof(pane->path));
    esp_err_t err = strcmp(path, "/") != 0 ?
        ftp_app_pane_add(pane, "..", true, true, 0) : ESP_OK;
    if (err == ESP_OK) {
        err = solar_os_ftp_list(session, path, ftp_app_remote_collect, pane);
    }
    if (err == ESP_OK && pane->count > 1U) {
        qsort(pane->entries, pane->count, sizeof(pane->entries[0]), ftp_app_entry_compare);
    }
    if (err != ESP_OK) {
        ftp_app_pane_clear(pane);
    }
    return err;
}

static esp_err_t ftp_app_remove_local(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while (err == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!ftp_app_join(child, sizeof(child), path, entry->d_name)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = ftp_app_remove_local(child);
        }
    }
    closedir(directory);
    return err == ESP_OK && rmdir(path) != 0 ? ESP_FAIL : err;
}

static bool ftp_app_work_add(uint64_t *total, uint64_t amount)
{
    if (total == NULL || amount > UINT64_MAX - *total) {
        return false;
    }
    *total += amount;
    return true;
}

static void ftp_app_worker_publish(ftp_app_worker_progress_t *progress,
                                   const char *path,
                                   bool total_known)
{
    if (progress == NULL || ftp_app.progress_events == NULL) {
        return;
    }
    ftp_app_progress_event_t event = {
        .done = progress->done,
        .total = progress->total,
        .total_known = total_known,
    };
    if (path != NULL) {
        const char *item = ftp_app_basename(path);
        strlcpy(progress->item,
                item != NULL && item[0] != '\0' ? item : path,
                sizeof(progress->item));
    }
    strlcpy(event.item, progress->item, sizeof(event.item));
    (void)xQueueOverwrite(ftp_app.progress_events, &event);
}

static void ftp_app_transfer_progress(uint64_t bytes, void *user)
{
    ftp_app_worker_progress_t *progress = (ftp_app_worker_progress_t *)user;
    progress->done = bytes > UINT64_MAX - progress->file_base ?
        UINT64_MAX : progress->file_base + bytes;
    ftp_app_worker_publish(progress, NULL, true);
}

static esp_err_t ftp_app_measure_local(const char *path, uint64_t *total)
{
    if (ftp_app.cancel_requested) {
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    uint64_t work = 1U;
    if (!S_ISDIR(st.st_mode) &&
        (st.st_size < 0 || !ftp_app_work_add(&work, (uint64_t)st.st_size))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!ftp_app_work_add(total, work)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while (err == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        err = ftp_app_join(child, sizeof(child), path, entry->d_name) ?
            ftp_app_measure_local(child, total) : ESP_ERR_INVALID_SIZE;
    }
    closedir(directory);
    return err;
}

static esp_err_t ftp_app_measure_remote(solar_os_ftp_session_t *session,
                                        const char *path,
                                        bool directory,
                                        uint64_t size,
                                        uint64_t *total)
{
    if (ftp_app.cancel_requested) {
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t work = 1U;
    if (!directory && !ftp_app_work_add(&work, size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!ftp_app_work_add(total, work)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!directory) {
        return ESP_OK;
    }
    ftp_app_pane_t items = {0};
    esp_err_t err = ftp_app_list_remote(session, path, &items);
    for (size_t i = 0; err == ESP_OK && i < items.count; i++) {
        if (items.entries[i].parent) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!ftp_app_join(child, sizeof(child), path, items.entries[i].name)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = ftp_app_measure_remote(session,
                                         child,
                                         items.entries[i].is_directory,
                                         items.entries[i].size,
                                         total);
        }
    }
    ftp_app_pane_clear(&items);
    return err;
}

static esp_err_t ftp_app_upload_tree(solar_os_ftp_session_t *session,
                                     const char *local_path,
                                     const char *remote_path,
                                     ftp_app_worker_progress_t *progress)
{
    struct stat st;
    if (stat(local_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!S_ISDIR(st.st_mode)) {
        progress->file_base = progress->done;
        ftp_app_worker_publish(progress, local_path, true);
        esp_err_t err = solar_os_ftp_upload(session,
                                            local_path,
                                            remote_path,
                                            ftp_app_transfer_progress,
                                            progress);
        if (err == ESP_OK) {
            progress->done = progress->file_base + (uint64_t)st.st_size + 1U;
            ftp_app_worker_publish(progress, local_path, true);
        }
        return err;
    }
    esp_err_t err = solar_os_ftp_mkdir(session, remote_path);
    if (err != ESP_OK) {
        ftp_app_pane_t existing = {0};
        err = ftp_app_list_remote(session, remote_path, &existing);
        ftp_app_pane_clear(&existing);
        if (err != ESP_OK) {
            return err;
        }
    }
    progress->done++;
    ftp_app_worker_publish(progress, local_path, true);
    DIR *directory = opendir(local_path);
    if (directory == NULL) {
        return ESP_FAIL;
    }
    err = ESP_OK;
    struct dirent *entry;
    while (err == ESP_OK && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char local_child[SOLAR_OS_STORAGE_PATH_MAX];
        char remote_child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!ftp_app_join(local_child, sizeof(local_child), local_path, entry->d_name) ||
            !ftp_app_join(remote_child, sizeof(remote_child), remote_path, entry->d_name)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = ftp_app_upload_tree(session, local_child, remote_child, progress);
        }
    }
    closedir(directory);
    return err;
}

static esp_err_t ftp_app_download_tree(solar_os_ftp_session_t *session,
                                       const char *remote_path,
                                       const char *local_path,
                                       bool directory,
                                       uint64_t size,
                                       ftp_app_worker_progress_t *progress)
{
    if (!directory) {
        progress->file_base = progress->done;
        ftp_app_worker_publish(progress, remote_path, true);
        esp_err_t err = solar_os_ftp_download(session,
                                              remote_path,
                                              local_path,
                                              ftp_app_transfer_progress,
                                              progress);
        if (err == ESP_OK) {
            progress->done = progress->file_base + size + 1U;
            ftp_app_worker_publish(progress, remote_path, true);
        }
        return err;
    }
    if (mkdir(local_path, 0777) != 0) {
        struct stat st;
        if (errno != EEXIST || stat(local_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return ESP_FAIL;
        }
    }
    progress->done++;
    ftp_app_worker_publish(progress, remote_path, true);
    ftp_app_pane_t items = {0};
    esp_err_t err = ftp_app_list_remote(session, remote_path, &items);
    for (size_t i = 0; err == ESP_OK && i < items.count; i++) {
        if (items.entries[i].parent) {
            continue;
        }
        char remote_child[SOLAR_OS_STORAGE_PATH_MAX];
        char local_child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!ftp_app_join(remote_child, sizeof(remote_child), remote_path,
                          items.entries[i].name) ||
            !ftp_app_join(local_child, sizeof(local_child), local_path,
                          items.entries[i].name)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = ftp_app_download_tree(session,
                                        remote_child,
                                        local_child,
                                        items.entries[i].is_directory,
                                        items.entries[i].size,
                                        progress);
        }
    }
    ftp_app_pane_clear(&items);
    return err;
}

static esp_err_t ftp_app_remove_remote(solar_os_ftp_session_t *session,
                                       const char *path,
                                       bool directory)
{
    if (!directory) {
        return solar_os_ftp_remove(session, path);
    }
    ftp_app_pane_t items = {0};
    esp_err_t err = ftp_app_list_remote(session, path, &items);
    for (size_t i = 0; err == ESP_OK && i < items.count; i++) {
        if (items.entries[i].parent) {
            continue;
        }
        char child[SOLAR_OS_STORAGE_PATH_MAX];
        if (!ftp_app_join(child, sizeof(child), path, items.entries[i].name)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = ftp_app_remove_remote(session, child, items.entries[i].is_directory);
        }
    }
    ftp_app_pane_clear(&items);
    return err == ESP_OK ? solar_os_ftp_rmdir(session, path) : err;
}

static bool ftp_app_cancelled(void *user)
{
    (void)user;
    return ftp_app.cancel_requested;
}

static void ftp_app_worker(void *arg)
{
    (void)arg;
    solar_os_ftp_session_t *session = NULL;
    while (!ftp_app.cancel_requested) {
        ftp_app_request_t request;
        if (xQueueReceive(ftp_app.requests, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (request.kind == FTP_APP_WORK_STOP) {
            break;
        }
        ftp_app_result_t result = {.kind = request.kind, .error = ESP_OK};
        if (session == NULL) {
            const solar_os_ftp_options_t options = {
                .host = ftp_app.host,
                .port = ftp_app.port,
                .username = ftp_app.username,
                .password = ftp_app.password,
                .timeout_ms = SOLAR_OS_FTP_DEFAULT_TIMEOUT_MS,
            };
            result.error = solar_os_ftp_connect(&options,
                                                ftp_app_cancelled,
                                                NULL,
                                                &session);
        }
        ftp_app_worker_progress_t progress = {0};
        if (result.error == ESP_OK &&
            (request.kind == FTP_APP_WORK_COPY || request.kind == FTP_APP_WORK_MOVE)) {
            result.error = request.remote_source ?
                ftp_app_measure_remote(session,
                                       request.source,
                                       request.is_directory,
                                       request.size,
                                       &progress.total) :
                ftp_app_measure_local(request.source, &progress.total);
            if (result.error == ESP_OK) {
                ftp_app_worker_publish(&progress, request.source, true);
            }
        }
        if (result.error == ESP_OK && request.kind == FTP_APP_WORK_COPY) {
            result.error = request.remote_source ?
                ftp_app_download_tree(session,
                                      request.source,
                                      request.destination,
                                      request.is_directory,
                                      request.size,
                                      &progress) :
                ftp_app_upload_tree(session,
                                    request.source,
                                    request.destination,
                                    &progress);
        } else if (result.error == ESP_OK && request.kind == FTP_APP_WORK_MOVE) {
            if (request.remote_source) {
                result.error = ftp_app_download_tree(session,
                                                     request.source,
                                                     request.destination,
                                                     request.is_directory,
                                                     request.size,
                                                     &progress);
                if (result.error == ESP_OK) {
                    result.error = ftp_app_remove_remote(session,
                                                         request.source,
                                                         request.is_directory);
                }
            } else {
                result.error = ftp_app_upload_tree(session,
                                                   request.source,
                                                   request.destination,
                                                   &progress);
                if (result.error == ESP_OK) {
                    result.error = ftp_app_remove_local(request.source);
                }
            }
        } else if (result.error == ESP_OK && request.kind == FTP_APP_WORK_DELETE) {
            result.error = request.remote_source ?
                ftp_app_remove_remote(session, request.source, request.is_directory) :
                ftp_app_remove_local(request.source);
        } else if (result.error == ESP_OK && request.kind == FTP_APP_WORK_MKDIR) {
            result.error = request.remote_source ?
                solar_os_ftp_mkdir(session, request.source) :
                (mkdir(request.source, 0777) == 0 ? ESP_OK : ESP_FAIL);
        } else if (result.error == ESP_OK && request.kind == FTP_APP_WORK_VIEW) {
            result.error = solar_os_ftp_download(session,
                                                 request.source,
                                                 request.destination,
                                                 NULL,
                                                 NULL);
            if (result.error == ESP_OK) {
                strlcpy(result.view_path, request.destination, sizeof(result.view_path));
            }
        }
        if (result.error == ESP_OK && session != NULL) {
            ftp_app_pane_t remote = {0};
            const char *remote_path = request.kind == FTP_APP_WORK_LIST ?
                request.source : request.remote_directory;
            result.error = ftp_app_list_remote(session, remote_path, &remote);
            if (result.error == ESP_OK) {
                result.remote_entries = remote.entries;
                result.remote_count = remote.count;
                strlcpy(result.remote_path, remote.path, sizeof(result.remote_path));
            }
        }
        if (result.error != ESP_OK && session != NULL) {
            strlcpy(result.detail,
                    solar_os_ftp_last_reply(session),
                    sizeof(result.detail));
        }
        (void)xQueueSend(ftp_app.results, &result, portMAX_DELAY);
    }
    solar_os_ftp_disconnect(session);
    ftp_app.worker_done = true;
    ftp_app.worker = NULL;
    solar_os_task_delete(NULL);
}

static void ftp_app_set_message(const char *message)
{
    strlcpy(ftp_app.message, message != NULL ? message : "", sizeof(ftp_app.message));
}

static void ftp_app_format_size(uint64_t size, char *out, size_t out_len)
{
    if (size < 1024U) {
        snprintf(out, out_len, "%lluB", (unsigned long long)size);
    } else if (size < 1024U * 1024U) {
        snprintf(out, out_len, "%lluK", (unsigned long long)(size / 1024U));
    } else {
        snprintf(out, out_len, "%lluM", (unsigned long long)(size / (1024U * 1024U)));
    }
}

static void ftp_app_draw_pane(ftp_app_pane_t *pane,
                              size_t index,
                              size_t row,
                              size_t col,
                              size_t height,
                              size_t width)
{
    const bool active = ftp_app.active == index;
    const uint8_t title_attr = active ?
        SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD : SOLAR_OS_TUI_ATTR_BOLD;
    solar_os_tui_box(&ftp_app.tui, row, col, height, width, SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_fill(&ftp_app.tui, row, col + 1U, 1U, width - 2U, ' ', title_attr);
    solar_os_tui_write_cell(&ftp_app.tui, row, col + 2U, width - 4U,
                            pane->path, title_attr);
    const size_t list_height = height - 2U;
    if (pane->cursor < pane->top) {
        pane->top = pane->cursor;
    } else if (list_height > 0 && pane->cursor >= pane->top + list_height) {
        pane->top = pane->cursor - list_height + 1U;
    }
    for (size_t i = 0; i < list_height; i++) {
        const size_t item_index = pane->top + i;
        const size_t item_row = row + 1U + i;
        const uint8_t attr = active && item_index == pane->cursor ?
            SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL;
        solar_os_tui_fill(&ftp_app.tui, item_row, col + 1U, 1U, width - 2U, ' ', attr);
        if (item_index >= pane->count) {
            continue;
        }
        const ftp_app_entry_t *entry = &pane->entries[item_index];
        char name[SOLAR_OS_FTP_NAME_MAX + 2U];
        snprintf(name, sizeof(name), "%s%s", entry->name,
                 entry->is_directory && !entry->parent ? "/" : "");
        char size[12] = "";
        if (entry->is_directory && !entry->parent) {
            strlcpy(size, "<DIR>", sizeof(size));
        } else if (!entry->is_directory) {
            ftp_app_format_size(entry->size, size, sizeof(size));
        }
        const size_t size_width = width >= 18U ? 8U : 0U;
        const size_t name_width = size_width > 0 ? width - size_width - 3U : width - 2U;
        solar_os_tui_write_cell(&ftp_app.tui, item_row, col + 1U,
                                name_width, name,
                                entry->is_directory ? attr | SOLAR_OS_TUI_ATTR_BOLD : attr);
        if (size_width > 0) {
            solar_os_tui_write_cell(&ftp_app.tui, item_row,
                                    col + width - size_width - 1U,
                                    size_width, size, attr);
        }
    }
}

static const char *ftp_app_progress_name(ftp_app_work_kind_t kind)
{
    return kind == FTP_APP_WORK_MOVE ? "move" : "copy";
}

static const char *ftp_app_progress_title(ftp_app_work_kind_t kind)
{
    return kind == FTP_APP_WORK_MOVE ? "Moving" : "Copying";
}

static void ftp_app_draw_progress(size_t rows, size_t cols)
{
    if (!ftp_app.progress.active || rows < 4U || cols < 12U) {
        return;
    }
    const solar_os_tui_rect_t bounds = {
        .row = rows > 2U ? 1U : 0U,
        .col = 0U,
        .height = rows > 2U ? rows - 2U : rows,
        .width = cols,
    };
    solar_os_tui_rect_t popup = {0};
    char text[SOLAR_OS_FTP_NAME_MAX + 24U];
    snprintf(text,
             sizeof(text),
             "%s %s",
             ftp_app.progress.total_known ? "Transferring" : "Preparing",
             ftp_app.progress.item);
    if (solar_os_tui_text_popup(&ftp_app.tui,
                                &bounds,
                                ftp_app_progress_title(ftp_app.progress.kind),
                                text,
                                &popup) != ESP_OK ||
        popup.height <= 2U || popup.width <= 4U) {
        return;
    }
    (void)solar_os_tui_progress_bar(&ftp_app.tui,
                                    popup.row + popup.height - 2U,
                                    popup.col + 2U,
                                    popup.width - 4U,
                                    ftp_app_progress_name(ftp_app.progress.kind),
                                    ftp_app.progress.done,
                                    ftp_app.progress.total,
                                    ftp_app.progress.total_known);
}

static void ftp_app_render(void)
{
    const size_t rows = solar_os_tui_rows(&ftp_app.tui);
    const size_t cols = solar_os_tui_cols(&ftp_app.tui);
    solar_os_tui_set_cursor_visible(&ftp_app.tui,
                                    ftp_app.input_mode == FTP_APP_INPUT_MKDIR);
    solar_os_tui_clear(&ftp_app.tui);
    if (rows < 6U || cols < FTP_APP_PANEL_MIN_WIDTH * 2U) {
        solar_os_tui_draw_too_small(&ftp_app.tui, "ftp");
        solar_os_tui_refresh(&ftp_app.tui);
        return;
    }
    char title[FTP_APP_MESSAGE_MAX];
    snprintf(title, sizeof(title), "ftp %s:%u  %s  Tab switch  q quit",
             ftp_app.host, (unsigned)ftp_app.port,
             strcmp(ftp_app.username, "anonymous") == 0 ? "anonymous" : ftp_app.username);
    solar_os_tui_fill(&ftp_app.tui, 0, 0, 1, cols, ' ',
                      SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    solar_os_tui_write_cell(&ftp_app.tui, 0, 0, cols, title,
                            SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    const size_t left_width = cols / 2U;
    const size_t pane_height = rows - 3U;
    ftp_app_draw_pane(&ftp_app.panes[0], 0, 1, 0, pane_height, left_width);
    ftp_app_draw_pane(&ftp_app.panes[1], 1, 1, left_width,
                      pane_height, cols - left_width);
    const size_t message_row = rows - 2U;
    solar_os_tui_fill(&ftp_app.tui, message_row, 0, 1, cols, ' ',
                      SOLAR_OS_TUI_ATTR_NORMAL);
    if (ftp_app.input_mode == FTP_APP_INPUT_MKDIR) {
        char prompt[FTP_APP_INPUT_MAX + 12U];
        snprintf(prompt, sizeof(prompt), "mkdir: %s", ftp_app.input);
        solar_os_tui_write_cell(&ftp_app.tui, message_row, 0, cols,
                                prompt, SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_move(&ftp_app.tui, message_row, 7U + ftp_app.input_len);
    } else {
        solar_os_tui_write_cell(&ftp_app.tui, message_row, 0, cols,
                                ftp_app.message,
                                ftp_app.input_mode == FTP_APP_INPUT_DELETE ?
                                    SOLAR_OS_TUI_ATTR_BOLD : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    solar_os_tui_draw_help(&ftp_app.tui, "F3 View F5 Copy F6 Move F7 Mkdir F8 Del");
    ftp_app_draw_progress(rows, cols);
    solar_os_tui_refresh(&ftp_app.tui);
}

static ftp_app_entry_t *ftp_app_selected(void)
{
    ftp_app_pane_t *pane = &ftp_app.panes[ftp_app.active];
    return pane->cursor < pane->count ? &pane->entries[pane->cursor] : NULL;
}

static bool ftp_app_selected_path(char *path, size_t path_len)
{
    ftp_app_pane_t *pane = &ftp_app.panes[ftp_app.active];
    ftp_app_entry_t *entry = ftp_app_selected();
    if (entry == NULL) {
        return false;
    }
    if (entry->parent) {
        ftp_app_parent(pane->path, path, path_len);
        return true;
    }
    return ftp_app_join(path, path_len, pane->path, entry->name);
}

static bool ftp_app_submit(const ftp_app_request_t *request, const char *message)
{
    if (ftp_app.busy || xQueueSend(ftp_app.requests, request, 0) != pdTRUE) {
        ftp_app_set_message("FTP worker is busy");
        return false;
    }
    ftp_app.busy = true;
    ftp_app_set_message(message);
    return true;
}

static const char *ftp_app_default_viewer(const char *path)
{
    const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find_opener(path);
    if (entry != NULL) {
        return entry->name;
    }
    return solar_os_app_registry_find("less") != NULL ? "less" : "edit";
}

static void ftp_app_view_local(solar_os_context_t *ctx, const char *path)
{
    const char *name = ftp_app_default_viewer(path);
    const solar_os_app_registry_entry_t *entry = solar_os_app_registry_find(name);
    if (entry == NULL || entry->app == NULL) {
        ftp_app_set_message("viewer not available");
        return;
    }
    char app_arg[SOLAR_OS_APP_ARG_LEN];
    char path_arg[SOLAR_OS_APP_ARG_LEN];
    strlcpy(app_arg, name, sizeof(app_arg));
    strlcpy(path_arg, path, sizeof(path_arg));
    char *argv[] = {app_arg, path_arg};
    const esp_err_t err = solar_os_context_request_launch_ex(ctx,
                                                             entry->app,
                                                             2,
                                                             argv,
                                                             SOLAR_OS_LAUNCH_CHILD_RETURN);
    if (err != ESP_OK) {
        ftp_app_set_message(esp_err_to_name(err));
    }
}

static void ftp_app_restore_position(ftp_app_pane_t *pane,
                                     size_t cursor,
                                     size_t top)
{
    if (pane->count == 0) {
        pane->cursor = 0;
        pane->top = 0;
        return;
    }
    pane->cursor = cursor < pane->count ? cursor : pane->count - 1U;
    pane->top = top < pane->count ? top : pane->count - 1U;
}

static esp_err_t ftp_app_refresh_local(void)
{
    ftp_app_pane_t *pane = &ftp_app.panes[0];
    const size_t old_cursor = pane->cursor;
    const size_t old_top = pane->top;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(path, pane->path, sizeof(path));
    const esp_err_t err = ftp_app_load_local(pane, path);
    if (err == ESP_OK) {
        ftp_app_restore_position(pane, old_cursor, old_top);
    }
    return err;
}

static void ftp_app_poll(solar_os_context_t *ctx)
{
    bool redraw = false;
    ftp_app_progress_event_t progress;
    if (ftp_app.progress_events != NULL &&
        xQueueReceive(ftp_app.progress_events, &progress, 0) == pdTRUE) {
        ftp_app.progress.done = progress.done;
        ftp_app.progress.total = progress.total;
        ftp_app.progress.total_known = progress.total_known;
        strlcpy(ftp_app.progress.item,
                progress.item,
                sizeof(ftp_app.progress.item));
        redraw = true;
    }
    ftp_app_result_t result;
    while (ftp_app.results != NULL && xQueueReceive(ftp_app.results, &result, 0) == pdTRUE) {
        ftp_app.busy = false;
        ftp_app.progress.active = false;
        if (result.error == ESP_OK) {
            ftp_app_pane_t *remote = &ftp_app.panes[1];
            const bool same_remote_path = strcmp(remote->path, result.remote_path) == 0;
            const size_t old_remote_cursor = remote->cursor;
            const size_t old_remote_top = remote->top;
            solar_os_memory_free(remote->entries);
            remote->entries = result.remote_entries;
            remote->count = result.remote_count;
            remote->capacity = result.remote_count;
            remote->cursor = 0;
            remote->top = 0;
            strlcpy(remote->path, result.remote_path, sizeof(remote->path));
            if (same_remote_path) {
                ftp_app_restore_position(remote, old_remote_cursor, old_remote_top);
            }
            (void)ftp_app_refresh_local();
            ftp_app_set_message(result.kind == FTP_APP_WORK_CONNECT ? "connected" : "done");
            if (result.view_path[0] != '\0') {
                strlcpy(ftp_app.temporary_view,
                        result.view_path,
                        sizeof(ftp_app.temporary_view));
                ftp_app_view_local(ctx, result.view_path);
            }
        } else {
            solar_os_memory_free(result.remote_entries);
            char message[FTP_APP_MESSAGE_MAX];
            snprintf(message,
                     sizeof(message),
                     "%s%s%s",
                     esp_err_to_name(result.error),
                     result.detail[0] != '\0' ? ": " : "",
                     result.detail);
            ftp_app_set_message(message);
        }
        redraw = true;
    }
    if (redraw) {
        ftp_app_render();
    }
}

static void ftp_app_open_selected(void)
{
    ftp_app_entry_t *entry = ftp_app_selected();
    if (entry == NULL || !entry->is_directory) {
        return;
    }
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!ftp_app_selected_path(path, sizeof(path))) {
        return;
    }
    if (ftp_app.active == 0) {
        if (ftp_app_load_local(&ftp_app.panes[0], path) != ESP_OK) {
            ftp_app_set_message("cannot open local directory");
        }
    } else {
        const ftp_app_request_t request = {
            .kind = FTP_APP_WORK_LIST,
        };
        ftp_app_request_t work = request;
        strlcpy(work.source, path, sizeof(work.source));
        strlcpy(work.remote_directory, path, sizeof(work.remote_directory));
        (void)ftp_app_submit(&work, "loading remote directory...");
    }
}

static void ftp_app_transfer(bool move)
{
    ftp_app_entry_t *entry = ftp_app_selected();
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    if (entry == NULL || entry->parent || !ftp_app_selected_path(source, sizeof(source))) {
        ftp_app_set_message("select a file or directory");
        return;
    }
    ftp_app_request_t request = {
        .kind = move ? FTP_APP_WORK_MOVE : FTP_APP_WORK_COPY,
        .remote_source = ftp_app.active == 1,
        .is_directory = entry->is_directory,
        .size = entry->size,
    };
    strlcpy(request.source, source, sizeof(request.source));
    const ftp_app_pane_t *destination = &ftp_app.panes[ftp_app.active ^ 1U];
    if (!ftp_app_join(request.destination,
                      sizeof(request.destination),
                      destination->path,
                      entry->name)) {
        ftp_app_set_message("destination path is too long");
        return;
    }
    strlcpy(request.remote_directory,
            ftp_app.panes[1].path,
            sizeof(request.remote_directory));
    if (ftp_app_submit(&request, move ? "moving..." : "copying...")) {
        memset(&ftp_app.progress, 0, sizeof(ftp_app.progress));
        ftp_app.progress.active = true;
        ftp_app.progress.kind = request.kind;
        strlcpy(ftp_app.progress.item, entry->name, sizeof(ftp_app.progress.item));
    }
}

static void ftp_app_delete(void)
{
    ftp_app_entry_t *entry = ftp_app_selected();
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (entry == NULL || entry->parent || !ftp_app_selected_path(path, sizeof(path))) {
        ftp_app.input_mode = FTP_APP_INPUT_NONE;
        return;
    }
    ftp_app_request_t request = {
        .kind = FTP_APP_WORK_DELETE,
        .remote_source = ftp_app.active == 1,
        .is_directory = entry->is_directory,
    };
    strlcpy(request.source, path, sizeof(request.source));
    strlcpy(request.remote_directory,
            ftp_app.panes[1].path,
            sizeof(request.remote_directory));
    ftp_app.input_mode = FTP_APP_INPUT_NONE;
    (void)ftp_app_submit(&request, "deleting...");
}

static void ftp_app_create_directory(void)
{
    if (ftp_app.input_len == 0 || strchr(ftp_app.input, '/') != NULL) {
        ftp_app_set_message("enter one directory name");
        return;
    }
    ftp_app_request_t request = {
        .kind = FTP_APP_WORK_MKDIR,
        .remote_source = ftp_app.active == 1,
    };
    if (!ftp_app_join(request.source,
                      sizeof(request.source),
                      ftp_app.panes[ftp_app.active].path,
                      ftp_app.input)) {
        ftp_app_set_message("path is too long");
        return;
    }
    strlcpy(request.remote_directory,
            ftp_app.panes[1].path,
            sizeof(request.remote_directory));
    ftp_app.input_mode = FTP_APP_INPUT_NONE;
    ftp_app.input[0] = '\0';
    ftp_app.input_len = 0;
    (void)ftp_app_submit(&request, "creating directory...");
}

static bool ftp_app_parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool ftp_app_temporary_view_path(char *path,
                                        size_t path_len,
                                        const char *name)
{
    for (size_t attempt = 0; attempt < 256U; attempt++) {
        char temporary_name[SOLAR_OS_FTP_NAME_MAX + 32U];
        const uint32_t serial = ++ftp_app.temporary_serial;
        const int written = snprintf(temporary_name,
                                     sizeof(temporary_name),
                                     ".ftp-view-%" PRIu32 "-%s",
                                     serial,
                                     name);
        struct stat info;
        if (written > 0 && (size_t)written < sizeof(temporary_name) &&
            ftp_app_join(path,
                         path_len,
                         ftp_app.panes[0].path,
                         temporary_name) &&
            stat(path, &info) != 0 && errno == ENOENT) {
            return true;
        }
    }
    return false;
}

static esp_err_t ftp_app_parse_args(solar_os_context_t *ctx,
                                    char *local_path,
                                    char *remote_path)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(ftp_app.host, solar_os_context_argv(ctx, 1), sizeof(ftp_app.host));
    strlcpy(ftp_app.username, "anonymous", sizeof(ftp_app.username));
    strlcpy(ftp_app.password, "solaros@", sizeof(ftp_app.password));
    ftp_app.port = SOLAR_OS_FTP_DEFAULT_PORT;
    strlcpy(remote_path, "/", SOLAR_OS_STORAGE_PATH_MAX);
    strlcpy(local_path, ".", SOLAR_OS_STORAGE_PATH_MAX);
    bool user_set = false;
    bool password_set = false;
    bool port_set = false;
    for (int i = 2; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "--user") == 0 && i + 1 < argc && !user_set) {
            strlcpy(ftp_app.username,
                    solar_os_context_argv(ctx, ++i),
                    sizeof(ftp_app.username));
            user_set = true;
        } else if (strcmp(arg, "--password") == 0 && i + 1 < argc && !password_set) {
            strlcpy(ftp_app.password,
                    solar_os_context_argv(ctx, ++i),
                    sizeof(ftp_app.password));
            password_set = true;
        } else if (strcmp(arg, "--remote") == 0 && i + 1 < argc) {
            strlcpy(remote_path,
                    solar_os_context_argv(ctx, ++i),
                    SOLAR_OS_STORAGE_PATH_MAX);
        } else if (strcmp(arg, "--local") == 0 && i + 1 < argc) {
            strlcpy(local_path,
                    solar_os_context_argv(ctx, ++i),
                    SOLAR_OS_STORAGE_PATH_MAX);
        } else if (port_set || !ftp_app_parse_port(arg, &ftp_app.port)) {
            return ESP_ERR_INVALID_ARG;
        } else {
            port_set = true;
        }
    }
    return user_set == password_set ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t ftp_app_start(solar_os_context_t *ctx)
{
    memset(&ftp_app, 0, sizeof(ftp_app));
    char local_arg[SOLAR_OS_STORAGE_PATH_MAX];
    char remote_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = ftp_app_parse_args(ctx, local_arg, remote_path);
    if (err != ESP_OK) {
        return err;
    }
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_shell_resolve_path(ctx, local_arg, local_path, sizeof(local_path));
    if (err != ESP_OK || ftp_app_load_local(&ftp_app.panes[0], local_path) != ESP_OK) {
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    strlcpy(ftp_app.panes[1].path, remote_path, sizeof(ftp_app.panes[1].path));
    err = solar_os_tui_screen_begin(&ftp_app.tui, ctx);
    if (err != ESP_OK) {
        ftp_app_pane_clear(&ftp_app.panes[0]);
        return err;
    }
    ftp_app.requests = solar_os_queue_create(FTP_APP_QUEUE_LEN, sizeof(ftp_app_request_t));
    ftp_app.results = solar_os_queue_create(FTP_APP_QUEUE_LEN, sizeof(ftp_app_result_t));
    ftp_app.progress_events = solar_os_queue_create(1U, sizeof(ftp_app_progress_event_t));
    if (ftp_app.requests == NULL || ftp_app.results == NULL ||
        ftp_app.progress_events == NULL) {
        if (ftp_app.requests != NULL) {
            solar_os_queue_delete(ftp_app.requests);
            ftp_app.requests = NULL;
        }
        if (ftp_app.results != NULL) {
            solar_os_queue_delete(ftp_app.results);
            ftp_app.results = NULL;
        }
        if (ftp_app.progress_events != NULL) {
            solar_os_queue_delete(ftp_app.progress_events);
            ftp_app.progress_events = NULL;
        }
        solar_os_tui_end(&ftp_app.tui);
        ftp_app_pane_clear(&ftp_app.panes[0]);
        return ESP_ERR_NO_MEM;
    }
    ftp_app_request_t connect = {.kind = FTP_APP_WORK_CONNECT};
    strlcpy(connect.remote_directory, remote_path, sizeof(connect.remote_directory));
    if (xQueueSend(ftp_app.requests, &connect, 0) != pdTRUE ||
        solar_os_task_create_pinned(ftp_app_worker,
                                    "ftp_app",
                                    FTP_APP_TASK_STACK,
                                    NULL,
                                    FTP_APP_TASK_PRIORITY,
                                    &ftp_app.worker,
                                    tskNO_AFFINITY,
                                    SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        solar_os_queue_delete(ftp_app.requests);
        solar_os_queue_delete(ftp_app.results);
        solar_os_queue_delete(ftp_app.progress_events);
        ftp_app.requests = NULL;
        ftp_app.results = NULL;
        ftp_app.progress_events = NULL;
        solar_os_tui_end(&ftp_app.tui);
        ftp_app_pane_clear(&ftp_app.panes[0]);
        return ESP_ERR_NO_MEM;
    }
    ftp_app.busy = true;
    ftp_app_set_message("connecting...");
    ftp_app_render();
    return ESP_OK;
}

static void ftp_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    ftp_app.cancel_requested = true;
    if (ftp_app.requests != NULL && ftp_app.worker != NULL) {
        const ftp_app_request_t stop = {.kind = FTP_APP_WORK_STOP};
        (void)xQueueSend(ftp_app.requests, &stop, 0);
    }
    if (ftp_app.worker != NULL &&
        !solar_os_task_wait_done(ftp_app.worker,
                                 &ftp_app.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "FTP worker did not stop in time");
        return;
    }
    solar_os_tui_set_cursor_visible(&ftp_app.tui, true);
    ftp_app_release_cleanup();
}

static void ftp_app_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    if (ftp_app.temporary_view[0] != '\0') {
        (void)unlink(ftp_app.temporary_view);
        ftp_app.temporary_view[0] = '\0';
    }
    (void)ftp_app_refresh_local();
    ftp_app_render();
}

static bool ftp_app_input_event(uint8_t ch)
{
    if (ftp_app.input_mode == FTP_APP_INPUT_DELETE) {
        if (ch == 'y' || ch == 'Y') {
            ftp_app_delete();
        } else {
            ftp_app.input_mode = FTP_APP_INPUT_NONE;
            ftp_app_set_message("");
        }
        return true;
    }
    if (ftp_app.input_mode != FTP_APP_INPUT_MKDIR) {
        return false;
    }
    if (ch == SOLAR_OS_KEY_ESCAPE) {
        ftp_app.input_mode = FTP_APP_INPUT_NONE;
        ftp_app_set_message("");
    } else if (ch == '\r' || ch == '\n') {
        ftp_app_create_directory();
    } else if (ch == '\b' || ch == 0x7fU) {
        if (ftp_app.input_len > 0) {
            ftp_app.input[--ftp_app.input_len] = '\0';
        }
    } else if (isprint(ch) && ftp_app.input_len + 1U < sizeof(ftp_app.input)) {
        ftp_app.input[ftp_app.input_len++] = (char)ch;
        ftp_app.input[ftp_app.input_len] = '\0';
    }
    return true;
}

static bool ftp_app_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        ftp_app_poll(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }
    const uint8_t ch = (uint8_t)event->data.ch;
    if (ftp_app.busy) {
        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_F10) {
            solar_os_context_finish(ctx, 0, NULL);
        }
        return true;
    }
    if (ftp_app_input_event(ch)) {
        ftp_app_render();
        return true;
    }
    ftp_app_pane_t *pane = &ftp_app.panes[ftp_app.active];
    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
    case SOLAR_OS_KEY_APP_EXIT:
    case SOLAR_OS_KEY_F10:
    case 'q':
    case 'Q':
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    case '\t':
    case SOLAR_OS_KEY_LEFT:
    case SOLAR_OS_KEY_RIGHT:
        ftp_app.active ^= 1U;
        break;
    case SOLAR_OS_KEY_UP:
    case 'k':
        if (pane->cursor > 0) {
            pane->cursor--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        if (pane->cursor + 1U < pane->count) {
            pane->cursor++;
        }
        break;
    case SOLAR_OS_KEY_HOME:
        pane->cursor = 0;
        break;
    case SOLAR_OS_KEY_END:
        pane->cursor = pane->count > 0 ? pane->count - 1U : 0;
        break;
    case '\r':
    case '\n':
        ftp_app_open_selected();
        break;
    case SOLAR_OS_KEY_F3:
    case 'v':
    case 'V': {
        ftp_app_entry_t *entry = ftp_app_selected();
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        if (entry != NULL && !entry->is_directory &&
            ftp_app_selected_path(path, sizeof(path))) {
            if (ftp_app.active == 0) {
                ftp_app_view_local(ctx, path);
            } else {
                ftp_app_request_t request = {
                    .kind = FTP_APP_WORK_VIEW,
                    .remote_source = true,
                };
                strlcpy(request.source, path, sizeof(request.source));
                if (ftp_app_temporary_view_path(request.destination,
                                                sizeof(request.destination),
                                                entry->name)) {
                    strlcpy(request.remote_directory,
                            ftp_app.panes[1].path,
                            sizeof(request.remote_directory));
                    (void)ftp_app_submit(&request, "downloading for view...");
                }
            }
        }
        break;
    }
    case SOLAR_OS_KEY_F5:
    case 'c':
    case 'C':
        ftp_app_transfer(false);
        break;
    case SOLAR_OS_KEY_F6:
    case 'm':
    case 'M':
        ftp_app_transfer(true);
        break;
    case SOLAR_OS_KEY_F7:
    case 'n':
    case 'N':
        ftp_app.input_mode = FTP_APP_INPUT_MKDIR;
        ftp_app.input[0] = '\0';
        ftp_app.input_len = 0;
        break;
    case SOLAR_OS_KEY_F8:
    case SOLAR_OS_KEY_DELETE:
    case 'd':
    case 'D': {
        ftp_app_entry_t *entry = ftp_app_selected();
        if (entry != NULL && !entry->parent) {
            ftp_app.input_mode = FTP_APP_INPUT_DELETE;
            char message[FTP_APP_MESSAGE_MAX];
            snprintf(message, sizeof(message), "delete %s? y/N", entry->name);
            ftp_app_set_message(message);
        }
        break;
    }
    case 'r':
    case 'R': {
        ftp_app_request_t request = {.kind = FTP_APP_WORK_LIST};
        strlcpy(request.source, ftp_app.panes[1].path, sizeof(request.source));
        strlcpy(request.remote_directory,
                ftp_app.panes[1].path,
                sizeof(request.remote_directory));
        (void)ftp_app_submit(&request, "refreshing...");
        break;
    }
    default:
        return true;
    }
    ftp_app_render();
    return true;
}

static bool ftp_app_release_ready(void)
{
    return ftp_app_state == NULL || ftp_app.worker == NULL || ftp_app.worker_done;
}

static void ftp_app_release_cleanup(void)
{
    if (ftp_app_state == NULL) {
        return;
    }
    if (ftp_app.temporary_view[0] != '\0') {
        (void)unlink(ftp_app.temporary_view);
    }
    if (ftp_app.requests != NULL) {
        solar_os_queue_delete(ftp_app.requests);
        ftp_app.requests = NULL;
    }
    if (ftp_app.results != NULL) {
        ftp_app_result_t result;
        while (xQueueReceive(ftp_app.results, &result, 0) == pdTRUE) {
            solar_os_memory_free(result.remote_entries);
        }
        solar_os_queue_delete(ftp_app.results);
        ftp_app.results = NULL;
    }
    if (ftp_app.progress_events != NULL) {
        solar_os_queue_delete(ftp_app.progress_events);
        ftp_app.progress_events = NULL;
    }
    solar_os_tui_end(&ftp_app.tui);
    ftp_app_pane_clear(&ftp_app.panes[0]);
    ftp_app_pane_clear(&ftp_app.panes[1]);
    memset(ftp_app.password, 0, sizeof(ftp_app.password));
}

const solar_os_app_t solar_os_ftp_app = {
    .name = "ftp",
    .summary = "two-pane FTP file manager",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = ftp_app_start,
    .resume = ftp_app_resume,
    .stop = ftp_app_stop,
    .event = ftp_app_event,
    .state_slot = &ftp_app_state,
    .state_size = sizeof(ftp_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_ready = ftp_app_release_ready,
    .state_release_cleanup = ftp_app_release_cleanup,
    .worker_stack_bytes = FTP_APP_TASK_STACK,
};
