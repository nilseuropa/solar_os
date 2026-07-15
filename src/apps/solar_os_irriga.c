#include "solar_os_irriga.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_config.h"
#include "solar_os_gfx.h"
#include "solar_os_irrig.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_time.h"
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
#include "solar_os_wifi.h"
#endif

/*
 * Irrigation controller UI -- the graphical front-end for the irrigd
 * schedule engine (which keeps running on its own whether this app is
 * open or not; see solar_os_irrigd_job.c). Everything is key-driven
 * (arrows/Enter/Esc plus a few letter shortcuts), so it works the same
 * from CardKB, a BLE keyboard, or the remote web page. Touch comes
 * later as an additional input, not a requirement.
 *
 * The home screen reproduces the layout of the standalone controller
 * this was ported from: seven-segment clock and date top-left, IP and
 * one button per zone top-right, and a two-column grid of zone panels
 * listing every schedule slot. A zone whose relay is on gets a double
 * border and a filled letter button; the slot window matching the
 * current time is drawn inverted. White-on-black, like the original.
 *
 * Other screens: ZONE (one zone's 4 slots), EDIT (field-by-field slot
 * editor), SETTINGS (zone count, mode, manual clock set for sites
 * without NTP).
 */
#define IRRIGA_CLOCK_X 4
#define IRRIGA_CLOCK_Y 4
#define IRRIGA_DIGIT_W 16
#define IRRIGA_DIGIT_H 30
#define IRRIGA_SEG_T 4
#define IRRIGA_CLOCK_GAP 3
#define IRRIGA_COLON_W 8
#define IRRIGA_DATE_BASELINE 46
#define IRRIGA_PANELS_TOP 52
#define IRRIGA_PANEL_ROW_STEP 17
#define IRRIGA_BUTTON_SIZE 22
#define IRRIGA_BUTTON_GAP 6
#define IRRIGA_RIGHT_MARGIN 4
#define IRRIGA_FOOTER_MARGIN 4
#define IRRIGA_EDIT_FIELDS 12U
#define IRRIGA_CLOCK_FIELDS 5U

typedef enum {
    IRRIGA_SCREEN_HOME,
    IRRIGA_SCREEN_ZONE,
    IRRIGA_SCREEN_EDIT,
    IRRIGA_SCREEN_SETTINGS,
    IRRIGA_SCREEN_SETCLOCK,
} irriga_screen_t;

typedef struct {
    irriga_screen_t screen;
    bool suspended;
    uint32_t last_render_s;
    uint8_t sel_zone;
    uint8_t sel_slot;
    uint8_t sel_setting;
    uint8_t edit_field;
    solar_os_irrig_schedule_t edit_schedule;
    uint8_t clock_field;
    solar_os_datetime_t clock_edit;
    char message[48];
} irriga_state_t;

static const char irriga_day_letters[7] = {'L', 'M', 'M', 'J', 'V', 'S', 'D'};
static const char *irriga_weekday_names[7] = {"Dum", "Lun", "Mar", "Mie", "Joi", "Vin", "Sam"};

static irriga_state_t irriga;

static uint8_t irriga_weekday_of(int year, int month, int day)
{
    /* Sakamoto's algorithm, tm_wday-compatible (0 = Sunday). */
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year -= 1;
    }
    return (uint8_t)((year + year / 4 - year / 100 + year / 400 +
                      offsets[month - 1] + day) % 7);
}

static bool irriga_engine_running(void)
{
    solar_os_job_status_t status;
    return solar_os_jobs_get_by_name("irrigd", &status) &&
        status.state == SOLAR_OS_JOB_RUNNING;
}

static void irriga_format_days(uint8_t days, char *out)
{
    for (int i = 0; i < 7; i++) {
        out[i] = (days & (1U << i)) != 0 ? irriga_day_letters[i] : '-';
    }
    out[7] = '\0';
}

static bool irriga_slot_active_now(const solar_os_irrig_schedule_t *schedule,
                                   const solar_os_datetime_t *now,
                                   bool time_ok)
{
    if (!time_ok || !schedule->active) {
        return false;
    }
    const uint8_t day_bit = now->weekday == 0 ? 6U : (uint8_t)(now->weekday - 1U);
    if ((schedule->days & (1U << day_bit)) == 0) {
        return false;
    }
    const uint16_t minute = (uint16_t)((now->hour * 60U) + now->minute);
    return minute >= schedule->start_minute && minute < schedule->end_minute;
}

/* Rectangle with the corners knocked off -- close enough to the
 * rounded panels of the original at one bit per pixel. */
