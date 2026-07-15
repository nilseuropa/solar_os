#include "solar_os_shell_commands.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_irrig.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

/* Weekday letters, Monday-first, matching the schedule day mask. */
static const char irrig_day_letters[7] = {'L', 'M', 'M', 'J', 'V', 'S', 'D'};

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void irrig_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  irrig [status]");
    solar_os_shell_io_writeln(term, "  irrig zones <1..8>");
    solar_os_shell_io_writeln(term, "  irrig mode auto|manual");
    solar_os_shell_io_writeln(term, "  irrig on <zone> | irrig off <zone>   (manual mode)");
    solar_os_shell_io_writeln(term, "  irrig set <zone> <slot 1-4> <HH:MM> <HH:MM> <days> <A|->");
    solar_os_shell_io_writeln(term, "    days: 7 chars Monday-first, '-' = skip, e.g. LMMJV--");
    solar_os_shell_io_writeln(term, "  irrig pin <zone> <gpio|->");
    solar_os_shell_io_writeln(term, "zones are letters: A, B, C...");
}

static bool irrig_parse_zone(const char *text, uint8_t *zone)
{
    if (text == NULL || zone == NULL || text[1] != '\0') {
        return false;
    }

    char ch = text[0];
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch < 'A' || ch >= (char)('A' + solar_os_irrig_zone_count())) {
        return false;
    }

    *zone = (uint8_t)(ch - 'A');
    return true;
}

static bool irrig_parse_time(const char *text, uint16_t *minute_of_day)
{
    if (text == NULL || minute_of_day == NULL || strlen(text) != 5 || text[2] != ':') {
        return false;
    }

    for (int i = 0; i < 5; i++) {
        if (i != 2 && (text[i] < '0' || text[i] > '9')) {
            return false;
        }
    }

    const int hour = ((text[0] - '0') * 10) + (text[1] - '0');
    const int minute = ((text[3] - '0') * 10) + (text[4] - '0');
    if (hour > 23 || minute > 59) {
        return false;
    }

    *minute_of_day = (uint16_t)((hour * 60) + minute);
    return true;
}

static bool irrig_parse_days(const char *text, uint8_t *days)
{
    if (text == NULL || days == NULL || strlen(text) != 7) {
        return false;
    }

    uint8_t mask = 0;
    for (int i = 0; i < 7; i++) {
        if (text[i] != '-') {
            mask |= (uint8_t)(1U << i);
        }
    }

    *days = mask;
    return true;
}

static void irrig_format_days(uint8_t days, char *out)
{
    for (int i = 0; i < 7; i++) {
        out[i] = (days & (1U << i)) != 0 ? irrig_day_letters[i] : '-';
    }
    out[7] = '\0';
}

static void irrig_print_status(solar_os_shell_io_t *term)
{
    const uint8_t zones = solar_os_irrig_zone_count();
    if (zones == 0) {
        solar_os_shell_io_writeln(term, "irrig: engine not started (job start irrigd)");
        return;
    }

    solar_os_shell_io_printf(term,
                             "mode: %s, zones: %u\n",
                             solar_os_irrig_mode() == SOLAR_OS_IRRIG_MODE_AUTO ? "auto" : "manual",
                             (unsigned)zones);

    for (uint8_t zone = 0; zone < zones; zone++) {
        solar_os_irrig_zone_status_t status;
        if (solar_os_irrig_zone_status(zone, &status) != ESP_OK) {
            continue;
        }

        solar_os_shell_io_printf(term,
                                 "zone %c: %s (sched %s, manual %s, pin %d)\n",
                                 'A' + zone,
                                 status.output_on ? "ON" : "off",
                                 status.schedule_active ? "on" : "off",
                                 status.manual_on ? "on" : "off",
                                 status.pin);

        for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
            solar_os_irrig_schedule_t schedule;
            if (solar_os_irrig_get_schedule(zone, slot, &schedule) != ESP_OK) {
                continue;
            }

            char days[8];
            irrig_format_days(schedule.days, days);
            solar_os_shell_io_printf(term,
                                     "  %u: %02u:%02u-%02u:%02u %s %c\n",
                                     (unsigned)(slot + 1U),
                                     (unsigned)(schedule.start_minute / 60U),
                                     (unsigned)(schedule.start_minute % 60U),
                                     (unsigned)(schedule.end_minute / 60U),
                                     (unsigned)(schedule.end_minute % 60U),
                                     days,
                                     schedule.active ? 'A' : '-');
        }
    }
}

