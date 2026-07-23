#include "solar_os_daq_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_storage.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"
#include "solar_os_time.h"

#define DAQ_DEFAULT_SCALAR_INTERVAL_MS 1000U
#define DAQ_DEFAULT_BYTE_INTERVAL_MS 25U
#define DAQ_MAX_INTERVAL_MS 86400000U
#define DAQ_RAW_READ_MAX 512U
#define DAQ_MAX_STREAMS (SOLAR_OS_APP_ARG_MAX - 2)
#define DAQ_STREAM_LIST_MAX SOLAR_OS_DAQ_STREAM_LIST_MAX
#define DAQ_CSV_HEADER_MAX 512U
#define DAQ_CSV_LINE_MAX 512U
#define DAQ_WORKER_STACK 8192U
#define DAQ_WORKER_PRIORITY (tskIDLE_PRIORITY + 1)
#define DAQ_NOTIFY_SAMPLE (1U << 0)
#define DAQ_NOTIFY_STOP (1U << 1)

static const char *TAG = "solar_os_daq";

typedef struct {
    solar_os_stream_info_t infos[DAQ_MAX_STREAMS];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    size_t stream_count;
    uint32_t interval_ms;
    bool change_only;
    bool append;
    bool raw;
} daq_start_config_t;

typedef struct {
    bool running;
    FILE *file;
    solar_os_stream_handle_t streams[DAQ_MAX_STREAMS];
    solar_os_stream_info_t infos[DAQ_MAX_STREAMS];
    size_t stream_count;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char stream_list[DAQ_STREAM_LIST_MAX];
    char last_change_key[SOLAR_OS_STREAM_CHANGE_KEY_MAX];
    uint32_t interval_ms;
    uint32_t next_sample_ms;
    uint32_t written_records;
    uint64_t written_bytes;
    uint32_t skipped_records;
    uint32_t failed_records;
    bool change_only;
    bool append;
    bool raw;
    esp_err_t last_error;
    TaskHandle_t worker_task;
    volatile bool worker_done;
} daq_job_state_t;

static daq_job_state_t daq = {.last_error = ESP_OK};

static void daq_worker_task(void *arg);

static bool daq_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        parsed < min ||
        parsed > max) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static void daq_init_stream_handles(void)
{
    for (size_t i = 0; i < DAQ_MAX_STREAMS; i++) {
        daq.streams[i] = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    }
}

static bool daq_append_text(char *buffer, size_t buffer_len, const char *text)
{
    if (buffer == NULL || text == NULL || buffer_len == 0) {
        return false;
    }

    const size_t used = strlen(buffer);
    if (used >= buffer_len) {
        return false;
    }

    const int written = snprintf(&buffer[used], buffer_len - used, "%s", text);
    return written >= 0 && (size_t)written < buffer_len - used;
}

static bool daq_append_comma_text(char *buffer, size_t buffer_len, const char *text)
{
    return daq_append_text(buffer, buffer_len, ",") &&
        daq_append_text(buffer, buffer_len, text);
}

static void daq_build_stream_list(const solar_os_stream_info_t *infos,
                                  size_t count,
                                  char *buffer,
                                  size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    buffer[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (i > 0 && !daq_append_text(buffer, buffer_len, ",")) {
            return;
        }
        if (!daq_append_text(buffer, buffer_len, infos[i].id)) {
            return;
        }
    }
}

static esp_err_t daq_flush_to_disk(FILE *file)
{
    if (file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fflush(file) != 0) {
        return ESP_FAIL;
    }

    const int fd = fileno(file);
    if (fd < 0) {
        return ESP_FAIL;
    }
    return fsync(fd) == 0 ? ESP_OK : ESP_FAIL;
}

static void daq_cleanup(void)
{
    if (daq.file != NULL) {
        (void)daq_flush_to_disk(daq.file);
        fclose(daq.file);
        daq.file = NULL;
    }
    for (size_t i = 0; i < DAQ_MAX_STREAMS; i++) {
        solar_os_stream_close(&daq.streams[i]);
    }
    daq.running = false;
    daq.worker_task = NULL;
    daq.worker_done = false;
    daq.next_sample_ms = 0;
    daq.stream_count = 0;
    daq_init_stream_handles();
}