static void irriga_round_rect(solar_os_gfx_t *gfx, int x, int y, int w, int h)
{
    solar_os_gfx_line(gfx, x + 3, y, x + w - 4, y);
    solar_os_gfx_line(gfx, x + 3, y + h - 1, x + w - 4, y + h - 1);
    solar_os_gfx_line(gfx, x, y + 3, x, y + h - 4);
    solar_os_gfx_line(gfx, x + w - 1, y + 3, x + w - 1, y + h - 4);
    solar_os_gfx_pixel(gfx, x + 1, y + 1);
    solar_os_gfx_pixel(gfx, x + 2, y + 1);
    solar_os_gfx_pixel(gfx, x + 1, y + 2);
    solar_os_gfx_pixel(gfx, x + w - 2, y + 1);
    solar_os_gfx_pixel(gfx, x + w - 3, y + 1);
    solar_os_gfx_pixel(gfx, x + w - 2, y + 2);
    solar_os_gfx_pixel(gfx, x + 1, y + h - 2);
    solar_os_gfx_pixel(gfx, x + 2, y + h - 2);
    solar_os_gfx_pixel(gfx, x + 1, y + h - 3);
    solar_os_gfx_pixel(gfx, x + w - 2, y + h - 2);
    solar_os_gfx_pixel(gfx, x + w - 3, y + h - 2);
    solar_os_gfx_pixel(gfx, x + w - 2, y + h - 3);
}

static void irriga_draw_7seg_digit(solar_os_gfx_t *gfx, int value, int x, int y)
{
    static const uint8_t digit_masks[] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f,
    };
    if (value < 0 || value > 9) {
        return;
    }

    const int w = IRRIGA_DIGIT_W;
    const int h = IRRIGA_DIGIT_H;
    const int t = IRRIGA_SEG_T;
    const int half = (h - t) / 2;
    const uint8_t mask = digit_masks[value];

    if (mask & 0x01) { /* A: top */
        solar_os_gfx_fill_rect(gfx, x + 2, y, w - 4, t);
    }
    if (mask & 0x02) { /* B: top right */
        solar_os_gfx_fill_rect(gfx, x + w - t, y + 2, t, half - 3);
    }
    if (mask & 0x04) { /* C: bottom right */
        solar_os_gfx_fill_rect(gfx, x + w - t, y + half + t + 1, t, half - 3);
    }
    if (mask & 0x08) { /* D: bottom */
        solar_os_gfx_fill_rect(gfx, x + 2, y + h - t, w - 4, t);
    }
    if (mask & 0x10) { /* E: bottom left */
        solar_os_gfx_fill_rect(gfx, x, y + half + t + 1, t, half - 3);
    }
    if (mask & 0x20) { /* F: top left */
        solar_os_gfx_fill_rect(gfx, x, y + 2, t, half - 3);
    }
    if (mask & 0x40) { /* G: middle */
        solar_os_gfx_fill_rect(gfx, x + 2, y + half, w - 4, t);
    }
}

static int irriga_draw_7seg_colon(solar_os_gfx_t *gfx, int x, int y)
{
    const int dot = IRRIGA_SEG_T;
    const int cx = x + (IRRIGA_COLON_W - dot) / 2;
    solar_os_gfx_fill_rect(gfx, cx, y + (IRRIGA_DIGIT_H / 3) - (dot / 2), dot, dot);
    solar_os_gfx_fill_rect(gfx, cx, y + (2 * IRRIGA_DIGIT_H / 3) - (dot / 2), dot, dot);
    return IRRIGA_COLON_W;
}

static void irriga_draw_7seg_clock(solar_os_gfx_t *gfx,
                                   const solar_os_datetime_t *now,
                                   bool time_ok)
{
    int x = IRRIGA_CLOCK_X;
    const int y = IRRIGA_CLOCK_Y;
    const int values[3] = {now->hour, now->minute, now->second};

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    for (int group = 0; group < 3; group++) {
        for (int digit = 0; digit < 2; digit++) {
            const int value = time_ok ?
                (digit == 0 ? values[group] / 10 : values[group] % 10) : -1;
            if (time_ok) {
                irriga_draw_7seg_digit(gfx, value, x, y);
            } else {
                /* Middle segment only: the classic unset-clock dash. */
                solar_os_gfx_fill_rect(gfx, x + 2,
                                       y + (IRRIGA_DIGIT_H - IRRIGA_SEG_T) / 2,
                                       IRRIGA_DIGIT_W - 4, IRRIGA_SEG_T);
            }
            x += IRRIGA_DIGIT_W + IRRIGA_CLOCK_GAP;
        }
        if (group < 2) {
            x += irriga_draw_7seg_colon(gfx, x, y) + IRRIGA_CLOCK_GAP;
        }
    }
}

static void irriga_footer(solar_os_gfx_t *gfx, const char *text)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    solar_os_gfx_text(gfx,
                      IRRIGA_FOOTER_MARGIN,
                      (int)solar_os_gfx_height(gfx) - IRRIGA_FOOTER_MARGIN,
                      text);
}

