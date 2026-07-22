#include "solar_os_ntp_sync_job.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_log.h"
#include "solar_os_jobs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_time.h"
#include "solar_os_wifi.h"

#define NTP_SYNC_DEFAULT_INTERVAL_SEC 60U
#define NTP_SYNC_MIN_INTERVAL_SEC 10U
#define NTP_SYNC_MAX_INTERVAL_SEC 86400U
#define NTP_SYNC_SERVER_MAX 96
#define NTP_SYNC_TASK_STACK 4096

static const char *TAG = "solar_os_ntp_job";

typedef struct {
    bool running;
    volatile bool sync_in_progress;
    volatile bool complete_requested;
    bool once;
    uint32_t interval_ms;
    uint32_t next_sync_ms;
    char server[NTP_SYNC_SERVER_MAX];
    TaskHandle_t task;
    uint32_t success_count;
    uint32_t fail_count;
    uint32_t generation;
    esp_err_t last_error;
} ntp_sync_job_state_t;

static ntp_sync_job_state_t ntp_job = {
    .interval_ms = NTP_SYNC_DEFAULT_INTERVAL_SEC * 1000U,
    .server = SOLAR_OS_NTP_DEFAULT_SERVER,
    .last_error = ESP_OK,
};

static bool parse_interval_arg(const char *text, uint32_t *seconds)
{
    if (text == NULL || text[0] == '\0' || seconds == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        parsed < NTP_SYNC_MIN_INTERVAL_SEC ||
        parsed > NTP_SYNC_MAX_INTERVAL_SEC) {
        return false;
    }

    *seconds = (uint32_t)parsed;
    return true;
}

static bool parse_start_args(int argc,
                             char **argv,
                             uint32_t *interval_sec,
                             const char **server,
                             bool *once)
{
    if (interval_sec == NULL || server == NULL || once == NULL || argc < 0) {
        return false;
    }

    *interval_sec = NTP_SYNC_DEFAULT_INTERVAL_SEC;
    *server = SOLAR_OS_NTP_DEFAULT_SERVER;
    *once = false;

    int first_arg = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_ntp_sync_job.name) == 0) {
        first_arg = 1;
    }
    if (argc > first_arg + 3) {
        return false;
    }

    const char *positional[2] = {0};
    size_t positional_count = 0;
    for (int i = first_arg; i < argc; i++) {
        if (argv == NULL || argv[i] == NULL || argv[i][0] == '\0') {
            return false;
        }

        if (strcmp(argv[i], "once") == 0) {
            if (*once) {
                return false;
            }
            *once = true;
            continue;
        }

        if (positional_count >= (sizeof(positional) / sizeof(positional[0]))) {
            return false;
        }
        positional[positional_count++] = argv[i];
    }

    if (positional_count >= 1 &&
        !parse_interval_arg(positional[0], interval_sec)) {
        return false;
    }
    if (positional_count >= 2) {
        *server = positional[1];
    }

    return true;
}

static void log_datetime(const char *label, const solar_os_datetime_t *datetime)
{
    if (datetime == NULL) {
        return;
    }

    SOLAR_OS_LOGI(TAG,
             "%s %04u-%02u-%02u %02u:%02u:%02u",
             label,
             (unsigned)datetime->year,
             (unsigned)datetime->month,
             (unsigned)datetime->day,
             (unsigned)datetime->hour,
             (unsigned)datetime->minute,
             (unsigned)datetime->second);
}

static bool rtc_time_is_usable(solar_os_datetime_t *utc)
{
    solar_os_datetime_t current_utc;
    if (solar_os_time_get_utc_datetime(&current_utc) != ESP_OK ||
        !solar_os_time_datetime_is_valid(&current_utc) ||
        !current_utc.clock_integrity) {
        return false;
    }

    if (utc != NULL) {
        *utc = current_utc;
    }
    return true;
}

