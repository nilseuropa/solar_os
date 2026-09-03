#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "solar_os_rtc.h"
#include "solar_os_schedule.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_time.h"

#define RTC_CLI_OWNER "rtc.cli"

static bool parse_uint(const char *text, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > max) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_duration(const char *text, uint32_t *seconds)
{
    if (text == NULL || seconds == NULL) {
        return false;
    }
    const size_t len = strlen(text);
    if (len < 2) {
        return false;
    }
    char number[16];
    if (len >= sizeof(number)) {
        return false;
    }
    memcpy(number, text, len - 1U);
    number[len - 1U] = '\0';
    uint32_t value = 0;
    if (!parse_uint(number, UINT32_MAX, &value) || value == 0) {
        return false;
    }
    uint32_t multiplier = 0;
    switch (text[len - 1U]) {
    case 's': multiplier = 1U; break;
    case 'm': multiplier = 60U; break;
    case 'h': multiplier = 3600U; break;
    case 'd': multiplier = 86400U; break;
    default: return false;
    }
    if (value > UINT32_MAX / multiplier) {
        return false;
    }
    *seconds = value * multiplier;
    return true;
}

static bool parse_time(const char *text,
                       uint8_t *hour,
                       uint8_t *minute,
                       uint8_t *second)
{
    unsigned h = 0;
    unsigned m = 0;
    unsigned s = 0;
    char tail = '\0';
    const char *value = text != NULL ? text : "";
    int fields = sscanf(value, "%u:%u:%u%c", &h, &m, &s, &tail);
    if (fields != 3) {
        s = 0;
        fields = sscanf(value, "%u:%u%c", &h, &m, &tail);
        if (fields != 2) {
            return false;
        }
    }
    if (h > 23 || m > 59 || s > 59) {
        return false;
    }
    *hour = (uint8_t)h;
    *minute = (uint8_t)m;
    *second = (uint8_t)s;
    return true;
}