static void irriga_message_line(solar_os_gfx_t *gfx, int baseline)
{
    if (irriga.message[0] == '\0') {
        return;
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, IRRIGA_FOOTER_MARGIN, baseline, irriga.message);
}

static void irriga_render_home_header(solar_os_gfx_t *gfx,
                                      const solar_os_datetime_t *now,
                                      bool time_ok)
{
    const int width = (int)solar_os_gfx_width(gfx);
    char text[40];

    irriga_draw_7seg_clock(gfx, now, time_ok);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    if (time_ok) {
        snprintf(text, sizeof(text), "%02u/%02u/%04u %s",
                 (unsigned)now->day, (unsigned)now->month, (unsigned)now->year,
                 irriga_weekday_names[now->weekday % 7]);
    } else {
        strlcpy(text, "ceas nesetat", sizeof(text));
    }
    solar_os_gfx_text(gfx, IRRIGA_CLOCK_X, IRRIGA_DATE_BASELINE, text);

    /* Status cues share the date line so the grid keeps its space. */
    if (!irriga_engine_running()) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
        solar_os_gfx_text(gfx, 116, IRRIGA_DATE_BASELINE, "! irrigd off");
    } else if (solar_os_irrig_mode() == SOLAR_OS_IRRIG_MODE_MANUAL) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
        solar_os_gfx_text(gfx, 116, IRRIGA_DATE_BASELINE, "MANUAL");
    }

    /* IP top right. */
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (wifi.state == SOLAR_OS_WIFI_STATE_CONNECTED && wifi.ip[0] != '\0') {
        snprintf(text, sizeof(text), "IP:%s", wifi.ip);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        const int ip_w = (int)solar_os_gfx_text_width(gfx, text);
        solar_os_gfx_text(gfx, width - IRRIGA_RIGHT_MARGIN - ip_w, 12, text);
    }
#endif

    /* One lettered button per zone, right-aligned under the IP.
     * Filled = that zone's relay is on. Many zones get smaller
     * buttons so the row never reaches the clock digits. */
    const uint8_t zones = solar_os_irrig_zone_count();
    const int bsize = zones > 6 ? 16 : IRRIGA_BUTTON_SIZE;
    const int bgap = zones > 6 ? 4 : IRRIGA_BUTTON_GAP;
    const int buttons_w = (zones * bsize) + ((zones - 1) * bgap);
    int bx = width - IRRIGA_RIGHT_MARGIN - buttons_w;
    const int by = 20;

    solar_os_gfx_set_font(gfx, zones > 6 ? SOLAR_OS_GFX_FONT_BOLD_12 : SOLAR_OS_GFX_FONT_BOLD_14);
    for (uint8_t zone = 0; zone < zones; zone++) {
        solar_os_irrig_zone_status_t status = {0};
        (void)solar_os_irrig_zone_status(zone, &status);

        char letter[2] = {(char)('A' + zone), '\0'};
        const int letter_w = (int)solar_os_gfx_text_width(gfx, letter);
        const int lx = bx + (bsize - letter_w) / 2;
        const int ly = by + (bsize / 2) + 5;

        if (status.output_on) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, bx, by, bsize, bsize);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_text(gfx, lx, ly, letter);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            irriga_round_rect(gfx, bx, by, bsize, bsize);
            solar_os_gfx_text(gfx, lx, ly, letter);
        }
        bx += bsize + bgap;
    }
}

static void irriga_render_zone_panel(solar_os_gfx_t *gfx,
                                     uint8_t zone,
                                     int px,
                                     int py,
                                     int pw,
                                     int ph,
                                     const solar_os_datetime_t *now,
                                     bool time_ok)
{
    char text[32];
    char days[8];

    solar_os_irrig_zone_status_t status = {0};
    (void)solar_os_irrig_zone_status(zone, &status);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    irriga_round_rect(gfx, px, py, pw, ph);
    if (status.output_on) {
        irriga_round_rect(gfx, px + 1, py + 1, pw - 2, ph - 2);
    }

    /* The keyboard cursor is a dashed inner frame -- deliberately
     * different from the solid double border, which means "relay on". */
    if (zone == irriga.sel_zone) {
        solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
        irriga_round_rect(gfx, px + 2, py + 2, pw - 4, ph - 4);
        solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
    }

    snprintf(text, sizeof(text), "Zone %c", 'A' + zone);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, px + 7, py + 14, text);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    int baseline = py + 14 + IRRIGA_PANEL_ROW_STEP;
    for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
        if (baseline > py + ph - 4) {
            break;
        }

        solar_os_irrig_schedule_t schedule;
        if (solar_os_irrig_get_schedule(zone, slot, &schedule) != ESP_OK) {
            continue;
        }

        irriga_format_days(schedule.days, days);
        snprintf(text, sizeof(text), "%02u:%02u-%02u:%02u %s %c",
                 (unsigned)(schedule.start_minute / 60U),
                 (unsigned)(schedule.start_minute % 60U),
                 (unsigned)(schedule.end_minute / 60U),
                 (unsigned)(schedule.end_minute % 60U),
                 days,
                 schedule.active ? 'A' : '-');

        if (irriga_slot_active_now(&schedule, now, time_ok)) {
            const int row_w = (int)solar_os_gfx_text_width(gfx, text);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, px + 4, baseline - 11, row_w + 6, 14);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        } else {
            solar_os_gfx_set_color(gfx, schedule.active ?
                                            SOLAR_OS_GFX_COLOR_WHITE :
                                            SOLAR_OS_GFX_COLOR_LIGHT);
        }
        solar_os_gfx_text(gfx, px + 7, baseline, text);
        baseline += IRRIGA_PANEL_ROW_STEP;
    }
}

