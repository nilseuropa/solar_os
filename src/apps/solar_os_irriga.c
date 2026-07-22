#include "solar_os_irriga.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "solar_os_config.h"
#include "solar_os_gfx.h"
#include "solar_os_irrig.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_time.h"
#if SOLAR_OS_BOARD_HAS_TOUCH
#include "solar_os_touch.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
#include "solar_os_wifi.h"
#endif

/*
 * Irrigation controller UI -- the graphical front-end for the irrigd
 * schedule engine (which keeps running on its own whether this app is
 * open or not; see solar_os_irrigd_job.c). Key-driven (CardKB, BLE
 * keyboard, remote web page) and, on boards with a touch panel,
 * tap-driven: the home-screen zone buttons/panels open the editor,
 * and the editor is built around big touch buttons.
 *
 * The home screen reproduces the layout of the standalone controller
 * this was ported from: large clock and date top-left, IP and one
 * button per zone top-right, and a two-column grid of zone panels
 * listing every schedule slot. A zone whose relay is on gets a double
 * border and a filled letter button; the slot window matching the
 * current time is drawn inverted. White-on-black, like the original.
 *
 * The zone editor is Casio-style: the zone's four slots on the left
 * with the field being edited inverted, and BACK / SET / NEXT / +1 /
 * -1 buttons around it. NEXT walks fields (start h/m, end h/m, day
 * toggles, active) across all four slots; SET commits the whole zone
 * to the engine.
 *
 * The whole layout is designed on a 320x240 canvas and drawn at an
 * integer scale: 1 on the small panels, 2 on 800x480 (the height is
 * the binding side, 480/240; the extra width is absorbed by the
 * width-adaptive home panels). The 2x fonts are pixel-doubled ProFont
 * (fonts/generate_profont_2x.py), so the app looks identical, just
 * bigger. IRRIGA_PX() scales a design-space pixel value.
 */
#define IRRIGA_PX(v) ((v) * (int)irriga.ui_scale)

#define IRRIGA_FIELDS_PER_SLOT 12U
#define IRRIGA_CLOCK_FIELDS 5U
#define IRRIGA_FOCUS_TIMEOUT_MS 5000U
#define IRRIGA_TOUCH_POLL_MS 30U

typedef enum {
    IRRIGA_SCREEN_HOME,
    IRRIGA_SCREEN_EDITZ,
    IRRIGA_SCREEN_SETTINGS,
    IRRIGA_SCREEN_SETCLOCK,
} irriga_screen_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
} irriga_rect_t;

typedef struct {
    irriga_screen_t screen;
    bool suspended;
    uint8_t ui_scale; /* 1 or 2, decided from the panel size at start */
    uint32_t last_render_s;
    uint8_t sel_zone;
    uint8_t sel_setting;
    solar_os_irrig_schedule_t editz[SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE];
    uint8_t editz_pos; /* slot * IRRIGA_FIELDS_PER_SLOT + field */
    char editz_status[4]; /* "", "OK", "ERR" -- shown on the SET button */
    uint8_t clock_field;
    solar_os_datetime_t clock_edit;
    uint32_t focus_last_key_ms; /* 0 = focus frame hidden */
#if SOLAR_OS_BOARD_HAS_TOUCH
    bool touch_down;
    uint32_t touch_last_poll_ms;
#endif
    char message[48];
} irriga_state_t;

static const char irriga_day_letters[7] = {'L', 'M', 'M', 'J', 'V', 'S', 'D'};
static const char *irriga_weekday_names[7] = {"Dum", "Lun", "Mar", "Mie", "Joi", "Vin", "Sam"};

/* Char position of each editable field inside the rendered slot row
 * "16:00-16:30 LMMJVSD A": start h/m, end h/m, 7 days, active flag. */
static const struct {
    uint8_t index;
    uint8_t chars;
} irriga_field_spans[IRRIGA_FIELDS_PER_SLOT] = {
    {0, 2}, {3, 2}, {6, 2}, {9, 2},
    {12, 1}, {13, 1}, {14, 1}, {15, 1}, {16, 1}, {17, 1}, {18, 1},
    {20, 1},
};

static irriga_state_t irriga;

/* Design-role fonts, picked per UI scale. */
static solar_os_gfx_font_t irriga_font_rows(void)
{
    return irriga.ui_scale == 2 ? SOLAR_OS_GFX_FONT_PROFONT_24 : SOLAR_OS_GFX_FONT_PROFONT_12;
}

static solar_os_gfx_font_t irriga_font_text(void)
{
    return irriga.ui_scale == 2 ? SOLAR_OS_GFX_FONT_PROFONT_30 : SOLAR_OS_GFX_FONT_PROFONT_15;
}

static solar_os_gfx_font_t irriga_font_title(void)
{
    return irriga.ui_scale == 2 ? SOLAR_OS_GFX_FONT_PROFONT_34 : SOLAR_OS_GFX_FONT_PROFONT_17;
}