static void ntp_sync_task(void *arg)
{
    (void)arg;

    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);
    if (!wifi_status.has_ip) {
        ntp_job.last_error = ESP_ERR_INVALID_STATE;
        ntp_job.fail_count++;
        SOLAR_OS_LOGW(TAG, "sync skipped: Wi-Fi is not connected");
        ntp_job.sync_in_progress = false;
        ntp_job.task = NULL;
        vTaskDelete(NULL);
        return;
    }

    solar_os_datetime_t utc;
    solar_os_datetime_t local;
    SOLAR_OS_LOGI(TAG, "syncing with %s", ntp_job.server);
    const esp_err_t err = solar_os_time_ntp_sync(ntp_job.server,
                                                 SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS,
                                                 &utc,
                                                 NULL);
    ntp_job.last_error = err;
    if (err == ESP_OK) {
        ntp_job.success_count++;
        log_datetime("UTC", &utc);
        if (solar_os_time_utc_to_local(&utc, &local) == ESP_OK) {
            log_datetime("local", &local);
        }
        if (ntp_job.once) {
            ntp_job.running = false;
            ntp_job.complete_requested = true;
            SOLAR_OS_LOGI(TAG, "one-shot sync complete");
        }
    } else {
        solar_os_datetime_t rtc_utc;
        if (ntp_job.once && err == ESP_ERR_TIMEOUT && rtc_time_is_usable(&rtc_utc)) {
            ntp_job.last_error = ESP_OK;
            ntp_job.success_count++;
            ntp_job.running = false;
            ntp_job.complete_requested = true;
            SOLAR_OS_LOGW(TAG,
                          "sync timed out; RTC already valid, stopping one-shot");
            log_datetime("RTC", &rtc_utc);
        } else {
            ntp_job.fail_count++;
            SOLAR_OS_LOGW(TAG, "sync failed: %s", esp_err_to_name(err));
        }
    }

    ntp_job.sync_in_progress = false;
    ntp_job.task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t ntp_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    if (ntp_job.task != NULL || ntp_job.sync_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t interval_sec = NTP_SYNC_DEFAULT_INTERVAL_SEC;
    const char *server = SOLAR_OS_NTP_DEFAULT_SERVER;
    bool once = false;
    if (!parse_start_args(argc, argv, &interval_sec, &server, &once)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (server == NULL || server[0] == '\0' || strlen(server) >= sizeof(ntp_job.server)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_jobs_get_generation(solar_os_ntp_sync_job.name,
                                                 &ntp_job.generation);
    if (err != ESP_OK) {
        return err;
    }

    ntp_job.running = true;
    ntp_job.once = once;
    ntp_job.complete_requested = false;
    ntp_job.interval_ms = interval_sec * 1000U;
    ntp_job.next_sync_ms = 0;
    strlcpy(ntp_job.server, server, sizeof(ntp_job.server));
    ntp_job.success_count = 0;
    ntp_job.fail_count = 0;
    ntp_job.last_error = ESP_OK;

    SOLAR_OS_LOGI(TAG,
             "started: mode=%s interval=%us server=%s",
             ntp_job.once ? "once" : "periodic",
             (unsigned)interval_sec,
             ntp_job.server);
    (void)solar_os_jobs_note_resource(solar_os_ntp_sync_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      ntp_job.server,
                                      "ntp");
    return ESP_OK;
}

static void ntp_sync_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    ntp_job.running = false;
    ntp_job.once = false;
    ntp_job.complete_requested = false;
    SOLAR_OS_LOGI(TAG,
             "stopped: ok=%u fail=%u",
             (unsigned)ntp_job.success_count,
             (unsigned)ntp_job.fail_count);
}

static bool ntp_sync_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }

    if (ntp_job.complete_requested && !ntp_job.sync_in_progress) {
        ntp_job.complete_requested = false;
        ntp_job.once = false;
        (void)solar_os_jobs_mark_stopped(solar_os_ntp_sync_job.name,
                                         ntp_job.generation,
                                         ESP_OK);
        return true;
    }

    if (!ntp_job.running || ntp_job.sync_in_progress) {
        return false;
    }

    const uint32_t now_ms = event->data.tick_ms;
    if (ntp_job.next_sync_ms != 0 &&
        (int32_t)(now_ms - ntp_job.next_sync_ms) < 0) {
        return false;
    }

    ntp_job.next_sync_ms = now_ms + ntp_job.interval_ms;
    ntp_job.sync_in_progress = true;
    if (xTaskCreate(ntp_sync_task,
                    "ntp_sync_job",
                    NTP_SYNC_TASK_STACK,
                    NULL,
                    tskIDLE_PRIORITY + 2,
                    &ntp_job.task) != pdPASS) {
        ntp_job.sync_in_progress = false;
        ntp_job.task = NULL;
        ntp_job.last_error = ESP_ERR_NO_MEM;
        ntp_job.fail_count++;
        SOLAR_OS_LOGW(TAG, "failed to create sync task");
    }

    return true;
}

const solar_os_job_t solar_os_ntp_sync_job = {
    .name = "ntp-sync",
    .summary = "RTC NTP sync",
    .start = ntp_sync_start,
    .stop = ntp_sync_stop,
    .event = ntp_sync_event,
    .tick_interval_ms = 100U,
    .tick_deadline_ms = 10U,
};