static uint8_t irriga_grid_rows(void)
{
    const uint8_t zones = solar_os_irrig_zone_count();
    uint8_t rows = (uint8_t)((zones + 1U) / 2U);
    return rows == 0 ? 1U : rows;
}

static void irriga_render_home(solar_os_gfx_t *gfx,
                               const solar_os_datetime_t *now,
                               bool time_ok)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    irriga_render_home_header(gfx, now, time_ok);

    /* Zone panels, two columns, filled column-major like the original
     * (A above B on the left, C above D on the right). */
    const uint8_t zones = solar_os_irrig_zone_count();
    const uint8_t rows = irriga_grid_rows();
    const int pw = (width - 3 * 2) / 2;
    int ph = ((height - IRRIGA_PANELS_TOP - 2) / rows) - 2;
    if (ph > 92) {
        ph = 92;
    }

    for (uint8_t zone = 0; zone < zones; zone++) {
        const int col = zone / rows;
        const int row = zone % rows;
        const int px = 2 + col * (pw + 2);
        const int py = IRRIGA_PANELS_TOP + row * (ph + 2);
        irriga_render_zone_panel(gfx, zone, px, py, pw, ph, now, time_ok);
    }

    irriga_message_line(gfx, height - IRRIGA_FOOTER_MARGIN);
}

static void irriga_render_zone(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    char text[48];

    solar_os_irrig_zone_status_t status = {0};
    (void)solar_os_irrig_zone_status(irriga.sel_zone, &status);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    snprintf(text, sizeof(text), "Zone %c  %s", 'A' + irriga.sel_zone,
             status.output_on ? "ON" : "off");
    solar_os_gfx_text(gfx, IRRIGA_FOOTER_MARGIN, 18, text);
    solar_os_gfx_line(gfx, 0, 26, width - 1, 26);

    int y = 56;
    for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
        solar_os_irrig_schedule_t schedule;
        if (solar_os_irrig_get_schedule(irriga.sel_zone, slot, &schedule) != ESP_OK) {
            continue;
        }

        char days[8];
        irriga_format_days(schedule.days, days);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        if (slot == irriga.sel_slot) {
            solar_os_gfx_text(gfx, IRRIGA_FOOTER_MARGIN, y, ">");
        }
        snprintf(text, sizeof(text), "%u  %02u:%02u-%02u:%02u  %s  %c",
                 (unsigned)(slot + 1U),
                 (unsigned)(schedule.start_minute / 60U),
                 (unsigned)(schedule.start_minute % 60U),
                 (unsigned)(schedule.end_minute / 60U),
                 (unsigned)(schedule.end_minute % 60U),
                 days,
                 schedule.active ? 'A' : '-');
        solar_os_gfx_text(gfx, 20, y, text);
        y += 28;
    }

    irriga_message_line(gfx, y + 6);
    irriga_footer(gfx, "ENT edit  ESC back");
}