static solar_os_gfx_font_t irriga_font_button(void)
{
    return irriga.ui_scale == 2 ? SOLAR_OS_GFX_FONT_PROFONT_44 : SOLAR_OS_GFX_FONT_PROFONT_22;
}

static solar_os_gfx_font_t irriga_font_clock(void)
{
    return irriga.ui_scale == 2 ? SOLAR_OS_GFX_FONT_PROFONT_58 : SOLAR_OS_GFX_FONT_PROFONT_29;
}

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

static uint32_t irriga_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/*
 * The home-screen focus frame is transient: it shows up on the first
 * key press and fades out 5 seconds after the last one, so the idle
 * status screen stays clean.
 */
static bool irriga_focus_visible(void)
{
    return irriga.focus_last_key_ms != 0 &&
        (uint32_t)(irriga_now_ms() - irriga.focus_last_key_ms) < IRRIGA_FOCUS_TIMEOUT_MS;
}

static void irriga_focus_touch(void)
{
    irriga.focus_last_key_ms = irriga_now_ms();
    if (irriga.focus_last_key_ms == 0) {
        irriga.focus_last_key_ms = 1;
    }
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

static void irriga_format_slot_row(const solar_os_irrig_schedule_t *schedule,
                                   char *out,
                                   size_t out_len)
{
    char days[8];
    irriga_format_days(schedule->days, days);
    snprintf(out, out_len, "%02u:%02u-%02u:%02u %s %c",
             (unsigned)(schedule->start_minute / 60U),
             (unsigned)(schedule->start_minute % 60U),
             (unsigned)(schedule->end_minute / 60U),
             (unsigned)(schedule->end_minute % 60U),
             days,
             schedule->active ? 'A' : '-');
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

static bool irriga_rect_hit(const irriga_rect_t *rect, int x, int y)
{
    return x >= rect->x && x < rect->x + rect->w &&
        y >= rect->y && y < rect->y + rect->h;
}

/* One design-space pixel: an s-by-s block on screen. */
static void irriga_block(solar_os_gfx_t *gfx, int x, int y)
{
    const int s = (int)irriga.ui_scale;
    if (s == 1) {
        solar_os_gfx_pixel(gfx, x, y);
    } else {
        solar_os_gfx_fill_rect(gfx, x, y, s, s);
    }
}

/* Rectangle with the corners knocked off -- close enough to the
 * rounded panels of the original at one bit per pixel. Geometry is in
 * screen pixels; the chamfer pattern scales with the UI. */
static void irriga_round_rect(solar_os_gfx_t *gfx, int x, int y, int w, int h)
{
    const int s = (int)irriga.ui_scale;
    const int c = 3 * s;

    solar_os_gfx_line(gfx, x + c, y, x + w - 1 - c, y);
    solar_os_gfx_line(gfx, x + c, y + h - 1, x + w - 1 - c, y + h - 1);
    solar_os_gfx_line(gfx, x, y + c, x, y + h - 1 - c);
    solar_os_gfx_line(gfx, x + w - 1, y + c, x + w - 1, y + h - 1 - c);

    irriga_block(gfx, x + s, y + s);
    irriga_block(gfx, x + 2 * s, y + s);
    irriga_block(gfx, x + s, y + 2 * s);
    irriga_block(gfx, x + w - 2 * s, y + s);
    irriga_block(gfx, x + w - 3 * s, y + s);
    irriga_block(gfx, x + w - 2 * s, y + 2 * s);
    irriga_block(gfx, x + s, y + h - 2 * s);
    irriga_block(gfx, x + 2 * s, y + h - 2 * s);
    irriga_block(gfx, x + s, y + h - 3 * s);
    irriga_block(gfx, x + w - 2 * s, y + h - 2 * s);
    irriga_block(gfx, x + w - 3 * s, y + h - 2 * s);
    irriga_block(gfx, x + w - 2 * s, y + h - 3 * s);
}

static void irriga_touch_button(solar_os_gfx_t *gfx,
                                const irriga_rect_t *rect,
                                bool big,
                                const char *label)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    irriga_round_rect(gfx, rect->x, rect->y, rect->w, rect->h);
    solar_os_gfx_set_font(gfx, big ? irriga_font_button() : irriga_font_text());
    const int label_w = (int)solar_os_gfx_text_width(gfx, label);
    solar_os_gfx_text(gfx,
                      rect->x + (rect->w - label_w) / 2,
                      rect->y + (rect->h / 2) + IRRIGA_PX(big ? 8 : 6),
                      label);
}

static void irriga_draw_clock(solar_os_gfx_t *gfx,
                              const solar_os_datetime_t *now,
                              bool time_ok)
{
    char text[12];
    if (time_ok) {
        snprintf(text, sizeof(text), "%02u:%02u:%02u",
                 (unsigned)now->hour, (unsigned)now->minute, (unsigned)now->second);
    } else {
        strlcpy(text, "--:--:--", sizeof(text));
    }
    solar_os_gfx_set_font(gfx, irriga_font_clock());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, IRRIGA_PX(4), IRRIGA_PX(30), text);
}