void solar_os_shell_cmd_irrig(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        irrig_print_status(term);
        return;
    }

    if (solar_os_irrig_zone_count() == 0) {
        solar_os_shell_io_writeln(term, "irrig: engine not started (job start irrigd)");
        return;
    }

    if (argc == 3 && strcmp(argv[1], "zones") == 0) {
        char *end = NULL;
        errno = 0;
        const unsigned long count = strtoul(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' ||
            solar_os_irrig_set_zone_count((uint8_t)count) != ESP_OK) {
            solar_os_shell_io_printf(term, "irrig zones: 1..%u\n", (unsigned)SOLAR_OS_IRRIG_ZONES_MAX);
            return;
        }
        solar_os_shell_io_printf(term, "zones: %lu\n", count);
        return;
    }

    if (argc == 3 && strcmp(argv[1], "mode") == 0) {
        if (strcmp(argv[2], "auto") == 0) {
            solar_os_irrig_set_mode(SOLAR_OS_IRRIG_MODE_AUTO);
        } else if (strcmp(argv[2], "manual") == 0) {
            solar_os_irrig_set_mode(SOLAR_OS_IRRIG_MODE_MANUAL);
        } else {
            irrig_print_usage(term);
            return;
        }
        solar_os_shell_io_printf(term, "mode: %s\n", argv[2]);
        return;
    }

    if (argc == 3 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        uint8_t zone = 0;
        if (!irrig_parse_zone(argv[2], &zone)) {
            irrig_print_usage(term);
            return;
        }

        const bool on = strcmp(argv[1], "on") == 0;
        (void)solar_os_irrig_set_manual(zone, on);
        solar_os_shell_io_printf(term, "zone %c manual: %s", 'A' + zone, on ? "on" : "off");
        if (solar_os_irrig_mode() != SOLAR_OS_IRRIG_MODE_MANUAL) {
            solar_os_shell_io_write(term, " (inactive until 'irrig mode manual')");
        }
        solar_os_shell_io_writeln(term, "");
        return;
    }

    if (argc == 8 && strcmp(argv[1], "set") == 0) {
        uint8_t zone = 0;
        char *end = NULL;
        errno = 0;
        const unsigned long slot = strtoul(argv[3], &end, 10);
        solar_os_irrig_schedule_t schedule = {0};

        if (!irrig_parse_zone(argv[2], &zone) ||
            errno != 0 || end == argv[3] || *end != '\0' ||
            slot < 1 || slot > SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE ||
            !irrig_parse_time(argv[4], &schedule.start_minute) ||
            !irrig_parse_time(argv[5], &schedule.end_minute) ||
            !irrig_parse_days(argv[6], &schedule.days) ||
            argv[7][1] != '\0' ||
            (argv[7][0] != 'A' && argv[7][0] != 'a' && argv[7][0] != '-')) {
            irrig_print_usage(term);
            return;
        }
        schedule.active = argv[7][0] != '-';

        const esp_err_t err = solar_os_irrig_set_schedule(zone, (uint8_t)(slot - 1U), &schedule);
        if (err != ESP_OK) {
            solar_os_shell_io_writeln(term, "irrig set: invalid schedule (start must be before end)");
            return;
        }

        char days[8];
        irrig_format_days(schedule.days, days);
        solar_os_shell_io_printf(term,
                                 "zone %c slot %lu: %s-%s %s %c\n",
                                 'A' + zone,
                                 slot,
                                 argv[4],
                                 argv[5],
                                 days,
                                 schedule.active ? 'A' : '-');
        return;
    }

    if (argc == 4 && strcmp(argv[1], "pin") == 0) {
        uint8_t zone = 0;
        int pin = -1;
        if (!irrig_parse_zone(argv[2], &zone)) {
            irrig_print_usage(term);
            return;
        }
        if (strcmp(argv[3], "-") != 0) {
            char *end = NULL;
            errno = 0;
            const long parsed = strtol(argv[3], &end, 10);
            if (errno != 0 || end == argv[3] || *end != '\0') {
                irrig_print_usage(term);
                return;
            }
            pin = (int)parsed;
        }

        if (solar_os_irrig_set_zone_pin(zone, pin) != ESP_OK) {
            solar_os_shell_io_writeln(term, "irrig pin: not a valid output gpio");
            return;
        }
        solar_os_shell_io_printf(term, "zone %c pin: %d\n", 'A' + zone, pin);
        return;
    }

    irrig_print_usage(term);
}