static void irriga_render_edit(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    char text[48];

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    snprintf(text, sizeof(text), "Zone %c  slot %u",
             'A' + irriga.sel_zone, (unsigned)(irriga.sel_slot + 1U));
    solar_os_gfx_text(gfx, IRRIGA_FOOTER_MARGIN, 18, text);
    solar_os_gfx_line(gfx, 0, 26, width - 1, 26);

    /* Times line: fields 0..3 map to char positions in "HH:MM - HH:MM". */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_20);
    const int char_w = (int)solar_os_gfx_text_width(gfx, "0");
    const int times_x = 24;
    const int times_y = 92;
    snprintf(text, sizeof(text), "%02u:%02u - %02u:%02u",
             (unsigned)(irriga.edit_schedule.start_minute / 60U),
             (unsigned)(irriga.edit_schedule.start_minute % 60U),
             (unsigned)(irriga.edit_schedule.end_minute / 60U),
             (unsigned)(irriga.edit_schedule.end_minute % 60U));
    solar_os_gfx_text(gfx, times_x, times_y, text);

    if (irriga.edit_field < 4) {
        static const int field_char[4] = {0, 3, 8, 11};
        const int x = times_x + (field_char[irriga.edit_field] * char_w) - 2;
        solar_os_gfx_rect(gfx, x, times_y - 22, (2 * char_w) + 4, 28);
    }

    /* Days line: fields 4..10, one letter every 2 chars. */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
    const int day_w = (int)solar_os_gfx_text_width(gfx, "0");
    const int days_x = 24;
    const int days_y = 150;
    for (int i = 0; i < 7; i++) {
        const bool set = (irriga.edit_schedule.days & (1U << i)) != 0;
        char letter[2] = {set ? irriga_day_letters[i] : '-', '\0'};
        solar_os_gfx_set_color(gfx, set ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_LIGHT);
        solar_os_gfx_text(gfx, days_x + (i * 2 * day_w), days_y, letter);
    }
    if (irriga.edit_field >= 4 && irriga.edit_field <= 10) {
        const int i = irriga.edit_field - 4;
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_rect(gfx, days_x + (i * 2 * day_w) - 3, days_y - 18, day_w + 6, 24);
    }

    /* Active line: field 11. */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    snprintf(text, sizeof(text), "Active: %s", irriga.edit_schedule.active ? "YES" : "no");
    const int active_y = 196;
    solar_os_gfx_text(gfx, 24, active_y, text);
    if (irriga.edit_field == 11) {
        const int label_w = (int)solar_os_gfx_text_width(gfx, "Active: ");
        solar_os_gfx_rect(gfx, 24 + label_w - 3, active_y - 18, (3 * day_w) + 8, 24);
    }

    irriga_message_line(gfx, active_y + 22);
    irriga_footer(gfx, "ARROWS edit  ENT save  ESC cancel");
}

static void irriga_render_settings(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, IRRIGA_FOOTER_MARGIN, 18, "Settings");
    solar_os_gfx_line(gfx, 0, 26, width - 1, 26);

    const char *labels[3];
    char zones_text[32];
    char mode_text[32];
    snprintf(zones_text, sizeof(zones_text), "Zones: %u", (unsigned)solar_os_irrig_zone_count());
    snprintf(mode_text, sizeof(mode_text), "Mode: %s",
             solar_os_irrig_mode() == SOLAR_OS_IRRIG_MODE_AUTO ? "auto" : "manual");
    labels[0] = zones_text;
    labels[1] = mode_text;
    labels[2] = "Set clock...";

    int y = 56;
    for (uint8_t i = 0; i < 3; i++) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        if (i == irriga.sel_setting) {
            solar_os_gfx_text(gfx, IRRIGA_FOOTER_MARGIN, y, ">");
        }
        solar_os_gfx_text(gfx, 24, y, labels[i]);
        y += 30;
    }

    irriga_message_line(gfx, y + 6);
    irriga_footer(gfx, "L/R change  ENT select  ESC back");
}

static void irriga_render_setclock(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    char text[48];

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, IRRIGA_FOOTER_MARGIN, 18, "Set clock");
    solar_os_gfx_line(gfx, 0, 26, width - 1, 26);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_20);
    const int char_w = (int)solar_os_gfx_text_width(gfx, "0");
    const int x0 = 24;

    /* Line 1: "HH:MM" -- fields 0 (hour), 1 (minute). */
    const int time_y = 100;
    snprintf(text, sizeof(text), "%02u:%02u",
             (unsigned)irriga.clock_edit.hour, (unsigned)irriga.clock_edit.minute);
    solar_os_gfx_text(gfx, x0, time_y, text);

    /* Line 2: "DD/MM/YYYY Www" -- fields 2, 3, 4 (weekday derived). */
    const int date_y = 156;
    const uint8_t weekday = irriga_weekday_of(irriga.clock_edit.year,
                                              irriga.clock_edit.month,
                                              irriga.clock_edit.day);
    snprintf(text, sizeof(text), "%02u/%02u/%04u %s",
             (unsigned)irriga.clock_edit.day,
             (unsigned)irriga.clock_edit.month,
             (unsigned)irriga.clock_edit.year,
             irriga_weekday_names[weekday]);
    solar_os_gfx_text(gfx, x0, date_y, text);

    static const struct {
        int y;
        int char_index;
        int chars;
    } fields[IRRIGA_CLOCK_FIELDS] = {
        {100, 0, 2},  /* hour */
        {100, 3, 2},  /* minute */
        {156, 0, 2},  /* day */
        {156, 3, 2},  /* month */
        {156, 6, 4},  /* year */
    };
    const int x = x0 + (fields[irriga.clock_field].char_index * char_w) - 2;
    solar_os_gfx_rect(gfx,
                      x,
                      fields[irriga.clock_field].y - 22,
                      (fields[irriga.clock_field].chars * char_w) + 4,
                      28);

    irriga_message_line(gfx, 196);
    irriga_footer(gfx, "ARROWS edit  ENT apply  ESC back");
}