static bool parse_date(const char *text, uint16_t *year, uint8_t *month, uint8_t *day)
{
    unsigned y = 0;
    unsigned m = 0;
    unsigned d = 0;
    char tail = '\0';
    if (sscanf(text != NULL ? text : "", "%u-%u-%u%c", &y, &m, &d, &tail) != 3 ||
        y < 1970 || y > 2199 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    *year = (uint16_t)y;
    *month = (uint8_t)m;
    *day = (uint8_t)d;
    return true;
}

static bool parse_weekdays(const char *text, uint8_t *weekdays)
{
    static const char *const names[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    if (text == NULL || weekdays == NULL || strlen(text) >= 40U) {
        return false;
    }
    char copy[40];
    strlcpy(copy, text, sizeof(copy));
    uint8_t mask = 0;
    char *save = NULL;
    for (char *token = strtok_r(copy, ",", &save);
         token != NULL;
         token = strtok_r(NULL, ",", &save)) {
        bool found = false;
        for (size_t i = 0; i < 7; i++) {
            if (strcmp(token, names[i]) == 0) {
                mask |= (uint8_t)(1U << i);
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    *weekdays = mask;
    return mask != 0;
}

static void print_rtc_error(solar_os_shell_io_t *io, const char *operation, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_rtc_info_t info;
        if (solar_os_rtc_get_info(&info) == ESP_OK) {
            const bool timer_operation = strstr(operation, "timer") != NULL;
            const char *owner = timer_operation ? info.countdown_owner : info.alarm_owner;
            if (owner[0] != '\0') {
                solar_os_shell_io_printf(io, "rtc: %s busy (%s owns %s)\n",
                                         operation, owner,
                                         timer_operation ? "timer" : "alarm");
                return;
            }
        }
    }
    solar_os_shell_io_printf(io, "rtc: %s failed: %s\n",
                             operation, solar_os_shell_error_text(err));
}

void solar_os_shell_cmd_rtc(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_context_io(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        solar_os_rtc_info_t info;
        const esp_err_t err = solar_os_rtc_get_info(&info);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            solar_os_shell_io_writeln(io, "rtc: unavailable");
            return;
        }
        if (err != ESP_OK) {
            print_rtc_error(io, "status", err);
            return;
        }
        solar_os_shell_io_printf(io, "rtc: %s\n", info.provider);
        solar_os_shell_io_printf(io, "  calendar: yes\n  alarm: %s\n  timer: %s\n",
                                 (info.capabilities & SOLAR_OS_RTC_CAP_ALARM) ? "yes" : "no",
                                 (info.capabilities & SOLAR_OS_RTC_CAP_COUNTDOWN) ? "yes" : "no");
        if (info.interrupt_gpio >= 0) {
            solar_os_shell_io_printf(io, "  interrupt: gpio%d active-%s\n",
                                     info.interrupt_gpio,
                                     info.interrupt_active_level == 0 ? "low" : "high");
        } else {
            solar_os_shell_io_writeln(io, "  interrupt: not wired");
        }
        solar_os_shell_io_printf(io, "  alarm owner: %s\n  timer owner: %s\n",
                                 info.alarm_owner[0] ? info.alarm_owner : "none",
                                 info.countdown_owner[0] ? info.countdown_owner : "none");
        return;
    }

    if (argc >= 3 && strcmp(argv[1], "alarm") == 0) {
        if (argc == 3 && strcmp(argv[2], "clear") == 0) {
            const esp_err_t err = solar_os_rtc_disable_alarm_for(RTC_CLI_OWNER);
            if (err == ESP_OK) solar_os_shell_io_writeln(io, "rtc alarm: cleared");
            else print_rtc_error(io, "alarm clear", err);
            return;
        }
        if (argc >= 4 && argc <= 6 && strcmp(argv[2], "set") == 0) {
            solar_os_rtc_alarm_t alarm = {0};
            if (!parse_time(argv[3], &alarm.hour, &alarm.minute, &alarm.second)) {
                solar_os_shell_diag_invalid(io, "rtc alarm set", "time", argv[3],
                                            "HH:MM[:SS]", "rtc alarm set HH:MM[:SS] [day=N] [weekday=N]", false);
                return;
            }
            alarm.match_fields = SOLAR_OS_RTC_ALARM_MATCH_SECOND |
                SOLAR_OS_RTC_ALARM_MATCH_MINUTE | SOLAR_OS_RTC_ALARM_MATCH_HOUR;
            for (int i = 4; i < argc; i++) {
                uint32_t value = 0;
                if (strncmp(argv[i], "day=", 4) == 0 && parse_uint(argv[i] + 4, 31, &value) && value >= 1) {
                    alarm.day = (uint8_t)value;
                    alarm.match_fields |= SOLAR_OS_RTC_ALARM_MATCH_DAY;
                } else if (strncmp(argv[i], "weekday=", 8) == 0 && parse_uint(argv[i] + 8, 6, &value)) {
                    alarm.weekday = (uint8_t)value;
                    alarm.match_fields |= SOLAR_OS_RTC_ALARM_MATCH_WEEKDAY;
                } else {
                    solar_os_shell_diag_invalid(io, "rtc alarm set", "match", argv[i],
                                                "day=1..31 or weekday=0..6",
                                                "rtc alarm set HH:MM[:SS] [day=N] [weekday=N]", false);
                    return;
                }
            }
            const esp_err_t err = solar_os_rtc_set_alarm_for(RTC_CLI_OWNER, &alarm);
            if (err == ESP_OK) solar_os_shell_io_writeln(io, "rtc alarm: set");
            else print_rtc_error(io, "alarm set", err);
            return;
        }
    }

    if (argc >= 3 && strcmp(argv[1], "timer") == 0) {
        if (argc == 3 && strcmp(argv[2], "clear") == 0) {
            const esp_err_t err = solar_os_rtc_disable_countdown_for(RTC_CLI_OWNER);
            if (err == ESP_OK) solar_os_shell_io_writeln(io, "rtc timer: cleared");
            else print_rtc_error(io, "timer clear", err);
            return;
        }
        if ((argc == 4 || argc == 5) && strcmp(argv[2], "set") == 0) {
            uint32_t seconds = 0;
            const bool repeat = argc == 5 && strcmp(argv[4], "repeat") == 0;
            if (!parse_duration(argv[3], &seconds) || (argc == 5 && !repeat)) {
                solar_os_shell_diag_invalid(io, "rtc timer set", "duration", argv[3],
                                            "number with s, m, h, or d suffix",
                                            "rtc timer set <duration> [repeat]", false);
                return;
            }
            const esp_err_t err = solar_os_rtc_set_countdown_for(RTC_CLI_OWNER, seconds, repeat);
            if (err == ESP_OK) solar_os_shell_io_writeln(io, "rtc timer: set");
            else print_rtc_error(io, "timer set", err);
            return;
        }
    }

    if (argc == 2 && strcmp(argv[1], "pending") == 0) {
        uint32_t pending = 0;
        const esp_err_t err = solar_os_rtc_get_interrupt_status(&pending);
        if (err != ESP_OK) {
            print_rtc_error(io, "pending", err);
        } else if (pending == 0) {
            solar_os_shell_io_writeln(io, "rtc pending: none");
        } else {
            solar_os_shell_io_printf(io, "rtc pending:%s%s\n",
                                     pending & SOLAR_OS_RTC_INTERRUPT_ALARM ? " alarm" : "",
                                     pending & SOLAR_OS_RTC_INTERRUPT_COUNTDOWN ? " timer" : "");
        }
        return;
    }
    if (argc == 3 && strcmp(argv[1], "ack") == 0) {
        uint32_t mask = strcmp(argv[2], "alarm") == 0 ? SOLAR_OS_RTC_INTERRUPT_ALARM :
            strcmp(argv[2], "timer") == 0 ? SOLAR_OS_RTC_INTERRUPT_COUNTDOWN :
            strcmp(argv[2], "all") == 0 ? SOLAR_OS_RTC_INTERRUPT_ALARM | SOLAR_OS_RTC_INTERRUPT_COUNTDOWN : 0;
        const esp_err_t err = mask ? solar_os_rtc_clear_interrupt_status(mask) : ESP_ERR_INVALID_ARG;
        if (err == ESP_OK) solar_os_shell_io_writeln(io, "rtc pending: acknowledged");
        else print_rtc_error(io, "ack", err);
        return;
    }
    solar_os_shell_io_writeln(io, "usage: rtc [status|alarm set|alarm clear|timer set|timer clear|pending|ack]");
}

static bool parse_action(solar_os_context_t *ctx,
                         solar_os_shell_io_t *io,
                         int argc,
                         char **argv,
                         int index,
                         solar_os_schedule_action_t *action,
                         char *value,
                         size_t value_len)
{
    if (index >= argc) {
        return false;
    }
    if (strcmp(argv[index], "alarm") == 0 && index + 1 == argc) {
        *action = SOLAR_OS_SCHEDULE_ACTION_ALARM;
        value[0] = '\0';
        return true;
    }
    if (strcmp(argv[index], "run") == 0 && index + 2 == argc) {
        *action = SOLAR_OS_SCHEDULE_ACTION_SCRIPT;
        return solar_os_shell_resolve_path_for_command(ctx, io, "schedule", argv[index + 1],
                                                        value, value_len);
    }
    return false;
}

static void print_schedule_entry(solar_os_shell_io_t *io,
                                 const solar_os_schedule_entry_t *entry)
{
    static const char *const weekday_names[] = {
        "sun", "mon", "tue", "wed", "thu", "fri", "sat",
    };
    solar_os_shell_io_printf(io, "%s  %s  %s ",
                             entry->name,
                             entry->enabled ? "enabled" : "disabled",
                             solar_os_schedule_kind_name(entry->kind));
    if (entry->kind == SOLAR_OS_SCHEDULE_ONCE_RELATIVE ||
        entry->kind == SOLAR_OS_SCHEDULE_INTERVAL) {
        solar_os_shell_io_printf(io, "%" PRIu32 "s", entry->interval_seconds);
    } else if (entry->kind == SOLAR_OS_SCHEDULE_ONCE_CALENDAR) {
        time_t timestamp = (time_t)entry->at_utc_seconds;
        struct tm tm;
        solar_os_datetime_t utc = {0};
        solar_os_datetime_t local = {0};
        if (gmtime_r(&timestamp, &tm) != NULL) {
            utc = (solar_os_datetime_t) {
                .year = (uint16_t)(tm.tm_year + 1900),
                .month = (uint8_t)(tm.tm_mon + 1),
                .day = (uint8_t)tm.tm_mday,
                .hour = (uint8_t)tm.tm_hour,
                .minute = (uint8_t)tm.tm_min,
                .second = (uint8_t)tm.tm_sec,
                .weekday = (uint8_t)tm.tm_wday,
                .clock_integrity = true,
            };
        }
        if (solar_os_time_utc_to_local(&utc, &local) == ESP_OK) {
            solar_os_shell_io_printf(io, "%04u-%02u-%02u %02u:%02u:%02u",
                                     (unsigned)local.year, (unsigned)local.month,
                                     (unsigned)local.day, (unsigned)local.hour,
                                     (unsigned)local.minute, (unsigned)local.second);
        } else {
            solar_os_shell_io_printf(io, "utc=%" PRIu64, entry->at_utc_seconds);
        }
    } else {
        if (entry->kind == SOLAR_OS_SCHEDULE_WEEKLY) {
            bool separator = false;
            for (size_t i = 0; i < 7; i++) {
                if ((entry->weekdays & (1U << i)) != 0) {
                    solar_os_shell_io_printf(io, "%s%s",
                                             separator ? "," : "",
                                             weekday_names[i]);
                    separator = true;
                }
            }
            solar_os_shell_io_write(io, " ");
        }
        solar_os_shell_io_printf(io, "%02u:%02u:%02u",
                                 (unsigned)entry->hour,
                                 (unsigned)entry->minute,
                                 (unsigned)entry->second);
    }
    solar_os_shell_io_printf(io, "  %s", solar_os_schedule_action_name(entry->action));
    if (entry->action == SOLAR_OS_SCHEDULE_ACTION_SCRIPT) {
        solar_os_shell_io_printf(io, " %s", entry->value);
    }
    if (!entry->persistent) {
        solar_os_shell_io_write(io, "  transient");
    }
    solar_os_shell_io_newline(io);
}

static void schedule_add(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_context_io(ctx);
    if (argc < 6) {
        solar_os_shell_io_writeln(io, "usage: schedule add <name> <in|every|at|daily|weekly> ... <alarm|run script>");
        return;
    }
    const char *name = argv[2];
    const char *kind = argv[3];
    int action_index = 5;
    uint32_t duration = 0;
    uint16_t year = 0;
    uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0, weekdays = 0;
    if ((strcmp(kind, "in") == 0 || strcmp(kind, "every") == 0)) {
        if (!parse_duration(argv[4], &duration)) {
            solar_os_shell_diag_invalid(io, "schedule add", "duration", argv[4],
                                        "number with s, m, h, or d suffix", NULL, false);
            return;
        }
    } else if (strcmp(kind, "at") == 0) {
        if (argc < 7 || !parse_date(argv[4], &year, &month, &day) ||
            !parse_time(argv[5], &hour, &minute, &second)) {
            solar_os_shell_io_writeln(io, "usage: schedule add <name> at YYYY-MM-DD HH:MM[:SS] <alarm|run script>");
            return;
        }
        action_index = 6;
    } else if (strcmp(kind, "daily") == 0) {
        if (!parse_time(argv[4], &hour, &minute, &second)) {
            solar_os_shell_io_writeln(io, "schedule: invalid daily time");
            return;
        }
    } else if (strcmp(kind, "weekly") == 0) {
        if (argc < 7 || !parse_weekdays(argv[4], &weekdays) ||
            !parse_time(argv[5], &hour, &minute, &second)) {
            solar_os_shell_io_writeln(io, "usage: schedule add <name> weekly mon,wed HH:MM[:SS] <alarm|run script>");
            return;
        }
        action_index = 6;
    } else {
        solar_os_shell_io_writeln(io, "schedule: type must be in, every, at, daily, or weekly");
        return;
    }

    solar_os_schedule_action_t action;
    char value[SOLAR_OS_SCHEDULE_VALUE_MAX];
    if (!parse_action(ctx, io, argc, argv, action_index, &action, value, sizeof(value))) {
        solar_os_shell_io_writeln(io, "schedule: action must be 'alarm' or 'run <script>'");
        return;
    }
    esp_err_t err;
    if (strcmp(kind, "in") == 0) {
        err = solar_os_schedule_add_relative(name, duration, action, value, true);
    } else if (strcmp(kind, "every") == 0) {
        err = solar_os_schedule_add_interval(name, duration, action, value);
    } else if (strcmp(kind, "at") == 0) {
        err = solar_os_schedule_add_at(name, year, month, day, hour, minute, second, action, value);
    } else if (strcmp(kind, "daily") == 0) {
        err = solar_os_schedule_add_daily(name, hour, minute, second, action, value);
    } else {
        err = solar_os_schedule_add_weekly(name, weekdays, hour, minute, second, action, value);
    }
    if (err == ESP_OK) {
        solar_os_shell_io_printf(io, "schedule: added %s\n", name);
    } else {
        solar_os_shell_io_printf(io, "schedule: add failed: %s\n", solar_os_shell_error_text(err));
    }
}

void solar_os_shell_cmd_schedule(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_context_io(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        const size_t count = solar_os_schedule_count();
        if (count == 0) {
            solar_os_shell_io_writeln(io, "schedule: no entries");
        }
        for (size_t i = 0; i < count; i++) {
            solar_os_schedule_entry_t entry;
            if (solar_os_schedule_get(i, &entry)) print_schedule_entry(io, &entry);
        }
        return;
    }
    if (strcmp(argv[1], "add") == 0) {
        schedule_add(ctx, argc, argv);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "show") == 0) {
        solar_os_schedule_entry_t entry;
        const esp_err_t err = solar_os_schedule_get_by_name(argv[2], &entry);
        if (err == ESP_OK) {
            print_schedule_entry(io, &entry);
            solar_os_shell_io_printf(io, "  runs: %" PRIu32 "  skipped: %" PRIu32 "\n",
                                     entry.run_count, entry.skipped_count);
        } else {
            solar_os_shell_io_printf(io, "schedule: %s not found\n", argv[2]);
        }
        return;
    }
    if (argc == 3 && (strcmp(argv[1], "enable") == 0 || strcmp(argv[1], "disable") == 0)) {
        const bool enabled = strcmp(argv[1], "enable") == 0;
        const esp_err_t err = solar_os_schedule_set_enabled(argv[2], enabled);
        solar_os_shell_io_printf(io, "schedule: %s%s%s\n", argv[2],
                                 err == ESP_OK ? " " : " failed: ",
                                 err == ESP_OK ? (enabled ? "enabled" : "disabled") : solar_os_shell_error_text(err));
        return;
    }
    if (argc == 3 && strcmp(argv[1], "remove") == 0) {
        const esp_err_t err = solar_os_schedule_remove(argv[2]);
        solar_os_shell_io_printf(io, "schedule: %s%s\n", argv[2],
                                 err == ESP_OK ? " removed" : " not found");
        return;
    }
    if (argc == 3 && strcmp(argv[1], "run") == 0) {
        const esp_err_t err = solar_os_schedule_run(argv[2]);
        solar_os_shell_io_printf(io, "schedule: %s%s\n", argv[2],
                                 err == ESP_OK ? " started" : " could not start");
        return;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "stop") == 0) {
        if (argc == 3) {
            char active_name[SOLAR_OS_SCHEDULE_NAME_MAX];
            if (!solar_os_schedule_alarm_active(active_name, sizeof(active_name)) ||
                strcmp(active_name, argv[2]) != 0) {
                solar_os_shell_io_printf(io, "schedule: %s is not ringing\n", argv[2]);
                return;
            }
        }
        solar_os_schedule_stop_alarm();
        solar_os_shell_io_writeln(io, "schedule: alarm stopped");
        return;
    }
    solar_os_shell_io_writeln(io, "usage: schedule [list|show|add|enable|disable|remove|run|stop] ...");
}