static void irriga_footer(solar_os_gfx_t *gfx, const char *text)
{
    solar_os_gfx_set_font(gfx, irriga_font_rows());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    solar_os_gfx_text(gfx,
                      IRRIGA_PX(4),
                      (int)solar_os_gfx_height(gfx) - IRRIGA_PX(4),
                      text);
}

static void irriga_message_line(solar_os_gfx_t *gfx, int baseline)
{
    if (irriga.message[0] == '\0') {
        return;
    }
    solar_os_gfx_set_font(gfx, irriga_font_text());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, IRRIGA_PX(4), baseline, irriga.message);
}

/* --- home screen layout, shared by rendering and tap hit-testing --- */

static uint8_t irriga_grid_rows(void)
{
    const uint8_t zones = solar_os_irrig_zone_count();
    uint8_t rows = (uint8_t)((zones + 1U) / 2U);
    return rows == 0 ? 1U : rows;
}

static void irriga_home_button_rect(int width, uint8_t zone, irriga_rect_t *out)
{
    const uint8_t zones = solar_os_irrig_zone_count();
    const int bsize = IRRIGA_PX(zones > 6 ? 16 : 22);
    const int bgap = IRRIGA_PX(zones > 6 ? 4 : 6);
    const int buttons_w = (zones * bsize) + ((zones - 1) * bgap);
    out->x = width - IRRIGA_PX(4) - buttons_w + zone * (bsize + bgap);
    out->y = IRRIGA_PX(20);
    out->w = bsize;
    out->h = bsize;
}

static void irriga_home_panel_rect(int width, int height, uint8_t zone, irriga_rect_t *out)
{
    const uint8_t rows = irriga_grid_rows();
    const int margin = IRRIGA_PX(2);
    const int top = IRRIGA_PX(52);
    const int pw = (width - 3 * margin) / 2;
    int ph = ((height - top - margin) / rows) - margin;
    if (ph > IRRIGA_PX(92)) {
        ph = IRRIGA_PX(92);
    }
    out->x = margin + (zone / rows) * (pw + margin);
    out->y = top + (zone % rows) * (ph + margin);
    out->w = pw;
    out->h = ph;
}

/* --- home screen rendering --- */

static void irriga_render_home_header(solar_os_gfx_t *gfx,
                                      const solar_os_datetime_t *now,
                                      bool time_ok)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const int date_baseline = IRRIGA_PX(46);
    char text[40];

    irriga_draw_clock(gfx, now, time_ok);

    solar_os_gfx_set_font(gfx, irriga_font_text());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    if (time_ok) {
        snprintf(text, sizeof(text), "%02u/%02u/%04u %s",
                 (unsigned)now->day, (unsigned)now->month, (unsigned)now->year,
                 irriga_weekday_names[now->weekday % 7]);
    } else {
        strlcpy(text, "ceas nesetat", sizeof(text));
    }
    solar_os_gfx_text(gfx, IRRIGA_PX(4), date_baseline, text);

    /* Status cues share the date line so the grid keeps its space. */
    if (!irriga_engine_running()) {
        solar_os_gfx_text(gfx, IRRIGA_PX(140), date_baseline, "irrigd OFF");
    } else if (solar_os_irrig_mode() == SOLAR_OS_IRRIG_MODE_MANUAL) {
        solar_os_gfx_text(gfx, IRRIGA_PX(140), date_baseline, "MANUAL");
    }

    /* IP top right. */
#if SOLAR_OS_PACKAGE_SERVICE_WIFI
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (wifi.state == SOLAR_OS_WIFI_STATE_CONNECTED && wifi.ip[0] != '\0') {
        snprintf(text, sizeof(text), "IP:%s", wifi.ip);
        solar_os_gfx_set_font(gfx, irriga_font_text());
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        const int ip_w = (int)solar_os_gfx_text_width(gfx, text);
        solar_os_gfx_text(gfx, width - IRRIGA_PX(4) - ip_w, IRRIGA_PX(14), text);
    }