static void irriga_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || irriga.suspended) {
        return;
    }

    solar_os_datetime_t now = {0};
    const bool time_ok = solar_os_time_get_datetime(&now) == ESP_OK &&
        solar_os_time_datetime_is_valid(&now) &&
        now.clock_integrity;

    /* The whole app draws white on black, like the original design. */
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    switch (irriga.screen) {
    case IRRIGA_SCREEN_ZONE:
        irriga_render_zone(gfx);
        break;
    case IRRIGA_SCREEN_EDIT:
        irriga_render_edit(gfx);
        break;
    case IRRIGA_SCREEN_SETTINGS:
        irriga_render_settings(gfx);
        break;
    case IRRIGA_SCREEN_SETCLOCK:
        irriga_render_setclock(gfx);
        break;
    case IRRIGA_SCREEN_HOME:
    default:
        irriga_render_home(gfx, &now, time_ok);
        break;
    }

    solar_os_gfx_present(gfx);
}

static void irriga_set_message(const char *message)
{
    strlcpy(irriga.message, message != NULL ? message : "", sizeof(irriga.message));
}

static void irriga_go(irriga_screen_t screen)
{
    irriga.screen = screen;
    irriga_set_message(NULL);
}

static void irriga_adjust_time_field(uint16_t *minute_of_day, bool hour_field, int delta)
{
    int hour = *minute_of_day / 60;
    int minute = *minute_of_day % 60;
    if (hour_field) {
        hour = (hour + delta + 24) % 24;
    } else {
        minute = (minute + delta + 60) % 60;
    }
    *minute_of_day = (uint16_t)((hour * 60) + minute);
}

static void irriga_edit_adjust(int delta)
{
    solar_os_irrig_schedule_t *schedule = &irriga.edit_schedule;

    switch (irriga.edit_field) {
    case 0:
        irriga_adjust_time_field(&schedule->start_minute, true, delta);
        break;
    case 1:
        irriga_adjust_time_field(&schedule->start_minute, false, delta);
        break;
    case 2:
        irriga_adjust_time_field(&schedule->end_minute, true, delta);
        break;
    case 3:
        irriga_adjust_time_field(&schedule->end_minute, false, delta);
        break;
    case 11:
        schedule->active = !schedule->active;
        break;
    default: {
        const uint8_t bit = (uint8_t)(1U << (irriga.edit_field - 4));
        schedule->days ^= bit;
        break;
    }
    }
}

static void irriga_clock_adjust(int delta)
{
    solar_os_datetime_t *dt = &irriga.clock_edit;

    switch (irriga.clock_field) {
    case 0:
        dt->hour = (uint8_t)((dt->hour + delta + 24) % 24);
        break;
    case 1:
        dt->minute = (uint8_t)((dt->minute + delta + 60) % 60);
        break;
    case 2:
        dt->day = (uint8_t)(((dt->day - 1 + delta + 31) % 31) + 1);
        break;
    case 3:
        dt->month = (uint8_t)(((dt->month - 1 + delta + 12) % 12) + 1);
        break;
    case 4: {
        int year = dt->year + delta;
        if (year < 2026) {
            year = 2099;
        } else if (year > 2099) {
            year = 2026;
        }
        dt->year = (uint16_t)year;
        break;
    }
    default:
        break;
    }
}

static void irriga_clock_apply(void)
{
    irriga.clock_edit.second = 0;
    irriga.clock_edit.weekday = irriga_weekday_of(irriga.clock_edit.year,
                                                  irriga.clock_edit.month,
                                                  irriga.clock_edit.day);
    irriga.clock_edit.clock_integrity = true;

    if (solar_os_time_set_datetime(&irriga.clock_edit) == ESP_OK) {
        irriga_go(IRRIGA_SCREEN_SETTINGS);
        irriga_set_message("clock set");
    } else {
        irriga_set_message("invalid date");
    }
}

static void irriga_open_setclock(void)
{
    if (solar_os_time_get_datetime(&irriga.clock_edit) != ESP_OK ||
        !solar_os_time_datetime_is_valid(&irriga.clock_edit)) {
        memset(&irriga.clock_edit, 0, sizeof(irriga.clock_edit));
        irriga.clock_edit.year = 2026;
        irriga.clock_edit.month = 1;
        irriga.clock_edit.day = 1;
        irriga.clock_edit.hour = 12;
    }
    irriga.clock_field = 0;
    irriga_go(IRRIGA_SCREEN_SETCLOCK);
}

