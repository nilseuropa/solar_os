#include "solar_os_schedule.h"

#include <limits.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_config.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_rtc.h"
#include "solar_os_task.h"
#include "solar_os_time.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif

#define SCHEDULE_NVS_NAMESPACE "schedule"
#define SCHEDULE_NVS_KEY "entries"
#define SCHEDULE_STORE_MAGIC 0x53434844U
#define SCHEDULE_STORE_VERSION 1U
#define SCHEDULE_RTC_OWNER "schedule"
#define SCHEDULE_SCRIPT_STACK 6144U
#define SCHEDULE_ALARM_TONE_INTERVAL_MS 1600U

static const char *TAG = "schedule";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    solar_os_schedule_entry_t entries[SOLAR_OS_SCHEDULE_MAX_ENTRIES];
} schedule_store_t;

typedef struct {
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    size_t count;
    solar_os_schedule_script_runner_t script_runner;
    TaskHandle_t script_task;
    char pending_script[SOLAR_OS_SCHEDULE_VALUE_MAX];
    bool alarm_active;
    char alarm_name[SOLAR_OS_SCHEDULE_NAME_MAX];
    uint64_t last_alarm_tone_ms;
    uint64_t armed_utc_seconds;
    uint64_t armed_countdown_uptime_ms;
    uint64_t attempted_countdown_uptime_ms;
    uint64_t countdown_attempt_ms;
} schedule_state_t;

static schedule_state_t state;
static EXT_RAM_BSS_ATTR solar_os_schedule_entry_t
    schedule_entries[SOLAR_OS_SCHEDULE_MAX_ENTRIES];

static void schedule_lock(void)
{
    (void)xSemaphoreTake(state.mutex, portMAX_DELAY);
}

static void schedule_unlock(void)
{
    (void)xSemaphoreGive(state.mutex);
}

static bool name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_SCHEDULE_NAME_MAX) >= SOLAR_OS_SCHEDULE_NAME_MAX) {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' )) {
            return false;
        }
    }
    return true;
}

static bool action_valid(solar_os_schedule_action_t action, const char *value)
{
    if (action == SOLAR_OS_SCHEDULE_ACTION_ALARM) {
        return value == NULL || value[0] == '\0';
    }
    return action == SOLAR_OS_SCHEDULE_ACTION_SCRIPT && value != NULL &&
        value[0] == '/' &&
        strnlen(value, SOLAR_OS_SCHEDULE_VALUE_MAX) < SOLAR_OS_SCHEDULE_VALUE_MAX;
}