#endif

    /* One lettered button per zone, right-aligned under the IP.
     * Filled = that zone's relay is on. Tapping one opens its editor. */
    const uint8_t zones = solar_os_irrig_zone_count();
    const int s = (int)irriga.ui_scale;
    solar_os_gfx_set_font(gfx, zones > 6 ? irriga_font_rows() : irriga_font_text());
    for (uint8_t zone = 0; zone < zones; zone++) {
        solar_os_irrig_zone_status_t status = {0};
        (void)solar_os_irrig_zone_status(zone, &status);

        irriga_rect_t rect;
        irriga_home_button_rect(width, zone, &rect);

        char letter[2] = {(char)('A' + zone), '\0'};
        const int letter_w = (int)solar_os_gfx_text_width(gfx, letter);
        const int lx = rect.x + (rect.w - letter_w) / 2;
        const int ly = rect.y + (rect.h / 2) + IRRIGA_PX(5);

        if (status.output_on) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, rect.x, rect.y, rect.w, rect.h);
            /* Knock the corners off the fill so the on button keeps
             * the same rounded shape as the off outline. */
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            irriga_block(gfx, rect.x, rect.y);
            irriga_block(gfx, rect.x + s, rect.y);
            irriga_block(gfx, rect.x, rect.y + s);
            irriga_block(gfx, rect.x + rect.w - s, rect.y);
            irriga_block(gfx, rect.x + rect.w - 2 * s, rect.y);
            irriga_block(gfx, rect.x + rect.w - s, rect.y + s);
            irriga_block(gfx, rect.x, rect.y + rect.h - s);
            irriga_block(gfx, rect.x + s, rect.y + rect.h - s);
            irriga_block(gfx, rect.x, rect.y + rect.h - 2 * s);
            irriga_block(gfx, rect.x + rect.w - s, rect.y + rect.h - s);
            irriga_block(gfx, rect.x + rect.w - 2 * s, rect.y + rect.h - s);
            irriga_block(gfx, rect.x + rect.w - s, rect.y + rect.h - 2 * s);
            solar_os_gfx_text(gfx, lx, ly, letter);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            irriga_round_rect(gfx, rect.x, rect.y, rect.w, rect.h);
            solar_os_gfx_text(gfx, lx, ly, letter);
        }
    }
}

static void irriga_render_zone_panel(solar_os_gfx_t *gfx,
                                     uint8_t zone,
                                     const irriga_rect_t *rect,
                                     const solar_os_datetime_t *now,
                                     bool time_ok)
{
    char text[32];
    const int px = rect->x;
    const int py = rect->y;
    const int pw = rect->w;
    const int ph = rect->h;
    const int row_step = IRRIGA_PX(17);

    solar_os_irrig_zone_status_t status = {0};
    (void)solar_os_irrig_zone_status(zone, &status);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    irriga_round_rect(gfx, px, py, pw, ph);
    if (status.output_on) {
        const int inset = IRRIGA_PX(1);
        irriga_round_rect(gfx, px + inset, py + inset, pw - 2 * inset, ph - 2 * inset);
    }

    /* The keyboard cursor is a dashed inner frame -- deliberately
     * different from the solid double border, which means "relay on".
     * Only shown while keys are being used (see irriga_focus_visible). */
    if (zone == irriga.sel_zone && irriga_focus_visible()) {
        const int inset = IRRIGA_PX(2);
        solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
        irriga_round_rect(gfx, px + inset, py + inset, pw - 2 * inset, ph - 2 * inset);
        solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
    }

    snprintf(text, sizeof(text), "Zone %c", 'A' + zone);
    solar_os_gfx_set_font(gfx, irriga_font_text());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, px + IRRIGA_PX(7), py + IRRIGA_PX(14), text);

    solar_os_gfx_set_font(gfx, irriga_font_rows());
    int baseline = py + IRRIGA_PX(14) + row_step;
    for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
        if (baseline > py + ph - IRRIGA_PX(4)) {
            break;
        }

        solar_os_irrig_schedule_t schedule;
        if (solar_os_irrig_get_schedule(zone, slot, &schedule) != ESP_OK) {
            continue;
        }

        irriga_format_slot_row(&schedule, text, sizeof(text));

        if (irriga_slot_active_now(&schedule, now, time_ok)) {
            const int row_w = (int)solar_os_gfx_text_width(gfx, text);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, px + IRRIGA_PX(3),
                                   baseline - IRRIGA_PX(10),
                                   row_w + IRRIGA_PX(6), IRRIGA_PX(13));
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        } else {
            solar_os_gfx_set_color(gfx, schedule.active ?
                                            SOLAR_OS_GFX_COLOR_WHITE :
                                            SOLAR_OS_GFX_COLOR_LIGHT);
        }
        solar_os_gfx_text(gfx, px + IRRIGA_PX(6), baseline, text);
        baseline += row_step;
    }
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
    for (uint8_t zone = 0; zone < zones; zone++) {
        irriga_rect_t rect;
        irriga_home_panel_rect(width, height, zone, &rect);
        irriga_render_zone_panel(gfx, zone, &rect, now, time_ok);
    }

    irriga_message_line(gfx, height - IRRIGA_PX(4));
}

/* --- Casio-style zone editor --- */