static bool irriga_handle_home_key(solar_os_context_t *ctx, uint8_t key)
{
    const uint8_t zones = solar_os_irrig_zone_count();
    const uint8_t rows = irriga_grid_rows();

    switch (key) {
    case SOLAR_OS_KEY_UP:
        irriga.sel_zone = irriga.sel_zone == 0 ? (uint8_t)(zones - 1U) : (uint8_t)(irriga.sel_zone - 1U);
        return true;
    case SOLAR_OS_KEY_DOWN:
        irriga.sel_zone = (uint8_t)((irriga.sel_zone + 1U) % zones);
        return true;
    case SOLAR_OS_KEY_LEFT:
    case SOLAR_OS_KEY_RIGHT:
        /* Jump between the two panel columns. */
        if (key == SOLAR_OS_KEY_RIGHT && irriga.sel_zone + rows < zones) {
            irriga.sel_zone = (uint8_t)(irriga.sel_zone + rows);
        } else if (key == SOLAR_OS_KEY_LEFT && irriga.sel_zone >= rows) {
            irriga.sel_zone = (uint8_t)(irriga.sel_zone - rows);
        }
        return true;
    case '\r':
    case '\n':
        irriga.sel_slot = 0;
        irriga_go(IRRIGA_SCREEN_ZONE);
        return true;
    case ' ': {
        solar_os_irrig_zone_status_t status;
        if (solar_os_irrig_zone_status(irriga.sel_zone, &status) == ESP_OK) {
            (void)solar_os_irrig_set_manual(irriga.sel_zone, !status.manual_on);
            if (solar_os_irrig_mode() != SOLAR_OS_IRRIG_MODE_MANUAL) {
                irriga_set_message("manual switch set; mode is AUTO (press M)");
            } else {
                irriga_set_message(NULL);
            }
        }
        return true;
    }
    case 'm':
    case 'M':
        solar_os_irrig_set_mode(solar_os_irrig_mode() == SOLAR_OS_IRRIG_MODE_AUTO ?
                                    SOLAR_OS_IRRIG_MODE_MANUAL :
                                    SOLAR_OS_IRRIG_MODE_AUTO);
        return true;
    case 's':
    case 'S':
        irriga.sel_setting = 0;
        irriga_go(IRRIGA_SCREEN_SETTINGS);
        return true;
    case SOLAR_OS_KEY_ESCAPE:
        solar_os_context_request_exit(ctx);
        return true;
    default:
        return false;
    }
}

static bool irriga_handle_zone_key(uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_UP:
        irriga.sel_slot = irriga.sel_slot == 0 ?
            (uint8_t)(SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE - 1U) :
            (uint8_t)(irriga.sel_slot - 1U);
        return true;
    case SOLAR_OS_KEY_DOWN:
        irriga.sel_slot = (uint8_t)((irriga.sel_slot + 1U) % SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE);
        return true;
    case '\r':
    case '\n':
        if (solar_os_irrig_get_schedule(irriga.sel_zone, irriga.sel_slot, &irriga.edit_schedule) == ESP_OK) {
            /* A blank slot edits more comfortably with a sane span. */
            if (irriga.edit_schedule.end_minute == 0) {
                irriga.edit_schedule.start_minute = 8U * 60U;
                irriga.edit_schedule.end_minute = (8U * 60U) + 30U;
            }
            irriga.edit_field = 0;
            irriga_go(IRRIGA_SCREEN_EDIT);
        }
        return true;
    case SOLAR_OS_KEY_ESCAPE:
        irriga_go(IRRIGA_SCREEN_HOME);
        return true;
    default:
        return false;
    }
}

static bool irriga_handle_edit_key(uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_LEFT:
        irriga.edit_field = irriga.edit_field == 0 ?
            (uint8_t)(IRRIGA_EDIT_FIELDS - 1U) :
            (uint8_t)(irriga.edit_field - 1U);
        return true;
    case SOLAR_OS_KEY_RIGHT:
        irriga.edit_field = (uint8_t)((irriga.edit_field + 1U) % IRRIGA_EDIT_FIELDS);
        return true;
    case SOLAR_OS_KEY_UP:
        irriga_edit_adjust(1);
        return true;
    case SOLAR_OS_KEY_DOWN:
        irriga_edit_adjust(-1);
        return true;
    case ' ':
        if (irriga.edit_field >= 4) {
            irriga_edit_adjust(1);
        }
        return true;
    case '\r':
    case '\n':
        if (solar_os_irrig_set_schedule(irriga.sel_zone, irriga.sel_slot, &irriga.edit_schedule) == ESP_OK) {
            irriga_go(IRRIGA_SCREEN_ZONE);
        } else {
            irriga_set_message("start must be before end");
        }
        return true;
    case SOLAR_OS_KEY_ESCAPE:
        irriga_go(IRRIGA_SCREEN_ZONE);
        return true;
    default:
        return false;
    }
}

