#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_SCHEDULE_MAX_ENTRIES 16U
#define SOLAR_OS_SCHEDULE_NAME_MAX 24U
#define SOLAR_OS_SCHEDULE_VALUE_MAX 128U

typedef enum {
    SOLAR_OS_SCHEDULE_ONCE_RELATIVE = 0,
    SOLAR_OS_SCHEDULE_ONCE_CALENDAR,
    SOLAR_OS_SCHEDULE_INTERVAL,
    SOLAR_OS_SCHEDULE_DAILY,
    SOLAR_OS_SCHEDULE_WEEKLY,
} solar_os_schedule_kind_t;

typedef enum {
    SOLAR_OS_SCHEDULE_ACTION_ALARM = 0,
    SOLAR_OS_SCHEDULE_ACTION_SCRIPT,
} solar_os_schedule_action_t;

typedef struct {
    char name[SOLAR_OS_SCHEDULE_NAME_MAX];
    solar_os_schedule_kind_t kind;
    solar_os_schedule_action_t action;
    char value[SOLAR_OS_SCHEDULE_VALUE_MAX];
    bool enabled;
    bool persistent;
    uint32_t interval_seconds;
    uint64_t at_utc_seconds;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekdays;
    uint64_t next_utc_seconds;
    uint64_t next_uptime_ms;
    uint32_t run_count;
    uint32_t skipped_count;
} solar_os_schedule_entry_t;

typedef esp_err_t (*solar_os_schedule_script_runner_t)(const char *path);

esp_err_t solar_os_schedule_init(void);
void solar_os_schedule_set_script_runner(solar_os_schedule_script_runner_t runner);
void solar_os_schedule_poll(void);

esp_err_t solar_os_schedule_add_relative(const char *name,
                                         uint32_t seconds,
                                         solar_os_schedule_action_t action,
                                         const char *value,
                                         bool persistent);
esp_err_t solar_os_schedule_add_interval(const char *name,
                                         uint32_t seconds,
                                         solar_os_schedule_action_t action,
                                         const char *value);
esp_err_t solar_os_schedule_add_at(const char *name,
                                   uint16_t year,
                                   uint8_t month,
                                   uint8_t day,
                                   uint8_t hour,
                                   uint8_t minute,
                                   uint8_t second,
                                   solar_os_schedule_action_t action,
                                   const char *value);
esp_err_t solar_os_schedule_add_daily(const char *name,
                                      uint8_t hour,
                                      uint8_t minute,
                                      uint8_t second,
                                      solar_os_schedule_action_t action,
                                      const char *value);
esp_err_t solar_os_schedule_add_weekly(const char *name,
                                       uint8_t weekdays,
                                       uint8_t hour,
                                       uint8_t minute,
                                       uint8_t second,
                                       solar_os_schedule_action_t action,
                                       const char *value);
size_t solar_os_schedule_count(void);
bool solar_os_schedule_get(size_t index, solar_os_schedule_entry_t *entry);
esp_err_t solar_os_schedule_get_by_name(const char *name,
                                        solar_os_schedule_entry_t *entry);
esp_err_t solar_os_schedule_remove(const char *name);
esp_err_t solar_os_schedule_set_enabled(const char *name, bool enabled);
esp_err_t solar_os_schedule_run(const char *name);
esp_err_t solar_os_schedule_remaining_seconds(const char *name,
                                              uint32_t *seconds);

bool solar_os_schedule_alarm_active(char *name, size_t name_len);
void solar_os_schedule_stop_alarm(void);
bool solar_os_schedule_next_wake_us(uint64_t *wake_after_us);

const char *solar_os_schedule_kind_name(solar_os_schedule_kind_t kind);
const char *solar_os_schedule_action_name(solar_os_schedule_action_t action);