static void irriga_editz_layout(const solar_os_gfx_t *gfx,
                                irriga_rect_t *back,
                                irriga_rect_t *list,
                                irriga_rect_t *set,
                                irriga_rect_t *next,
                                irriga_rect_t *plus,
                                irriga_rect_t *minus)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const int m = IRRIGA_PX(6);
    const int list_w = IRRIGA_PX(164);
    const int bottom_h = IRRIGA_PX(68);
    const int mid_y = IRRIGA_PX(48);
    const int bottom_y = height - m - bottom_h;
    const int mid_h = bottom_y - m - mid_y;
    const int right_x = m + list_w + m;
    const int right_w = width - right_x - m;

    *back = (irriga_rect_t){m, m, IRRIGA_PX(86), IRRIGA_PX(34)};
    *list = (irriga_rect_t){m, mid_y, list_w, mid_h};
    *set = (irriga_rect_t){right_x, mid_y, right_w, mid_h};
    *next = (irriga_rect_t){m, bottom_y, list_w, bottom_h};
    const int half = (right_w - m) / 2;
    *plus = (irriga_rect_t){right_x, bottom_y, half, bottom_h};
    *minus = (irriga_rect_t){right_x + half + m, bottom_y, half, bottom_h};
}

static int irriga_editz_row_baseline(const irriga_rect_t *list, uint8_t slot)
{
    return list->y + IRRIGA_PX(16) + IRRIGA_PX(17) * ((int)slot + 1);
}

static void irriga_render_editz(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    char text[32];

    irriga_rect_t back;
    irriga_rect_t list;
    irriga_rect_t set;
    irriga_rect_t next;
    irriga_rect_t plus;
    irriga_rect_t minus;
    irriga_editz_layout(gfx, &back, &list, &set, &next, &plus, &minus);

    irriga_touch_button(gfx, &back, false, "< BACK");

    snprintf(text, sizeof(text), "Edit Zone %c", 'A' + irriga.sel_zone);
    solar_os_gfx_set_font(gfx, irriga_font_title());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    const int title_w = (int)solar_os_gfx_text_width(gfx, text);
    solar_os_gfx_text(gfx, width - IRRIGA_PX(10) - title_w, IRRIGA_PX(29), text);

    /* Slot list with the field under edit inverted. */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    irriga_round_rect(gfx, list.x, list.y, list.w, list.h);
    solar_os_gfx_set_font(gfx, irriga_font_text());
    snprintf(text, sizeof(text), "Zone %c", 'A' + irriga.sel_zone);
    solar_os_gfx_text(gfx, list.x + IRRIGA_PX(8), list.y + IRRIGA_PX(16), text);

    solar_os_gfx_set_font(gfx, irriga_font_rows());
    const uint8_t cur_slot = (uint8_t)(irriga.editz_pos / IRRIGA_FIELDS_PER_SLOT);
    const uint8_t cur_field = (uint8_t)(irriga.editz_pos % IRRIGA_FIELDS_PER_SLOT);

    for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
        const int row_x = list.x + IRRIGA_PX(8);
        const int baseline = irriga_editz_row_baseline(&list, slot);

        irriga_format_slot_row(&irriga.editz[slot], text, sizeof(text));
        solar_os_gfx_set_color(gfx, irriga.editz[slot].active ?
                                        SOLAR_OS_GFX_COLOR_WHITE :
                                        SOLAR_OS_GFX_COLOR_LIGHT);
        solar_os_gfx_text(gfx, row_x, baseline, text);

        if (slot == cur_slot) {
            /* Invert the edited field, Casio style. The highlight's
             * x is measured from the actual rendered prefix, not
             * assumed from a per-character cell width -- so it lines
             * up with wherever DrawUTF8 really put that glyph even if
             * the font isn't perfectly uniform-width. */
            const uint8_t field_index = irriga_field_spans[cur_field].index;
            const uint8_t field_chars = irriga_field_spans[cur_field].chars;
            char prefix[24];
            memcpy(prefix, text, field_index);
            prefix[field_index] = '\0';
            int span_x = row_x + (int)solar_os_gfx_text_width(gfx, prefix);

            /*
             * Hardware-measured correction (2026-07-20, LCD-5),
             * tuned iteratively against on-device measurement across
             * all 12 fields of a slot row: the pixel-doubled 2x row
             * font (u8g2_font_solar_os_profont_24_mf) under-measures
             * a non-empty, space-free prefix (the three time fields
             * after start-hour) by 3 physical px; a prefix with one
             * space (the 7 day fields) by 1px; a prefix with both
             * spaces (the active field) is exact. Root cause not
             * pinned down (source BDF metrics are uniformly
             * monospace; something downstream of the doubling/bdfconv
             * step isn't), so this compensates the measured behavior
             * directly rather than a guessed mechanism. Only applies
             * to the doubled font -- the vendored 1x ProFont hasn't
             * shown this and shouldn't be touched blindly.
             */
            if (irriga.ui_scale == 2 && field_index != 0) {
                int spaces = 0;
                for (uint8_t i = 0; i < field_index; i++) {
                    if (prefix[i] == ' ') {
                        spaces++;
                    }
                }
                span_x += spaces == 0 ? 3 : (2 - spaces);
            }

            char span_text[4];
            memcpy(span_text, &text[field_index], field_chars);
            span_text[field_chars] = '\0';
            const int span_w = (int)solar_os_gfx_text_width(gfx, span_text);

            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, span_x - IRRIGA_PX(1),
                                   baseline - IRRIGA_PX(10),
                                   span_w + IRRIGA_PX(2), IRRIGA_PX(13));
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_text(gfx, span_x, baseline, span_text);
        }
    }

    const char *set_label = irriga.editz_status[0] != '\0' ? irriga.editz_status : "SET";
    irriga_touch_button(gfx, &set, true, set_label);
    irriga_touch_button(gfx, &next, true, "NEXT");
    irriga_touch_button(gfx, &plus, true, "+1");
    irriga_touch_button(gfx, &minus, true, "-1");
}