static int entry_index_locked(const char *name)
{
    for (size_t i = 0; i < state.count; i++) {
        if (strcmp(schedule_entries[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t save_locked(void)
{
    schedule_store_t *store = solar_os_memory_calloc(
        1, sizeof(*store), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "schedule.store");
    if (store == NULL) {
        return ESP_ERR_NO_MEM;
    }
    store->magic = SCHEDULE_STORE_MAGIC;
    store->version = SCHEDULE_STORE_VERSION;
    for (size_t i = 0; i < state.count; i++) {
        if (!schedule_entries[i].persistent) {
            continue;
        }
        store->entries[store->count++] = schedule_entries[i];
        store->entries[store->count - 1U].next_utc_seconds = 0;
        store->entries[store->count - 1U].next_uptime_ms = 0;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SCHEDULE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        solar_os_memory_free(store);
        return err;
    }
    err = nvs_set_blob(handle, SCHEDULE_NVS_KEY, store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    solar_os_memory_free(store);
    return err;
}

static void load_entries(void)
{
    nvs_handle_t handle = 0;
    if (nvs_open(SCHEDULE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    schedule_store_t *store = solar_os_memory_calloc(
        1, sizeof(*store), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "schedule.load");
    if (store == NULL) {
        nvs_close(handle);
        return;
    }
    size_t size = sizeof(*store);
    const esp_err_t err = nvs_get_blob(handle, SCHEDULE_NVS_KEY, store, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(*store) ||
        store->magic != SCHEDULE_STORE_MAGIC ||
        store->version != SCHEDULE_STORE_VERSION ||
        store->count > SOLAR_OS_SCHEDULE_MAX_ENTRIES) {
        solar_os_memory_free(store);
        return;
    }
    memcpy(schedule_entries, store->entries,
           store->count * sizeof(schedule_entries[0]));
    state.count = store->count;
    solar_os_memory_free(store);
}

/* Gregorian UTC conversion, valid for the SolarOS datetime range. */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned shifted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned doy = (153U * shifted_month + 2U) /
        5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool utc_seconds_from_datetime(const solar_os_datetime_t *datetime,
                                      uint64_t *seconds)
{
    if (datetime == NULL || seconds == NULL ||
        !solar_os_time_datetime_is_valid(datetime)) {
        return false;
    }
    const int64_t days = days_from_civil(datetime->year,
                                         datetime->month,
                                         datetime->day);
    if (days < 0) {
        return false;
    }
    *seconds = (uint64_t)days * 86400ULL +
        (uint64_t)datetime->hour * 3600ULL +
        (uint64_t)datetime->minute * 60ULL + datetime->second;
    return true;
}

static bool local_seconds(uint16_t year,
                          uint8_t month,
                          uint8_t day,
                          uint8_t hour,
                          uint8_t minute,
                          uint8_t second,
                          uint64_t *utc_seconds)
{
    solar_os_datetime_t local = {
        .year = year,
        .month = month,
        .day = day,
        .hour = hour,
        .minute = minute,
        .second = second,
        .clock_integrity = true,
    };
    solar_os_datetime_t utc;
    return solar_os_time_datetime_is_valid(&local) &&
        solar_os_time_local_to_utc(&local, &utc) == ESP_OK &&
        utc_seconds_from_datetime(&utc, utc_seconds);
}

static bool leap_year(uint16_t year)
{
    return (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
}

static uint8_t month_days(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2U && leap_year(year) ? 29U : days[month - 1U];
}

static void next_local_day(solar_os_datetime_t *datetime)
{
    datetime->day++;
    datetime->weekday = (uint8_t)((datetime->weekday + 1U) % 7U);
    if (datetime->day > month_days(datetime->year, datetime->month)) {
        datetime->day = 1;
        datetime->month++;
        if (datetime->month > 12U) {
            datetime->month = 1;
            datetime->year++;
        }
    }
}

static bool compute_calendar_next_locked(solar_os_schedule_entry_t *entry,
                                         uint64_t now_utc)
{
    if (entry->kind == SOLAR_OS_SCHEDULE_ONCE_CALENDAR) {
        entry->next_utc_seconds = entry->at_utc_seconds;
        return entry->next_utc_seconds > now_utc;
    }

    solar_os_datetime_t local;
    if (solar_os_time_get_datetime(&local) != ESP_OK || !local.clock_integrity) {
        entry->next_utc_seconds = 0;
        return false;
    }
    for (unsigned offset = 0; offset <= 7U; offset++) {
        const bool weekday_matches = entry->kind == SOLAR_OS_SCHEDULE_DAILY ||
            (entry->weekdays & (1U << local.weekday)) != 0;
        uint64_t candidate = 0;
        if (weekday_matches &&
            local_seconds(local.year, local.month, local.day,
                          entry->hour, entry->minute, entry->second,
                          &candidate) && candidate > now_utc) {
            entry->next_utc_seconds = candidate;
            return true;
        }
        next_local_day(&local);
    }
    entry->next_utc_seconds = 0;
    return false;
}

static esp_err_t add_entry(const solar_os_schedule_entry_t *entry)
{
    if (state.mutex == NULL || entry == NULL || !name_valid(entry->name) ||
        !action_valid(entry->action, entry->value)) {
        return ESP_ERR_INVALID_ARG;
    }
    schedule_lock();
    if (entry_index_locked(entry->name) >= 0) {
        schedule_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (state.count >= SOLAR_OS_SCHEDULE_MAX_ENTRIES) {
        schedule_unlock();
        return ESP_ERR_NO_MEM;
    }
    schedule_entries[state.count++] = *entry;
    const esp_err_t err = entry->persistent ? save_locked() : ESP_OK;
    schedule_unlock();
    return err;
}

esp_err_t solar_os_schedule_init(void)
{
    if (state.mutex != NULL) {
        return ESP_OK;
    }
    memset(&state, 0, sizeof(state));
    state.mutex = xSemaphoreCreateMutexStatic(&state.mutex_storage);
    if (state.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    load_entries();
    const uint64_t now_ms = solar_os_time_uptime_ms();
    for (size_t i = 0; i < state.count; i++) {
        if (schedule_entries[i].kind == SOLAR_OS_SCHEDULE_INTERVAL) {
            schedule_entries[i].next_uptime_ms = now_ms +
                (uint64_t)schedule_entries[i].interval_seconds * 1000ULL;
        }
    }
    return ESP_OK;
}

void solar_os_schedule_set_script_runner(solar_os_schedule_script_runner_t runner)
{
    if (state.mutex == NULL) {
        return;
    }
    schedule_lock();
    state.script_runner = runner;
    schedule_unlock();
}

static void script_task(void *arg)
{
    (void)arg;
    char path[SOLAR_OS_SCHEDULE_VALUE_MAX];
    solar_os_schedule_script_runner_t runner;
    schedule_lock();
    strlcpy(path, state.pending_script, sizeof(path));
    runner = state.script_runner;
    schedule_unlock();
    const esp_err_t err = runner != NULL ? runner(path) : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "script %s failed: %s", path, esp_err_to_name(err));
    }
    schedule_lock();
    state.pending_script[0] = '\0';
    state.script_task = NULL;
    schedule_unlock();
    solar_os_task_delete_internal(NULL);
}

static bool trigger_locked(solar_os_schedule_entry_t *entry)
{
    entry->run_count++;
    if (entry->action == SOLAR_OS_SCHEDULE_ACTION_ALARM) {
        state.alarm_active = true;
        strlcpy(state.alarm_name, entry->name, sizeof(state.alarm_name));
        state.last_alarm_tone_ms = 0;
        SOLAR_OS_LOGI(TAG, "alarm due: %s", entry->name);
        return true;
    }
    if (state.script_task != NULL || state.pending_script[0] != '\0' ||
        state.script_runner == NULL) {
        entry->skipped_count++;
        return false;
    }
    strlcpy(state.pending_script, entry->value, sizeof(state.pending_script));
    if (solar_os_task_create_pinned_internal(script_task,
                                             "schedule_script",
                                             SCHEDULE_SCRIPT_STACK,
                                             NULL,
                                             3,
                                             &state.script_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        state.pending_script[0] = '\0';
        state.script_task = NULL;
        entry->skipped_count++;
        return false;
    }
    return true;
}

static void service_alarm_sound(uint64_t now_ms)
{
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    schedule_lock();
    const bool due = state.alarm_active &&
        (state.last_alarm_tone_ms == 0 ||
         now_ms - state.last_alarm_tone_ms >= SCHEDULE_ALARM_TONE_INTERVAL_MS);
    if (due) {
        state.last_alarm_tone_ms = now_ms;
    }
    schedule_unlock();
    if (due) {
        static const solar_os_audio_tone_step_t steps[] = {
            {.frequency_hz = 1200, .duration_ms = 70, .pause_ms = 45},
            {.frequency_hz = 1200, .duration_ms = 70, .pause_ms = 45},
            {.frequency_hz = 1200, .duration_ms = 70, .pause_ms = 45},
            {.frequency_hz = 1200, .duration_ms = 70, .pause_ms = 0},
        };
        const solar_os_audio_tone_request_t request = {
            .steps = steps,
            .step_count = sizeof(steps) / sizeof(steps[0]),
            .volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL,
            .drop_if_busy = true,
        };
        (void)solar_os_audio_tone_enqueue(&request, NULL);
    }
#else
    (void)now_ms;
#endif
}

static bool rtc_alarm_can_represent(uint64_t now_utc, uint64_t target_utc)
{
    time_t target_timestamp = (time_t)target_utc;
    struct tm target_tm;
    if (gmtime_r(&target_timestamp, &target_tm) == NULL) {
        return false;
    }

    const uint64_t day_start = now_utc - now_utc % 86400ULL;
    const uint64_t time_of_day = (uint64_t)target_tm.tm_hour * 3600ULL +
        (uint64_t)target_tm.tm_min * 60ULL + (uint64_t)target_tm.tm_sec;
    /* A day-of-month alarm can skip a short month, so search two months. */
    for (unsigned offset = 0; offset <= 62U; offset++) {
        const uint64_t candidate = day_start +
            (uint64_t)offset * 86400ULL + time_of_day;
        if (candidate <= now_utc) {
            continue;
        }
        time_t candidate_timestamp = (time_t)candidate;
        struct tm candidate_tm;
        if (gmtime_r(&candidate_timestamp, &candidate_tm) == NULL) {
            return false;
        }
        if (candidate_tm.tm_mday == target_tm.tm_mday) {
            return candidate == target_utc;
        }
    }
    return false;
}

static void update_rtc_alarm(uint64_t next_utc, uint64_t now_utc)
{
    solar_os_rtc_info_t info;
    if (solar_os_rtc_get_info(&info) != ESP_OK ||
        (info.capabilities & SOLAR_OS_RTC_CAP_ALARM) == 0) {
        state.armed_utc_seconds = 0;
        return;
    }
    if (next_utc == 0) {
        if (strcmp(info.alarm_owner, SCHEDULE_RTC_OWNER) == 0) {
            (void)solar_os_rtc_disable_alarm_for(SCHEDULE_RTC_OWNER);
        }
        state.armed_utc_seconds = 0;
        return;
    }
    if (!rtc_alarm_can_represent(now_utc, next_utc)) {
        if (strcmp(info.alarm_owner, SCHEDULE_RTC_OWNER) == 0) {
            (void)solar_os_rtc_disable_alarm_for(SCHEDULE_RTC_OWNER);
        }
        state.armed_utc_seconds = 0;
        return;
    }
    if (state.armed_utc_seconds == next_utc &&
        strcmp(info.alarm_owner, SCHEDULE_RTC_OWNER) == 0) {
        return;
    }

    time_t timestamp = (time_t)next_utc;
    struct tm utc_tm;
    if (gmtime_r(&timestamp, &utc_tm) == NULL) {
        return;
    }
    const solar_os_rtc_alarm_t alarm = {
        .match_fields = SOLAR_OS_RTC_ALARM_MATCH_SECOND |
            SOLAR_OS_RTC_ALARM_MATCH_MINUTE |
            SOLAR_OS_RTC_ALARM_MATCH_HOUR |
            SOLAR_OS_RTC_ALARM_MATCH_DAY,
        .second = (uint8_t)utc_tm.tm_sec,
        .minute = (uint8_t)utc_tm.tm_min,
        .hour = (uint8_t)utc_tm.tm_hour,
        .day = (uint8_t)utc_tm.tm_mday,
    };
    if (solar_os_rtc_set_alarm_for(SCHEDULE_RTC_OWNER, &alarm) == ESP_OK) {
        state.armed_utc_seconds = next_utc;
    } else {
        state.armed_utc_seconds = 0;
    }
}

static void update_rtc_countdown(uint64_t next_uptime_ms, uint64_t now_ms)
{
    solar_os_rtc_info_t info;
    if (solar_os_rtc_get_info(&info) != ESP_OK ||
        (info.capabilities & SOLAR_OS_RTC_CAP_COUNTDOWN) == 0) {
        state.armed_countdown_uptime_ms = 0;
        state.attempted_countdown_uptime_ms = 0;
        return;
    }
    if (next_uptime_ms == 0) {
        if (strcmp(info.countdown_owner, SCHEDULE_RTC_OWNER) == 0) {
            (void)solar_os_rtc_disable_countdown_for(SCHEDULE_RTC_OWNER);
        }
        state.armed_countdown_uptime_ms = 0;
        state.attempted_countdown_uptime_ms = 0;
        return;
    }
    if (state.armed_countdown_uptime_ms == next_uptime_ms &&
        strcmp(info.countdown_owner, SCHEDULE_RTC_OWNER) == 0) {
        return;
    }
    if (state.attempted_countdown_uptime_ms == next_uptime_ms &&
        now_ms - state.countdown_attempt_ms < 1000U) {
        return;
    }
    state.attempted_countdown_uptime_ms = next_uptime_ms;
    state.countdown_attempt_ms = now_ms;

    const uint64_t remaining = next_uptime_ms > now_ms ?
        (next_uptime_ms - now_ms + 999ULL) / 1000ULL : 1ULL;
    if (remaining > UINT32_MAX) {
        state.armed_countdown_uptime_ms = 0;
        return;
    }
    if (solar_os_rtc_set_countdown_for(SCHEDULE_RTC_OWNER,
                                       (uint32_t)remaining,
                                       false) == ESP_OK) {
        state.armed_countdown_uptime_ms = next_uptime_ms;
    } else {
        state.armed_countdown_uptime_ms = 0;
    }
}

void solar_os_schedule_poll(void)
{
    if (state.mutex == NULL) {
        return;
    }
    const uint64_t now_ms = solar_os_time_uptime_ms();
    uint64_t epoch_ms = 0;
    const bool time_valid = solar_os_time_get_utc_epoch_ms(&epoch_ms) == ESP_OK;
    const uint64_t now_utc = epoch_ms / 1000ULL;
    uint64_t next_utc = 0;
    uint64_t next_uptime_ms = 0;
    bool persistent_changed = false;

    schedule_lock();
    for (size_t i = 0; i < state.count; i++) {
        solar_os_schedule_entry_t *entry = &schedule_entries[i];
        if (!entry->enabled) {
            continue;
        }
        bool due = false;
        if (entry->kind == SOLAR_OS_SCHEDULE_ONCE_RELATIVE ||
            entry->kind == SOLAR_OS_SCHEDULE_INTERVAL) {
            if (entry->next_uptime_ms == 0) {
                entry->next_uptime_ms = now_ms +
                    (uint64_t)entry->interval_seconds * 1000ULL;
            }
            due = now_ms >= entry->next_uptime_ms;
            if (due && entry->kind == SOLAR_OS_SCHEDULE_INTERVAL) {
                do {
                    entry->next_uptime_ms +=
                        (uint64_t)entry->interval_seconds * 1000ULL;
                } while (entry->next_uptime_ms <= now_ms);
            }
            if (time_valid && entry->next_uptime_ms > now_ms) {
                const uint64_t candidate = now_utc +
                    (entry->next_uptime_ms - now_ms + 999ULL) / 1000ULL;
                if (next_utc == 0 || candidate < next_utc) {
                    next_utc = candidate;
                }
            }
            if (entry->next_uptime_ms > now_ms &&
                (next_uptime_ms == 0 || entry->next_uptime_ms < next_uptime_ms)) {
                next_uptime_ms = entry->next_uptime_ms;
            }
        } else if (time_valid) {
            if (entry->next_utc_seconds == 0) {
                (void)compute_calendar_next_locked(entry, now_utc);
            }
            due = entry->next_utc_seconds != 0 && now_utc >= entry->next_utc_seconds;
            if (!due && entry->next_utc_seconds != 0 &&
                (next_utc == 0 || entry->next_utc_seconds < next_utc)) {
                next_utc = entry->next_utc_seconds;
            }
        }

        if (!due) {
            continue;
        }
        (void)trigger_locked(entry);
        if (entry->kind == SOLAR_OS_SCHEDULE_ONCE_RELATIVE ||
            entry->kind == SOLAR_OS_SCHEDULE_ONCE_CALENDAR) {
            entry->enabled = false;
            persistent_changed |= entry->persistent;
        } else if (entry->kind == SOLAR_OS_SCHEDULE_DAILY ||
                   entry->kind == SOLAR_OS_SCHEDULE_WEEKLY) {
            entry->next_utc_seconds = 0;
            if (time_valid && compute_calendar_next_locked(entry, now_utc) &&
                (next_utc == 0 || entry->next_utc_seconds < next_utc)) {
                next_utc = entry->next_utc_seconds;
            }
        }
    }
    if (persistent_changed) {
        (void)save_locked();
    }
    schedule_unlock();

    service_alarm_sound(now_ms);
    update_rtc_alarm(next_utc, now_utc);
    update_rtc_countdown(next_uptime_ms, now_ms);
    solar_os_rtc_info_t rtc_info;
    if (solar_os_rtc_get_info(&rtc_info) == ESP_OK) {
        uint32_t owned_interrupts = 0;
        if (strcmp(rtc_info.alarm_owner, SCHEDULE_RTC_OWNER) == 0) {
            owned_interrupts |= SOLAR_OS_RTC_INTERRUPT_ALARM;
        }
        if (strcmp(rtc_info.countdown_owner, SCHEDULE_RTC_OWNER) == 0) {
            owned_interrupts |= SOLAR_OS_RTC_INTERRUPT_COUNTDOWN;
        }
        uint32_t pending = 0;
        if (solar_os_rtc_get_interrupt_status(&pending) == ESP_OK &&
            (pending & owned_interrupts) != 0) {
            (void)solar_os_rtc_clear_interrupt_status(pending & owned_interrupts);
        }
    }
}

static void fill_action(solar_os_schedule_entry_t *entry,
                        solar_os_schedule_action_t action,
                        const char *value)
{
    entry->action = action;
    if (value != NULL) {
        strlcpy(entry->value, value, sizeof(entry->value));
    }
}

esp_err_t solar_os_schedule_add_relative(const char *name,
                                         uint32_t seconds,
                                         solar_os_schedule_action_t action,
                                         const char *value,
                                         bool persistent)
{
    if (seconds == 0 || !name_valid(name) || !action_valid(action, value)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_schedule_entry_t entry = {
        .kind = SOLAR_OS_SCHEDULE_ONCE_RELATIVE,
        .enabled = true,
        .persistent = persistent,
        .interval_seconds = seconds,
        .next_uptime_ms = solar_os_time_uptime_ms() + (uint64_t)seconds * 1000ULL,
    };
    strlcpy(entry.name, name, sizeof(entry.name));
    fill_action(&entry, action, value);
    return add_entry(&entry);
}

esp_err_t solar_os_schedule_add_interval(const char *name,
                                         uint32_t seconds,
                                         solar_os_schedule_action_t action,
                                         const char *value)
{
    if (seconds == 0 || !name_valid(name) || !action_valid(action, value)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_schedule_entry_t entry = {
        .kind = SOLAR_OS_SCHEDULE_INTERVAL,
        .enabled = true,
        .persistent = true,
        .interval_seconds = seconds,
        .next_uptime_ms = solar_os_time_uptime_ms() + (uint64_t)seconds * 1000ULL,
    };
    strlcpy(entry.name, name, sizeof(entry.name));
    fill_action(&entry, action, value);
    return add_entry(&entry);
}

esp_err_t solar_os_schedule_add_at(const char *name,
                                   uint16_t year,
                                   uint8_t month,
                                   uint8_t day,
                                   uint8_t hour,
                                   uint8_t minute,
                                   uint8_t second,
                                   solar_os_schedule_action_t action,
                                   const char *value)
{
    uint64_t at_utc = 0;
    uint64_t now_ms = 0;
    if (!name_valid(name) || !action_valid(action, value) ||
        !local_seconds(year, month, day, hour, minute, second, &at_utc) ||
        solar_os_time_get_utc_epoch_ms(&now_ms) != ESP_OK ||
        at_utc <= now_ms / 1000ULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_schedule_entry_t entry = {
        .kind = SOLAR_OS_SCHEDULE_ONCE_CALENDAR,
        .enabled = true,
        .persistent = true,
        .at_utc_seconds = at_utc,
        .next_utc_seconds = at_utc,
    };
    strlcpy(entry.name, name, sizeof(entry.name));
    fill_action(&entry, action, value);
    return add_entry(&entry);
}

static esp_err_t add_recurring(const char *name,
                               solar_os_schedule_kind_t kind,
                               uint8_t weekdays,
                               uint8_t hour,
                               uint8_t minute,
                               uint8_t second,
                               solar_os_schedule_action_t action,
                               const char *value)
{
    if (!name_valid(name) || !action_valid(action, value) ||
        hour > 23 || minute > 59 || second > 59 ||
        (kind == SOLAR_OS_SCHEDULE_WEEKLY &&
         (weekdays == 0 || (weekdays & 0x80U) != 0))) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_schedule_entry_t entry = {
        .kind = kind,
        .enabled = true,
        .persistent = true,
        .hour = hour,
        .minute = minute,
        .second = second,
        .weekdays = weekdays,
    };
    strlcpy(entry.name, name, sizeof(entry.name));
    fill_action(&entry, action, value);
    return add_entry(&entry);
}

esp_err_t solar_os_schedule_add_daily(const char *name,
                                      uint8_t hour,
                                      uint8_t minute,
                                      uint8_t second,
                                      solar_os_schedule_action_t action,
                                      const char *value)
{
    return add_recurring(name, SOLAR_OS_SCHEDULE_DAILY, 0,
                         hour, minute, second, action, value);
}

esp_err_t solar_os_schedule_add_weekly(const char *name,
                                       uint8_t weekdays,
                                       uint8_t hour,
                                       uint8_t minute,
                                       uint8_t second,
                                       solar_os_schedule_action_t action,
                                       const char *value)
{
    return add_recurring(name, SOLAR_OS_SCHEDULE_WEEKLY, weekdays,
                         hour, minute, second, action, value);
}

size_t solar_os_schedule_count(void)
{
    if (state.mutex == NULL) {
        return 0;
    }
    schedule_lock();
    const size_t count = state.count;
    schedule_unlock();
    return count;
}

bool solar_os_schedule_get(size_t index, solar_os_schedule_entry_t *entry)
{
    if (state.mutex == NULL || entry == NULL) {
        return false;
    }
    schedule_lock();
    const bool found = index < state.count;
    if (found) {
        *entry = schedule_entries[index];
    }
    schedule_unlock();
    return found;
}

esp_err_t solar_os_schedule_get_by_name(const char *name,
                                        solar_os_schedule_entry_t *entry)
{
    if (state.mutex == NULL || name == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    schedule_lock();
    const int index = entry_index_locked(name);
    if (index >= 0) {
        *entry = schedule_entries[index];
    }
    schedule_unlock();
    return index >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_schedule_remove(const char *name)
{
    if (state.mutex == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    schedule_lock();
    const int index = entry_index_locked(name);
    if (index < 0) {
        schedule_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const bool persistent = schedule_entries[index].persistent;
    for (size_t i = (size_t)index + 1U; i < state.count; i++) {
        schedule_entries[i - 1U] = schedule_entries[i];
    }
    state.count--;
    const esp_err_t err = persistent ? save_locked() : ESP_OK;
    schedule_unlock();
    return err;
}

esp_err_t solar_os_schedule_set_enabled(const char *name, bool enabled)
{
    if (state.mutex == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    schedule_lock();
    const int index = entry_index_locked(name);
    if (index < 0) {
        schedule_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    schedule_entries[index].enabled = enabled;
    schedule_entries[index].next_utc_seconds = 0;
    if (enabled && (schedule_entries[index].kind == SOLAR_OS_SCHEDULE_INTERVAL ||
                    schedule_entries[index].kind == SOLAR_OS_SCHEDULE_ONCE_RELATIVE)) {
        schedule_entries[index].next_uptime_ms = solar_os_time_uptime_ms() +
            (uint64_t)schedule_entries[index].interval_seconds * 1000ULL;
    }
    const esp_err_t err = schedule_entries[index].persistent ? save_locked() : ESP_OK;
    schedule_unlock();
    return err;
}

esp_err_t solar_os_schedule_run(const char *name)
{
    if (state.mutex == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    schedule_lock();
    const int index = entry_index_locked(name);
    const esp_err_t err = index >= 0 && trigger_locked(&schedule_entries[index]) ?
        ESP_OK : (index >= 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND);
    schedule_unlock();
    return err;
}

esp_err_t solar_os_schedule_remaining_seconds(const char *name,
                                              uint32_t *seconds)
{
    if (state.mutex == NULL || name == NULL || seconds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    schedule_lock();
    const int index = entry_index_locked(name);
    if (index < 0 || !schedule_entries[index].enabled) {
        schedule_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const solar_os_schedule_entry_t *entry = &schedule_entries[index];
    uint64_t remaining = 0;
    if (entry->kind == SOLAR_OS_SCHEDULE_ONCE_RELATIVE ||
        entry->kind == SOLAR_OS_SCHEDULE_INTERVAL) {
        const uint64_t now = solar_os_time_uptime_ms();
        remaining = entry->next_uptime_ms > now ?
            (entry->next_uptime_ms - now + 999ULL) / 1000ULL : 0;
    } else {
        uint64_t epoch_ms = 0;
        if (solar_os_time_get_utc_epoch_ms(&epoch_ms) != ESP_OK ||
            entry->next_utc_seconds == 0) {
            schedule_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        remaining = entry->next_utc_seconds > epoch_ms / 1000ULL ?
            entry->next_utc_seconds - epoch_ms / 1000ULL : 0;
    }
    *seconds = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    schedule_unlock();
    return ESP_OK;
}

bool solar_os_schedule_alarm_active(char *name, size_t name_len)
{
    if (state.mutex == NULL) {
        return false;
    }
    schedule_lock();
    const bool active = state.alarm_active;
    if (name != NULL && name_len > 0) {
        strlcpy(name, active ? state.alarm_name : "", name_len);
    }
    schedule_unlock();
    return active;
}

void solar_os_schedule_stop_alarm(void)
{
    if (state.mutex == NULL) {
        return;
    }
    schedule_lock();
    state.alarm_active = false;
    state.alarm_name[0] = '\0';
    schedule_unlock();
}

bool solar_os_schedule_next_wake_us(uint64_t *wake_after_us)
{
    if (state.mutex == NULL || wake_after_us == NULL) {
        return false;
    }
    const uint64_t now_ms = solar_os_time_uptime_ms();
    uint64_t epoch_ms = 0;
    const bool time_valid = solar_os_time_get_utc_epoch_ms(&epoch_ms) == ESP_OK;
    uint64_t best_ms = UINT64_MAX;
    schedule_lock();
    for (size_t i = 0; i < state.count; i++) {
        const solar_os_schedule_entry_t *entry = &schedule_entries[i];
        if (!entry->enabled) {
            continue;
        }
        uint64_t candidate = UINT64_MAX;
        if (entry->kind == SOLAR_OS_SCHEDULE_ONCE_RELATIVE ||
            entry->kind == SOLAR_OS_SCHEDULE_INTERVAL) {
            candidate = entry->next_uptime_ms > now_ms ?
                entry->next_uptime_ms - now_ms : 1;
        } else if (time_valid && entry->next_utc_seconds != 0) {
            const uint64_t now_utc_ms = epoch_ms;
            const uint64_t due_ms = entry->next_utc_seconds * 1000ULL;
            candidate = due_ms > now_utc_ms ? due_ms - now_utc_ms : 1;
        }
        if (candidate < best_ms) {
            best_ms = candidate;
        }
    }
    schedule_unlock();
    if (best_ms == UINT64_MAX) {
        return false;
    }
    *wake_after_us = best_ms * 1000ULL;
    return true;
}

const char *solar_os_schedule_kind_name(solar_os_schedule_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_SCHEDULE_ONCE_RELATIVE: return "in";
    case SOLAR_OS_SCHEDULE_ONCE_CALENDAR: return "at";
    case SOLAR_OS_SCHEDULE_INTERVAL: return "every";
    case SOLAR_OS_SCHEDULE_DAILY: return "daily";
    case SOLAR_OS_SCHEDULE_WEEKLY: return "weekly";
    default: return "unknown";
    }
}

const char *solar_os_schedule_action_name(solar_os_schedule_action_t action)
{
    return action == SOLAR_OS_SCHEDULE_ACTION_ALARM ? "alarm" :
        action == SOLAR_OS_SCHEDULE_ACTION_SCRIPT ? "run" : "unknown";
}