static bool irriga_handle_settings_key(uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_UP:
        irriga.sel_setting = irriga.sel_setting == 0 ? 2U : (uint8_t)(irriga.sel_setting - 1U);
        return true;
    case SOLAR_OS_KEY_DOWN:
        irriga.sel_setting = (uint8_t)((irriga.sel_setting + 1U) % 3U);
        return true;
    case SOLAR_OS_KEY_LEFT:
    case SOLAR_OS_KEY_RIGHT: {
        const int delta = key == SOLAR_OS_KEY_RIGHT ? 1 : -1;
        if (irriga.sel_setting == 0) {
            const int count = (int)solar_os_irrig_zone_count() + delta;
            if (solar_os_irrig_set_zone_count((uint8_t)count) != ESP_OK) {
                irriga_set_message("zones: 1..8");
            } else {
                irriga_set_message(NULL);
                if (irriga.sel_zone >= solar_os_irrig_zone_count()) {
                    irriga.sel_zone = 0;
                }
            }
        } else if (irriga.sel_setting == 1) {
            solar_os_irrig_set_mode(solar_os_irrig_mode() == SOLAR_OS_IRRIG_MODE_AUTO ?
                                        SOLAR_OS_IRRIG_MODE_MANUAL :
                                        SOLAR_OS_IRRIG_MODE_AUTO);
        }
        return true;
    }
    case '\r':
    case '\n':
        if (irriga.sel_setting == 2) {
            irriga_open_setclock();
        }
        return true;
    case SOLAR_OS_KEY_ESCAPE:
        irriga_go(IRRIGA_SCREEN_HOME);
        return true;
    default:
        return false;
    }
}

static bool irriga_handle_setclock_key(uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_LEFT:
        irriga.clock_field = irriga.clock_field == 0 ?
            (uint8_t)(IRRIGA_CLOCK_FIELDS - 1U) :
            (uint8_t)(irriga.clock_field - 1U);
        return true;
    case SOLAR_OS_KEY_RIGHT:
        irriga.clock_field = (uint8_t)((irriga.clock_field + 1U) % IRRIGA_CLOCK_FIELDS);
        return true;
    case SOLAR_OS_KEY_UP:
        irriga_clock_adjust(1);
        return true;
    case SOLAR_OS_KEY_DOWN:
        irriga_clock_adjust(-1);
        return true;
    case '\r':
    case '\n':
        irriga_clock_apply();
        return true;
    case SOLAR_OS_KEY_ESCAPE:
        irriga_go(IRRIGA_SCREEN_SETTINGS);
        return true;
    default:
        return false;
    }
}

static esp_err_t irriga_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&irriga, 0, sizeof(irriga));
    const esp_err_t ret = solar_os_irrig_init();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_context_set_graphics_active(ctx, true);
    irriga_render(ctx);
    return ESP_OK;
}

static void irriga_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void irriga_suspend(solar_os_context_t *ctx)
{
    irriga.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void irriga_resume(solar_os_context_t *ctx)
{
    irriga.suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    irriga_render(ctx);
}

static void irriga_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    strlcpy(buffer, "irriga", buffer_len);
}

static bool irriga_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t key = (uint8_t)event->data.ch;
        if (key == SOLAR_OS_KEY_APP_EXIT) {
            solar_os_context_request_exit(ctx);
            return true;
        }

        bool handled = false;
        switch (irriga.screen) {
        case IRRIGA_SCREEN_ZONE:
            handled = irriga_handle_zone_key(key);
            break;
        case IRRIGA_SCREEN_EDIT:
            handled = irriga_handle_edit_key(key);
            break;
        case IRRIGA_SCREEN_SETTINGS:
            handled = irriga_handle_settings_key(key);
            break;
        case IRRIGA_SCREEN_SETCLOCK:
            handled = irriga_handle_setclock_key(key);
            break;
        case IRRIGA_SCREEN_HOME:
        default:
            handled = irriga_handle_home_key(ctx, key);
            break;
        }

        if (handled) {
            irriga_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        /* Live screens track the clock/engine once a second; editors
         * hold still so the selection doesn't flicker under the user. */
        if (irriga.screen == IRRIGA_SCREEN_HOME || irriga.screen == IRRIGA_SCREEN_ZONE) {
            const uint32_t second = event->data.tick_ms / 1000U;
            if (second != irriga.last_render_s) {
                irriga.last_render_s = second;
                irriga_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        irriga_resume(ctx);
        return true;
    }

    return false;
}

const solar_os_app_t solar_os_irriga_app = {
    .name = "irriga",
    .summary = "irrigation controller UI (engine: job start irrigd)",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = irriga_start,
    .suspend = irriga_suspend,
    .resume = irriga_resume,
    .stop = irriga_stop,
    .event = irriga_event,
    .title = irriga_title,
};