static void irriga_open_editz(uint8_t zone)
{
    irriga.sel_zone = zone;
    for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
        if (solar_os_irrig_get_schedule(zone, slot, &irriga.editz[slot]) != ESP_OK) {
            memset(&irriga.editz[slot], 0, sizeof(irriga.editz[slot]));
        }
    }
    irriga.editz_pos = 0;
    irriga.editz_status[0] = '\0';
    irriga.screen = IRRIGA_SCREEN_EDITZ;
    irriga_focus_touch();
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

static void irriga_editz_adjust(int delta)
{
    solar_os_irrig_schedule_t *schedule =
        &irriga.editz[irriga.editz_pos / IRRIGA_FIELDS_PER_SLOT];
    const uint8_t field = (uint8_t)(irriga.editz_pos % IRRIGA_FIELDS_PER_SLOT);

    switch (field) {
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
        const uint8_t bit = (uint8_t)(1U << (field - 4));
        schedule->days ^= bit;
        break;
    }
    }
    irriga.editz_status[0] = '\0';
}

static void irriga_editz_move(int delta)
{
    const int total = (int)(SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE * IRRIGA_FIELDS_PER_SLOT);
    int pos = (int)irriga.editz_pos + delta;
    pos = ((pos % total) + total) % total;
    irriga.editz_pos = (uint8_t)pos;
    irriga.editz_status[0] = '\0';
}

static void irriga_editz_commit(void)
{
    unsigned rejected = 0;

    for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
        solar_os_irrig_schedule_t current;
        if (solar_os_irrig_get_schedule(irriga.sel_zone, slot, &current) != ESP_OK) {
            rejected++;
            continue;
        }
        /* Untouched slots are skipped: blank ones (start == end == 0)
         * would fail the engine's start < end validation. */
        if (memcmp(&irriga.editz[slot], &current, sizeof(current)) == 0) {
            continue;
        }
        if (solar_os_irrig_set_schedule(irriga.sel_zone, slot, &irriga.editz[slot]) != ESP_OK) {
            rejected++;
        }
    }

    strlcpy(irriga.editz_status, rejected == 0 ? "OK" : "ERR",
            sizeof(irriga.editz_status));
}

/* --- settings + manual clock --- */

static void irriga_render_settings(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);

    solar_os_gfx_set_font(gfx, irriga_font_title());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, IRRIGA_PX(4), IRRIGA_PX(18), "Settings");
    solar_os_gfx_line(gfx, 0, IRRIGA_PX(26), width - 1, IRRIGA_PX(26));

    const char *labels[3];
    char zones_text[32];
    char mode_text[32];
    snprintf(zones_text, sizeof(zones_text), "Zones: %u", (unsigned)solar_os_irrig_zone_count());
    snprintf(mode_text, sizeof(mode_text), "Mode: %s",
             solar_os_irrig_mode() == SOLAR_OS_IRRIG_MODE_AUTO ? "auto" : "manual");
    labels[0] = zones_text;
    labels[1] = mode_text;
    labels[2] = "Set clock...";

    int y = IRRIGA_PX(56);
    for (uint8_t i = 0; i < 3; i++) {
        solar_os_gfx_set_font(gfx, irriga_font_title());
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        if (i == irriga.sel_setting) {
            solar_os_gfx_text(gfx, IRRIGA_PX(4), y, ">");
        }
        solar_os_gfx_text(gfx, IRRIGA_PX(24), y, labels[i]);
        y += IRRIGA_PX(30);
    }

    irriga_message_line(gfx, y + IRRIGA_PX(6));
    irriga_footer(gfx, "L/R change  ENT select  ESC back");
}