static esp_err_t daq_parse_args(int argc, char **argv, daq_start_config_t *config)
{
    if (argc < 3 || argv == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->append = true;

    char *positionals[SOLAR_OS_APP_ARG_MAX];
    size_t positional_count = 0;
    bool interval_set = false;

    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL || argv[i][0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(argv[i], "--raw") == 0) {
            config->raw = true;
            continue;
        }
        if (strcmp(argv[i], "--changes") == 0) {
            config->change_only = true;
            continue;
        }
        if (strcmp(argv[i], "--append") == 0) {
            config->append = true;
            continue;
        }
        if (strcmp(argv[i], "--replace") == 0) {
            config->append = false;
            continue;
        }
        if (strcmp(argv[i], "--rate-ms") == 0) {
            if (i + 1 >= argc ||
                !daq_parse_u32(argv[++i], 0, DAQ_MAX_INTERVAL_MS, &config->interval_ms)) {
                return ESP_ERR_INVALID_ARG;
            }
            interval_set = true;
            continue;
        }
        if (strcmp(argv[i], "--rate") == 0) {
            uint32_t seconds = 0;
            if (i + 1 >= argc || !daq_parse_u32(argv[++i], 1, 86400, &seconds)) {
                return ESP_ERR_INVALID_ARG;
            }
            config->interval_ms = seconds * 1000U;
            interval_set = true;
            continue;
        }
        if (argv[i][0] == '-') {
            return ESP_ERR_INVALID_ARG;
        }
        if (positional_count >= SOLAR_OS_APP_ARG_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        positionals[positional_count++] = argv[i];
    }

    if (positional_count < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_stream_info_t first_info;
    const bool stream_first = solar_os_stream_get_info(positionals[0], &first_info) == ESP_OK;
    const size_t stream_start = stream_first ? 0U : 1U;
    const size_t stream_end = stream_first ? positional_count - 1U : positional_count;
    const char *path_arg = stream_first ? positionals[positional_count - 1U] : positionals[0];

    if (stream_end <= stream_start || stream_end - stream_start > DAQ_MAX_STREAMS) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = solar_os_storage_resolve_path(path_arg, config->path, sizeof(config->path));
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = stream_start; i < stream_end; i++) {
        err = solar_os_stream_get_info(positionals[i], &config->infos[config->stream_count]);
        if (err != ESP_OK) {
            return err;
        }
        config->stream_count++;
    }

    if (!interval_set) {
        config->interval_ms =
            config->stream_count == 1 &&
            config->infos[0].type == SOLAR_OS_STREAM_TYPE_BYTES ?
            DAQ_DEFAULT_BYTE_INTERVAL_MS :
            DAQ_DEFAULT_SCALAR_INTERVAL_MS;
    }

    if (config->raw &&
        (config->stream_count != 1 ||
         config->infos[0].type != SOLAR_OS_STREAM_TYPE_BYTES ||
         config->change_only)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!config->raw && config->stream_count > 1) {
        if (config->change_only) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        for (size_t i = 0; i < config->stream_count; i++) {
            if (config->infos[i].type == SOLAR_OS_STREAM_TYPE_BYTES) {
                return ESP_ERR_NOT_SUPPORTED;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t daq_stream_value_header(const solar_os_stream_info_t *info,
                                         char *header,
                                         size_t header_len)
{
    if (info == NULL || header == NULL || header_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (info->type == SOLAR_OS_STREAM_TYPE_EVENT) {
        return snprintf(header, header_len, "%s_value", info->id) < (int)header_len ?
            ESP_OK :
            ESP_ERR_INVALID_SIZE;
    }
    if (strncmp(info->id, "adc", 3) == 0) {
        return snprintf(header, header_len, "%s_raw,%s_mv", info->id, info->id) < (int)header_len ?
            ESP_OK :
            ESP_ERR_INVALID_SIZE;
    }
    if (strncmp(info->id, "mic", 3) == 0) {
        return snprintf(header,
                        header_len,
                        "%s_peak_percent,%s_avg_percent",
                        info->id,
                        info->id) < (int)header_len ?
            ESP_OK :
            ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(info->id, "battery") == 0) {
        strlcpy(header, "battery_voltage_v,battery_percent", header_len);
        return strlen(header) + 1 < header_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(info->id, "temperature") == 0) {
        strlcpy(header, "temperature_c", header_len);
        return strlen(header) + 1 < header_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(info->id, "humidity") == 0) {
        strlcpy(header, "humidity_percent", header_len);
        return strlen(header) + 1 < header_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    return snprintf(header, header_len, "%s_value", info->id) < (int)header_len ?
        ESP_OK :
        ESP_ERR_INVALID_SIZE;
}

static esp_err_t daq_csv_header(const solar_os_stream_info_t *infos,
                                size_t stream_count,
                                char *header,
                                size_t header_len)
{
    if (infos == NULL || stream_count == 0 || header == NULL || header_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stream_count == 1) {
        return solar_os_stream_csv_header(&infos[0], header, header_len);
    }

    strlcpy(header, "time_ms,uptime_ms", header_len);
    for (size_t i = 0; i < stream_count; i++) {
        char fields[96];
        esp_err_t err = daq_stream_value_header(&infos[i], fields, sizeof(fields));
        if (err != ESP_OK) {
            return err;
        }
        if (!daq_append_comma_text(header, header_len, fields)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return ESP_OK;
}

static esp_err_t daq_write_header_if_needed(FILE *file,
                                            const solar_os_stream_info_t *infos,
                                            size_t stream_count,
                                            bool append)
{
    if (file == NULL || infos == NULL || stream_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool write_header = !append;
    if (append) {
        if (fseek(file, 0, SEEK_END) != 0) {
            return ESP_FAIL;
        }
        const long pos = ftell(file);
        if (pos < 0) {
            return ESP_FAIL;
        }
        write_header = pos == 0;
    }

    if (!write_header) {
        return ESP_OK;
    }

    char header[DAQ_CSV_HEADER_MAX];
    esp_err_t err = daq_csv_header(infos, stream_count, header, sizeof(header));
    if (err != ESP_OK) {
        return err;
    }

    if (fprintf(file, "%s\n", header) < 0) {
        return ESP_FAIL;
    }
    return daq_flush_to_disk(file);
}

static esp_err_t daq_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    if (daq.worker_task != NULL && !daq.worker_done) {
        return ESP_ERR_INVALID_STATE;
    }

    daq_start_config_t config;
    esp_err_t err = daq_parse_args(argc, argv, &config);
    if (err != ESP_OK) {
        daq.last_error = err;
        return err;
    }

    if (daq.running || daq.file != NULL || daq.stream_count > 0) {
        daq_cleanup();
    }

    FILE *file = fopen(config.path,
                       config.raw ? (config.append ? "ab" : "wb") :
                       (config.append ? "a+" : "w"));
    if (file == NULL) {
        daq.last_error = ESP_FAIL;
        return ESP_FAIL;
    }

    daq_init_stream_handles();
    for (size_t i = 0; i < config.stream_count; i++) {
        err = solar_os_stream_open(config.infos[i].id, "daq", &daq.streams[i]);
        if (err != ESP_OK) {
            for (size_t j = 0; j < i; j++) {
                solar_os_stream_close(&daq.streams[j]);
            }
            fclose(file);
            daq.last_error = err;
            return err;
        }
    }

    if (!config.raw) {
        err = daq_write_header_if_needed(file,
                                         config.infos,
                                         config.stream_count,
                                         config.append);
        if (err != ESP_OK) {
            fclose(file);
            for (size_t i = 0; i < config.stream_count; i++) {
                solar_os_stream_close(&daq.streams[i]);
            }
            daq.last_error = err;
            return err;
        }
    }

    memset(daq.infos, 0, sizeof(daq.infos));
    for (size_t i = 0; i < config.stream_count; i++) {
        daq.infos[i] = config.infos[i];
    }
    daq.stream_count = config.stream_count;
    daq.file = file;
    strlcpy(daq.path, config.path, sizeof(daq.path));
    daq_build_stream_list(daq.infos, daq.stream_count, daq.stream_list, sizeof(daq.stream_list));
    daq.last_change_key[0] = '\0';
    daq.interval_ms = config.interval_ms;
    daq.next_sample_ms = 0;
    daq.written_records = 0;
    daq.written_bytes = 0;
    daq.skipped_records = 0;
    daq.failed_records = 0;
    daq.change_only = config.change_only;
    daq.append = config.append;
    daq.raw = config.raw;
    daq.last_error = ESP_OK;
    daq.running = true;
    daq.worker_done = false;
    daq.worker_task = NULL;
    (void)solar_os_jobs_note_resource(solar_os_daq_job.name,
                                      SOLAR_OS_JOB_RESOURCE_FILE,
                                      daq.path,
                                      daq.raw ? "raw" : "csv");
    (void)solar_os_jobs_note_resource(solar_os_daq_job.name,
                                      SOLAR_OS_JOB_RESOURCE_STREAM,
                                      daq.stream_list,
                                      daq.raw ? "bytes" : "scalar");

    /* Flash-backed VFS calls require an internal stack while cache is disabled. */
    if (solar_os_task_create_pinned_internal(daq_worker_task,
                                             "daq_worker",
                                             DAQ_WORKER_STACK,
                                             NULL,
                                             DAQ_WORKER_PRIORITY,
                                             &daq.worker_task,
                                             tskNO_AFFINITY) != pdPASS) {
        daq.last_error = ESP_ERR_NO_MEM;
        daq_cleanup();
        return ESP_ERR_NO_MEM;
    }

    SOLAR_OS_LOGI(TAG,
                  "started: %s -> %s interval=%" PRIu32 "ms%s%s",
                  daq.stream_list,
                  daq.path,
                  daq.interval_ms,
                  daq.raw ? " raw" : "",
                  daq.change_only ? " changes" : "");
    return ESP_OK;
}

static void daq_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    daq.running = false;
    if (daq.worker_task != NULL) {
        (void)xTaskNotify(daq.worker_task, DAQ_NOTIFY_STOP, eSetBits);
    }
    if (!solar_os_task_wait_done(daq.worker_task,
                                 &daq.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "worker did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }

    SOLAR_OS_LOGI(TAG,
                  "stopped: %s records=%" PRIu32 " bytes=%" PRIu64
                  " skipped=%" PRIu32 " failed=%" PRIu32,
                  daq.stream_list,
                  daq.written_records,
                  daq.written_bytes,
                  daq.skipped_records,
                  daq.failed_records);
    daq_cleanup();
}

static bool daq_should_sample(uint32_t now_ms)
{
    if (!daq.running) {
        return false;
    }
    if (daq.interval_ms == 0) {
        return true;
    }
    if (daq.next_sample_ms != 0 && (int32_t)(now_ms - daq.next_sample_ms) < 0) {
        return false;
    }

    daq.next_sample_ms = now_ms + daq.interval_ms;
    return true;
}

static bool daq_event_raw(void)
{
    if (!solar_os_port_handle_valid(&daq.streams[0].port)) {
        daq.failed_records++;
        daq.last_error = ESP_ERR_INVALID_STATE;
        return true;
    }

    uint8_t data[DAQ_RAW_READ_MAX];
    size_t read_len = 0;
    const esp_err_t err = solar_os_port_read(&daq.streams[0].port,
                                             data,
                                             sizeof(data),
                                             0,
                                             &read_len);
    if (err == ESP_ERR_TIMEOUT) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }
    if (err != ESP_OK) {
        daq.failed_records++;
        daq.last_error = err;
        SOLAR_OS_LOGW(TAG, "raw read failed: %s", esp_err_to_name(err));
        return true;
    }
    if (read_len == 0) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }

    if (fwrite(data, 1, read_len, daq.file) != read_len ||
        daq_flush_to_disk(daq.file) != ESP_OK) {
        daq.failed_records++;
        daq.last_error = ESP_FAIL;
        SOLAR_OS_LOGW(TAG, "raw write failed");
        return true;
    }

    daq.written_records++;
    daq.written_bytes += read_len;
    daq.last_error = ESP_OK;
    return true;
}

static const char *daq_csv_value_fields(const char *line)
{
    if (line == NULL) {
        return NULL;
    }

    unsigned commas = 0;
    for (const char *p = line; *p != '\0'; p++) {
        if (*p == ',') {
            commas++;
            if (commas == 3) {
                return p + 1;
            }
        }
    }
    return NULL;
}

static bool daq_csv_timestamp_prefix(char *line, size_t line_len)
{
    uint64_t time_ms = 0;
    const uint64_t uptime_ms = solar_os_time_uptime_ms();
    const bool time_valid = solar_os_time_get_utc_epoch_ms(&time_ms) == ESP_OK;

    int written = 0;
    if (time_valid) {
        written = snprintf(line, line_len, "%" PRIu64 ",%" PRIu64, time_ms, uptime_ms);
    } else {
        written = snprintf(line, line_len, ",%" PRIu64, uptime_ms);
    }

    return written >= 0 && (size_t)written < line_len;
}

static bool daq_event_csv_single(void)
{
    solar_os_stream_csv_record_t record;
    const solar_os_stream_read_options_t options = {
        .window_ms = daq.infos[0].id[0] == 'm' ? 100U : 0U,
        .timeout_ms = 0,
    };
    const esp_err_t err = solar_os_stream_read_csv(&daq.streams[0], &options, &record);
    if (err == ESP_ERR_TIMEOUT && !record.has_data) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }
    if (err != ESP_OK) {
        daq.failed_records++;
        daq.last_error = err;
        SOLAR_OS_LOGW(TAG, "sample failed: %s", esp_err_to_name(err));
        return true;
    }

    if (daq.change_only &&
        daq.last_change_key[0] != '\0' &&
        strcmp(daq.last_change_key, record.change_key) == 0) {
        daq.skipped_records++;
        daq.last_error = ESP_OK;
        return true;
    }

    if (fprintf(daq.file, "%s\n", record.line) < 0 ||
        daq_flush_to_disk(daq.file) != ESP_OK) {
        daq.failed_records++;
        daq.last_error = ESP_FAIL;
        SOLAR_OS_LOGW(TAG, "write failed");
        return true;
    }

    strlcpy(daq.last_change_key, record.change_key, sizeof(daq.last_change_key));
    daq.written_records++;
    daq.last_error = ESP_OK;
    return true;
}

static bool daq_event_csv_multi(void)
{
    char line[DAQ_CSV_LINE_MAX];
    if (!daq_csv_timestamp_prefix(line, sizeof(line))) {
        daq.failed_records++;
        daq.last_error = ESP_ERR_INVALID_SIZE;
        return true;
    }

    for (size_t i = 0; i < daq.stream_count; i++) {
        solar_os_stream_csv_record_t record;
        const solar_os_stream_read_options_t options = {
            .window_ms = daq.infos[i].id[0] == 'm' ? 100U : 0U,
            .timeout_ms = 0,
        };
        const esp_err_t err = solar_os_stream_read_csv(&daq.streams[i], &options, &record);
        if (err != ESP_OK) {
            daq.failed_records++;
            daq.last_error = err;
            SOLAR_OS_LOGW(TAG, "sample failed: %s", esp_err_to_name(err));
            return true;
        }

        const char *values = daq_csv_value_fields(record.line);
        if (values == NULL || !daq_append_comma_text(line, sizeof(line), values)) {
            daq.failed_records++;
            daq.last_error = ESP_ERR_INVALID_SIZE;
            return true;
        }
    }

    if (fprintf(daq.file, "%s\n", line) < 0 ||
        daq_flush_to_disk(daq.file) != ESP_OK) {
        daq.failed_records++;
        daq.last_error = ESP_FAIL;
        SOLAR_OS_LOGW(TAG, "write failed");
        return true;
    }

    daq.written_records++;
    daq.last_error = ESP_OK;
    return true;
}

static bool daq_event_csv(void)
{
    return daq.stream_count > 1 ? daq_event_csv_multi() : daq_event_csv_single();
}

static void daq_worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t notification = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY);
        if ((notification & DAQ_NOTIFY_STOP) != 0) {
            break;
        }
        if ((notification & DAQ_NOTIFY_SAMPLE) != 0 && daq.running) {
            (void)(daq.raw ? daq_event_raw() : daq_event_csv());
        }
    }

    daq.worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static bool daq_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL || event->type != SOLAR_OS_EVENT_TICK ||
        !daq_should_sample(event->data.tick_ms)) {
        return false;
    }

    if (daq.worker_task == NULL) {
        daq.failed_records++;
        daq.last_error = ESP_ERR_INVALID_STATE;
        return false;
    }
    return xTaskNotify(daq.worker_task, DAQ_NOTIFY_SAMPLE, eSetBits) == pdPASS;
}

void solar_os_daq_job_get_status(solar_os_daq_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->running = daq.running;
    if (daq.stream_count > 0) {
        strlcpy(status->stream_id, daq.infos[0].id, sizeof(status->stream_id));
        status->stream_type = daq.infos[0].type;
    }
    strlcpy(status->stream_ids, daq.stream_list, sizeof(status->stream_ids));
    strlcpy(status->path, daq.path, sizeof(status->path));
    status->stream_count = daq.stream_count;
    status->interval_ms = daq.interval_ms;
    status->written_records = daq.written_records;
    status->written_bytes = daq.written_bytes;
    status->skipped_records = daq.skipped_records;
    status->failed_records = daq.failed_records;
    status->change_only = daq.change_only;
    status->append = daq.append;
    status->raw = daq.raw;
    status->last_error = daq.last_error;
}

const solar_os_job_t solar_os_daq_job = {
    .name = "daq",
    .summary = "capture data streams to CSV or raw files",
    .start = daq_start,
    .stop = daq_stop,
    .event = daq_event,
    .tick_interval_ms = 25U,
    .tick_deadline_ms = 2U,
};
