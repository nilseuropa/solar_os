#include "solar_os_log_job.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_port.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"

#define LOG_JOB_POLL_MS 250U
#define LOG_JOB_BATCH 8U
#define LOG_JOB_LINE_MAX 256U
#define LOG_JOB_WORKER_STACK 6144U
#define LOG_JOB_WORKER_PRIORITY (tskIDLE_PRIORITY + 1)
#define LOG_JOB_NOTIFY_POLL (1U << 0)
#define LOG_JOB_NOTIFY_STOP (1U << 1)

static const char *TAG = "solar_os_log_job";

typedef enum {
    LOG_JOB_TARGET_NONE,
    LOG_JOB_TARGET_PORT,
    LOG_JOB_TARGET_FILE,
} log_job_target_t;

typedef struct {
    bool running;
    log_job_target_t target;
    solar_os_port_handle_t port;
    FILE *file;
    char target_name[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_log_level_t level;
    uint32_t last_sequence;
    uint32_t next_poll_ms;
    uint32_t written_entries;
    uint32_t failed_writes;
    esp_err_t last_error;
    TaskHandle_t worker_task;
    volatile bool worker_done;
} log_job_state_t;

static log_job_state_t log_job = {
    .port = SOLAR_OS_PORT_HANDLE_INIT,
    .last_error = ESP_OK,
};

static void log_job_worker_task(void *arg);

static char log_job_level_letter(solar_os_log_level_t level)
{
    switch (level) {
    case SOLAR_OS_LOG_LEVEL_ERROR:
        return 'E';
    case SOLAR_OS_LOG_LEVEL_WARN:
        return 'W';
    case SOLAR_OS_LOG_LEVEL_DEBUG:
        return 'D';
    case SOLAR_OS_LOG_LEVEL_INFO:
    default:
        return 'I';
    }
}

static uint32_t log_job_latest_sequence(void)
{
    solar_os_log_entry_t entry;
    size_t total = 0;
    const size_t copied = solar_os_log_snapshot(&entry, 1, &total);
    return copied > 0 ? entry.sequence : 0;
}

static void log_job_cleanup(void)
{
    if (log_job.file != NULL) {
        fclose(log_job.file);
        log_job.file = NULL;
    }
    if (solar_os_port_handle_valid(&log_job.port)) {
        (void)solar_os_port_release(&log_job.port);
    }

    log_job.running = false;
    log_job.worker_task = NULL;
    log_job.worker_done = false;
    log_job.target = LOG_JOB_TARGET_NONE;
    log_job.target_name[0] = '\0';
    log_job.next_poll_ms = 0;
}

static esp_err_t log_job_format_entry(const solar_os_log_entry_t *entry,
                                      bool crlf,
                                      char *line,
                                      size_t line_len)
{
    if (entry == NULL || line == NULL || line_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t seconds = entry->timestamp_ms / 1000U;
    const uint32_t ms = entry->timestamp_ms % 1000U;
    const int written = snprintf(line,
                                 line_len,
                                 "%06" PRIu32 " %5" PRIu32 ".%03" PRIu32 " %c %-16s %s%s%s",
                                 entry->sequence,
                                 seconds,
                                 ms,
                                 log_job_level_letter(entry->level),
                                 entry->tag,
                                 entry->message,
                                 entry->truncated ? "..." : "",
                                 crlf ? "\r\n" : "\n");
    if (written < 0) {
        return ESP_FAIL;
    }
    return (size_t)written < line_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t log_job_write_port(const char *line, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t written = 0;
        const esp_err_t err = solar_os_port_write(&log_job.port,
                                                  (const uint8_t *)&line[offset],
                                                  len - offset,
                                                  &written);
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_FAIL;
        }
        offset += written;
    }
    return ESP_OK;
}

static esp_err_t log_job_write_file(const char *line, size_t len)
{
    if (log_job.file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return fwrite(line, 1, len, log_job.file) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t log_job_write_entry(const solar_os_log_entry_t *entry)
{
    char line[LOG_JOB_LINE_MAX];
    const bool port_target = log_job.target == LOG_JOB_TARGET_PORT;
    esp_err_t err = log_job_format_entry(entry, port_target, line, sizeof(line));
    if (err != ESP_OK) {
        return err;
    }

    const size_t len = strlen(line);
    if (port_target) {
        return log_job_write_port(line, len);
    }
    if (log_job.target == LOG_JOB_TARGET_FILE) {
        return log_job_write_file(line, len);
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t log_job_start_port(const char *port_name)
{
    solar_os_port_info_t info;
    esp_err_t err = solar_os_port_get_info(port_name, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (info.claimed) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((info.capabilities & SOLAR_OS_PORT_CAP_WRITE) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = solar_os_jobs_claim_port(solar_os_log_job.name, port_name, &log_job.port);
    if (err != ESP_OK) {
        return err;
    }

    log_job.target = LOG_JOB_TARGET_PORT;
    strlcpy(log_job.target_name, port_name, sizeof(log_job.target_name));
    return ESP_OK;
}

static esp_err_t log_job_start_file(const char *path_arg)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_resolve_path(path_arg, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(path, "a");
    if (file == NULL) {
        return ESP_FAIL;
    }

    log_job.file = file;
    log_job.target = LOG_JOB_TARGET_FILE;
    strlcpy(log_job.target_name, path, sizeof(log_job.target_name));
    (void)solar_os_jobs_note_resource(solar_os_log_job.name,
                                      SOLAR_OS_JOB_RESOURCE_FILE,
                                      path,
                                      "append");
    return ESP_OK;
}

static bool log_job_parse_level_arg(const char *text, solar_os_log_level_t *level)
{
    if (text == NULL) {
        return true;
    }
    return solar_os_log_parse_level(text, level);
}

static esp_err_t log_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    if (log_job.worker_task != NULL && !log_job.worker_done) {
        return ESP_ERR_INVALID_STATE;
    }

    if (argc < 2 || argv == NULL || argv[1] == NULL || argv[1][0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (log_job.running || solar_os_port_handle_valid(&log_job.port) || log_job.file != NULL) {
        log_job_cleanup();
    }

    solar_os_log_status_t status;
    if (solar_os_log_get_status(&status) == ESP_OK) {
        log_job.level = status.level;
    } else {
        log_job.level = SOLAR_OS_LOG_LEVEL_INFO;
    }

    esp_err_t err = ESP_OK;
    if (strcmp(argv[1], "file") == 0) {
        if (argc < 3 || argc > 4 || argv[2] == NULL || argv[2][0] == '\0' ||
            !log_job_parse_level_arg(argc == 4 ? argv[3] : NULL, &log_job.level)) {
            return ESP_ERR_INVALID_ARG;
        }
        err = log_job_start_file(argv[2]);
    } else {
        if (argc > 3 || !log_job_parse_level_arg(argc == 3 ? argv[2] : NULL, &log_job.level)) {
            return ESP_ERR_INVALID_ARG;
        }
        err = log_job_start_port(argv[1]);
    }

    if (err != ESP_OK) {
        log_job_cleanup();
        log_job.last_error = err;
        return err;
    }

    log_job.running = true;
    log_job.last_sequence = log_job_latest_sequence();
    log_job.next_poll_ms = 0;
    log_job.written_entries = 0;
    log_job.failed_writes = 0;
    log_job.last_error = ESP_OK;
    log_job.worker_done = false;
    log_job.worker_task = NULL;

    /* Flash-backed VFS calls require an internal stack while cache is disabled. */
    if (solar_os_task_create_pinned_internal(log_job_worker_task,
                                             "log_worker",
                                             LOG_JOB_WORKER_STACK,
                                             NULL,
                                             LOG_JOB_WORKER_PRIORITY,
                                             &log_job.worker_task,
                                             tskNO_AFFINITY) != pdPASS) {
        log_job.last_error = ESP_ERR_NO_MEM;
        log_job_cleanup();
        return ESP_ERR_NO_MEM;
    }

    SOLAR_OS_LOGI(TAG,
                  "started: target=%s level=%s",
                  log_job.target_name,
                  solar_os_log_level_name(log_job.level));
    return ESP_OK;
}

static void log_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (!log_job.running &&
        log_job.file == NULL &&
        !solar_os_port_handle_valid(&log_job.port)) {
        return;
    }

    char target_copy[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(target_copy, log_job.target_name, sizeof(target_copy));
    const uint32_t written_entries = log_job.written_entries;
    const uint32_t failed_writes = log_job.failed_writes;

    log_job.running = false;
    if (log_job.worker_task != NULL) {
        (void)xTaskNotify(log_job.worker_task, LOG_JOB_NOTIFY_STOP, eSetBits);
    }
    if (!solar_os_task_wait_done(log_job.worker_task,
                                 &log_job.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "worker did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }

    if (log_job.file != NULL) {
        (void)fflush(log_job.file);
    }
    log_job_cleanup();

    SOLAR_OS_LOGI(TAG,
                  "stopped: target=%s written=%" PRIu32 " failed=%" PRIu32,
                  target_copy[0] != '\0' ? target_copy : "?",
                  written_entries,
                  failed_writes);
}

static void log_job_poll(void)
{
    solar_os_log_entry_t entries[LOG_JOB_BATCH];
    size_t available = 0;
    const size_t copied = solar_os_log_snapshot_since(log_job.last_sequence,
                                                      log_job.level,
                                                      entries,
                                                      sizeof(entries) / sizeof(entries[0]),
                                                      &available);
    if (copied == 0) {
        return;
    }

    for (size_t i = 0; i < copied; i++) {
        const esp_err_t err = log_job_write_entry(&entries[i]);
        if (err != ESP_OK) {
            log_job.last_error = err;
            log_job.failed_writes++;
            return;
        }

        log_job.last_sequence = entries[i].sequence;
        log_job.written_entries++;
    }

    if (log_job.file != NULL) {
        (void)fflush(log_job.file);
    }
    log_job.last_error = ESP_OK;
}

static void log_job_worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t notification = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY);
        if ((notification & LOG_JOB_NOTIFY_STOP) != 0) {
            break;
        }
        if ((notification & LOG_JOB_NOTIFY_POLL) != 0 && log_job.running) {
            log_job_poll();
        }
    }

    log_job.worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static bool log_job_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (!log_job.running || event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }

    const uint32_t now_ms = event->data.tick_ms;
    if (log_job.next_poll_ms != 0 &&
        (int32_t)(now_ms - log_job.next_poll_ms) < 0) {
        return false;
    }
    log_job.next_poll_ms = now_ms + LOG_JOB_POLL_MS;
    if (log_job.worker_task == NULL) {
        log_job.failed_writes++;
        log_job.last_error = ESP_ERR_INVALID_STATE;
        return false;
    }
    return xTaskNotify(log_job.worker_task, LOG_JOB_NOTIFY_POLL, eSetBits) == pdPASS;
}

const solar_os_job_t solar_os_log_job = {
    .name = "log",
    .summary = "stream SolarOS logs to a port or file",
    .start = log_job_start,
    .stop = log_job_stop,
    .event = log_job_event,
    .tick_interval_ms = LOG_JOB_POLL_MS,
    .tick_deadline_ms = 2U,
};