static void irriga_render_setclock(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    char time_text[16];
    char date_text[32];

    solar_os_gfx_set_font(gfx, irriga_font_title());
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text(gfx, IRRIGA_PX(4), IRRIGA_PX(18), "Set clock");
    solar_os_gfx_line(gfx, 0, IRRIGA_PX(26), width - 1, IRRIGA_PX(26));

    solar_os_gfx_set_font(gfx, irriga_font_button());
    const int x0 = IRRIGA_PX(24);

    /* Line 1: "HH:MM" -- fields 0 (hour), 1 (minute). */
    const int time_y = IRRIGA_PX(100);
    snprintf(time_text, sizeof(time_text), "%02u:%02u",
             (unsigned)irriga.clock_edit.hour, (unsigned)irriga.clock_edit.minute);
    solar_os_gfx_text(gfx, x0, time_y, time_text);

    /* Line 2: "DD/MM/YYYY Www" -- fields 2, 3, 4 (weekday derived). */
    const int date_y = IRRIGA_PX(156);
    const uint8_t weekday = irriga_weekday_of(irriga.clock_edit.year,
                                              irriga.clock_edit.month,
                                              irriga.clock_edit.day);
    snprintf(date_text, sizeof(date_text), "%02u/%02u/%04u %s",
             (unsigned)irriga.clock_edit.day,
             (unsigned)irriga.clock_edit.month,
             (unsigned)irriga.clock_edit.year,
             irriga_weekday_names[weekday]);
    solar_os_gfx_text(gfx, x0, date_y, date_text);

    static const struct {
        int y;
        int char_index;
        int chars;
        bool on_time_line;
    } fields[IRRIGA_CLOCK_FIELDS] = {
        {100, 0, 2, true},   /* hour */
        {100, 3, 2, true},   /* minute */
        {156, 0, 2, false},  /* day */
        {156, 3, 2, false},  /* month */
        {156, 6, 4, false},  /* year */
    };
    const int char_index = fields[irriga.clock_field].char_index;
    const int chars = fields[irriga.clock_field].chars;
    const char *line = fields[irriga.clock_field].on_time_line ? time_text : date_text;

    /* Measure the actual rendered prefix/field width instead of
     * assuming a uniform per-character cell, same reasoning as the
     * zone editor's field highlight. */
    char prefix[16];
    memcpy(prefix, line, (size_t)char_index);
    prefix[char_index] = '\0';
    char field_text[8];
    memcpy(field_text, &line[char_index], (size_t)chars);
    field_text[chars] = '\0';

    const int x = x0 + (int)solar_os_gfx_text_width(gfx, prefix) - IRRIGA_PX(2);
    solar_os_gfx_rect(gfx,
                      x,
                      IRRIGA_PX(fields[irriga.clock_field].y - 22),
                      (int)solar_os_gfx_text_width(gfx, field_text) + IRRIGA_PX(4),
                      IRRIGA_PX(28));

    irriga_message_line(gfx, IRRIGA_PX(196));
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
    case IRRIGA_SCREEN_EDITZ:
        irriga_render_editz(gfx);
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

/* --- keyboard handling --- */

static bool irriga_handle_home_key(solar_os_context_t *ctx, uint8_t key)
{
    const uint8_t zones = solar_os_irrig_zone_count();
    const uint8_t rows = irriga_grid_rows();
    const bool focus_was_visible = irriga_focus_visible();

    irriga_focus_touch();

    switch (key) {
    case SOLAR_OS_KEY_UP:
    case SOLAR_OS_KEY_DOWN:
    case SOLAR_OS_KEY_LEFT:
    case SOLAR_OS_KEY_RIGHT:
        /* The first key press only summons the focus frame; movement
         * starts once it is visible, so nothing shifts by accident. */
        if (!focus_was_visible) {
            return true;
        }
        if (key == SOLAR_OS_KEY_UP) {
            irriga.sel_zone = irriga.sel_zone == 0 ? (uint8_t)(zones - 1U) : (uint8_t)(irriga.sel_zone - 1U);
        } else if (key == SOLAR_OS_KEY_DOWN) {
            irriga.sel_zone = (uint8_t)((irriga.sel_zone + 1U) % zones);
        } else if (key == SOLAR_OS_KEY_RIGHT && irriga.sel_zone + rows < zones) {
            /* Jump between the two panel columns. */
            irriga.sel_zone = (uint8_t)(irriga.sel_zone + rows);
        } else if (key == SOLAR_OS_KEY_LEFT && irriga.sel_zone >= rows) {
            irriga.sel_zone = (uint8_t)(irriga.sel_zone - rows);
        }
        return true;
    case '\r':
    case '\n':
        irriga_open_editz(irriga.sel_zone);
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

static bool irriga_handle_editz_key(uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_RIGHT:
        irriga_editz_move(1);
        return true;
    case SOLAR_OS_KEY_LEFT:
        irriga_editz_move(-1);
        return true;
    case SOLAR_OS_KEY_UP:
        irriga_editz_adjust(1);
        return true;
    case SOLAR_OS_KEY_DOWN:
        irriga_editz_adjust(-1);
        return true;
    case '\r':
    case '\n':
        irriga_editz_commit();
        return true;
    case SOLAR_OS_KEY_ESCAPE:
        irriga_go(IRRIGA_SCREEN_HOME);
        irriga_focus_touch();
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
        irriga_focus_touch();
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

/* --- touch handling --- */

#if SOLAR_OS_BOARD_HAS_TOUCH
static bool irriga_handle_tap_home(solar_os_gfx_t *gfx, int x, int y)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const uint8_t zones = solar_os_irrig_zone_count();
    const int grow = IRRIGA_PX(3);

    for (uint8_t zone = 0; zone < zones; zone++) {
        irriga_rect_t rect;
        irriga_home_button_rect(width, zone, &rect);
        /* Grow the small letter buttons a bit -- fingers are blunt. */
        rect.x -= grow;
        rect.y -= grow;
        rect.w += 2 * grow;
        rect.h += 2 * grow;
        if (irriga_rect_hit(&rect, x, y)) {
            irriga_open_editz(zone);
            return true;
        }
    }
    for (uint8_t zone = 0; zone < zones; zone++) {
        irriga_rect_t rect;
        irriga_home_panel_rect(width, height, zone, &rect);
        if (irriga_rect_hit(&rect, x, y)) {
            irriga_open_editz(zone);
            return true;
        }
    }
    return false;
}

static bool irriga_handle_tap_editz(solar_os_gfx_t *gfx, int x, int y)
{
    irriga_rect_t back;
    irriga_rect_t list;
    irriga_rect_t set;
    irriga_rect_t next;
    irriga_rect_t plus;
    irriga_rect_t minus;
    irriga_editz_layout(gfx, &back, &list, &set, &next, &plus, &minus);

    if (irriga_rect_hit(&back, x, y)) {
        irriga_go(IRRIGA_SCREEN_HOME);
        irriga_focus_touch();
        return true;
    }
    if (irriga_rect_hit(&set, x, y)) {
        irriga_editz_commit();
        return true;
    }
    if (irriga_rect_hit(&next, x, y)) {
        irriga_editz_move(1);
        return true;
    }
    if (irriga_rect_hit(&plus, x, y)) {
        irriga_editz_adjust(1);
        return true;
    }
    if (irriga_rect_hit(&minus, x, y)) {
        irriga_editz_adjust(-1);
        return true;
    }
    if (irriga_rect_hit(&list, x, y)) {
        /* Tap a slot row to jump the cursor to it. */
        for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
            const int baseline = irriga_editz_row_baseline(&list, slot);
            if (y >= baseline - IRRIGA_PX(13) && y <= baseline + IRRIGA_PX(4)) {
                irriga.editz_pos = (uint8_t)(slot * IRRIGA_FIELDS_PER_SLOT);
                irriga.editz_status[0] = '\0';
                return true;
            }
        }
    }
    return false;
}

static bool irriga_handle_tap(solar_os_context_t *ctx, int x, int y)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return false;
    }

    switch (irriga.screen) {
    case IRRIGA_SCREEN_EDITZ:
        return irriga_handle_tap_editz(gfx, x, y);
    case IRRIGA_SCREEN_HOME:
        return irriga_handle_tap_home(gfx, x, y);
    default:
        return false;
    }
}

static void irriga_poll_touch(solar_os_context_t *ctx, uint32_t tick_ms)
{
    if (!solar_os_touch_available()) {
        return;
    }
    if ((uint32_t)(tick_ms - irriga.touch_last_poll_ms) < IRRIGA_TOUCH_POLL_MS) {
        return;
    }
    irriga.touch_last_poll_ms = tick_ms;

    bool pressed = false;
    uint16_t x = 0;
    uint16_t y = 0;
    if (solar_os_touch_read(&pressed, &x, &y) != ESP_OK) {
        return;
    }

    if (pressed && !irriga.touch_down) {
        irriga.touch_down = true;
        if (irriga_handle_tap(ctx, (int)x, (int)y)) {
            irriga_render(ctx);
        }
    } else if (!pressed) {
        irriga.touch_down = false;
    }
}
#endif /* SOLAR_OS_BOARD_HAS_TOUCH */

/* --- app plumbing --- */

static esp_err_t irriga_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&irriga, 0, sizeof(irriga));
    /* The layout is designed for 320x240; large panels draw it at 2x
     * (480 tall fits exactly twice the design height). */
    irriga.ui_scale = solar_os_gfx_height(gfx) >= 480 ? 2 : 1;

    const esp_err_t ret = solar_os_irrig_init();
    if (ret != ESP_OK) {
        return ret;
    }

#if SOLAR_OS_BOARD_HAS_TOUCH
    (void)solar_os_touch_init(); /* app works fine without it */
#endif

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
        case IRRIGA_SCREEN_EDITZ:
            handled = irriga_handle_editz_key(key);
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
        if (irriga.suspended) {
            return true;
        }
#if SOLAR_OS_BOARD_HAS_TOUCH
        irriga_poll_touch(ctx, event->data.tick_ms);
#endif
        /* The home screen tracks the clock/engine once a second; the
         * editors hold still so nothing flickers under the user. */
        if (irriga.screen == IRRIGA_SCREEN_HOME) {
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
