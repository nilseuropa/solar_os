#include "solar_os_clock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif
#include "solar_os_ble_keyboard.h"
#include "solar_os_gfx.h"
#include "solar_os_log.h"
#include "solar_os_task.h"
#include "solar_os_time.h"

#define CLOCK_ALARM_SOUND_TASK_STACK 4096
#define CLOCK_ALARM_SOUND_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define CLOCK_DISPLAY_MAX_MINUTES 99U

typedef enum {
    CLOCK_MODE_TIME,
    CLOCK_MODE_ALARM,
    CLOCK_MODE_STOPWATCH,
} clock_mode_t;

typedef struct {
    uint32_t last_second;
    clock_mode_t mode;
    uint32_t alarm_total_seconds;
    uint32_t alarm_start_ms;
    bool alarm_done;
    volatile bool alarm_sound_stop_requested;
    volatile bool alarm_sound_running;
    TaskHandle_t alarm_sound_task;
    bool suspended;
    bool stopwatch_running;
    uint32_t stopwatch_accum_ms;
    uint32_t stopwatch_start_ms;
} clock_state_t;

static const char *TAG = "solar_os_clock";
static clock_state_t clock_state;

static uint32_t clock_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int clock_colon_radius(int thick)
{
    return thick > 6 ? thick / 2 : 3;
}

static int clock_segment_gap(int thick)
{
    int gap = thick / 12;
    if (gap < 1) {
        gap = 1;
    }
    if (gap > 3) {
        gap = 3;
    }
    return gap;
}

static void clock_fill_segment(solar_os_gfx_t *gfx,
                               const solar_os_gfx_point_t *points,
                               size_t point_count,
                               int gap)
{
    if (points == NULL || point_count < 3 || point_count > 8) {
        return;
    }

    solar_os_gfx_point_t inset[8];
    int min_x = points[0].x;
    int max_x = points[0].x;
    int min_y = points[0].y;
    int max_y = points[0].y;
    for (size_t i = 1; i < point_count; i++) {
        if (points[i].x < min_x) {
            min_x = points[i].x;
        }
        if (points[i].x > max_x) {
            max_x = points[i].x;
        }
        if (points[i].y < min_y) {
            min_y = points[i].y;
        }
        if (points[i].y > max_y) {
            max_y = points[i].y;
        }
    }

    const int center_x = (min_x + max_x) / 2;
    const int center_y = (min_y + max_y) / 2;
    for (size_t i = 0; i < point_count; i++) {
        inset[i] = points[i];
        if (gap > 0) {
            if (inset[i].x < center_x) {
                inset[i].x += gap;
            } else if (inset[i].x > center_x) {
                inset[i].x -= gap;
            }
            if (inset[i].y < center_y) {
                inset[i].y += gap;
            } else if (inset[i].y > center_y) {
                inset[i].y -= gap;
            }
        }
    }

    solar_os_gfx_fill_polygon(gfx, inset, point_count);
}

static void clock_draw_segment(solar_os_gfx_t *gfx,
                               int segment,
                               int x,
                               int y,
                               int width,
                               int height,
                               int thick)
{
    if (width <= (2 * thick) || height <= (3 * thick)) {
        return;
    }

    const int segment_gap = clock_segment_gap(thick);
    const int bevel = thick / 2;
    const int left = x;
    const int right = x + width - 1;
    const int top = y;
    const int bottom = y + height - 1;
    const int mid = y + (height / 2);
    const int left_tip = left + bevel;
    const int left_inner = left + thick;
    const int right_tip = right - bevel;
    const int right_inner = right - thick;
    const int top_inner = top + thick;
    const int mid_top = mid - bevel;
    const int mid_bottom = mid + bevel;
    const int bottom_inner = bottom - thick;

    switch (segment) {
    case 0: {
        const solar_os_gfx_point_t points[] = {
            {left_inner, top},
            {right_inner, top},
            {right_tip, top + bevel},
            {right_inner, top_inner},
            {left_inner, top_inner},
            {left_tip, top + bevel},
        };
        clock_fill_segment(gfx, points, sizeof(points) / sizeof(points[0]), segment_gap);
        break;
    }
    case 1: {
        const solar_os_gfx_point_t points[] = {
            {right_tip, top + bevel},
            {right, top_inner},
            {right, mid_top},
            {right_tip, mid},
            {right_inner, mid_top},
            {right_inner, top_inner},
        };
        clock_fill_segment(gfx, points, sizeof(points) / sizeof(points[0]), segment_gap);
        break;
    }
    case 2: {
        const solar_os_gfx_point_t points[] = {
            {right_tip, mid},
            {right, mid_bottom},
            {right, bottom_inner},
            {right_tip, bottom - bevel},
            {right_inner, bottom_inner},
            {right_inner, mid_bottom},
        };
        clock_fill_segment(gfx, points, sizeof(points) / sizeof(points[0]), segment_gap);
        break;
    }
    case 3: {
        const solar_os_gfx_point_t points[] = {
            {left_inner, bottom_inner},
            {right_inner, bottom_inner},
            {right_tip, bottom - bevel},
            {right_inner, bottom},
            {left_inner, bottom},
            {left_tip, bottom - bevel},
        };
        clock_fill_segment(gfx, points, sizeof(points) / sizeof(points[0]), segment_gap);
        break;
    }
    case 4: {
        const solar_os_gfx_point_t points[] = {
            {left_tip, mid},
            {left_inner, mid_bottom},
            {left_inner, bottom_inner},
            {left_tip, bottom - bevel},
            {left, bottom_inner},
            {left, mid_bottom},
        };
        clock_fill_segment(gfx, points, sizeof(points) / sizeof(points[0]), segment_gap);
        break;
    }
    case 5: {
        const solar_os_gfx_point_t points[] = {
            {left_tip, top + bevel},
            {left_inner, top_inner},
            {left_inner, mid_top},
            {left_tip, mid},
            {left, mid_top},
            {left, top_inner},
        };
        clock_fill_segment(gfx, points, sizeof(points) / sizeof(points[0]), segment_gap);
        break;
    }
    case 6: {
        const solar_os_gfx_point_t points[] = {
            {left_inner, mid_top},
            {right_inner, mid_top},
            {right_tip, mid},
            {right_inner, mid_bottom},
            {left_inner, mid_bottom},
            {left_tip, mid},
        };
        clock_fill_segment(gfx, points, sizeof(points) / sizeof(points[0]), segment_gap);
        break;
    }
    default:
        break;
    }
}

static void clock_draw_digit(solar_os_gfx_t *gfx,
                             int value,
                             int x,
                             int y,
                             int width,
                             int height,
                             int thick)
{
    static const uint8_t digit_masks[] = {
        0x3f, /* 0 */
        0x06, /* 1 */
        0x5b, /* 2 */
        0x4f, /* 3 */
        0x66, /* 4 */
        0x6d, /* 5 */
        0x7d, /* 6 */
        0x07, /* 7 */
        0x7f, /* 8 */
        0x6f, /* 9 */
    };

    uint8_t mask = 0x40;
    if (value >= 0 && value <= 9) {
        mask = digit_masks[value];
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    for (int segment = 0; segment < 7; segment++) {
        clock_draw_segment(gfx, segment, x, y, width, height, thick);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int segment = 0; segment < 7; segment++) {
        if ((mask & (1U << segment)) != 0) {
            clock_draw_segment(gfx, segment, x, y, width, height, thick);
        }
    }
}

static void clock_draw_colon(solar_os_gfx_t *gfx, int x, int y, int height, int thick)
{
    const int radius = clock_colon_radius(thick);
    const int center_x = x + radius;

    solar_os_gfx_fill_circle(gfx, center_x, y + (height / 3), radius);
    solar_os_gfx_fill_circle(gfx, center_x, y + ((height * 2) / 3), radius);
}

static void clock_layout(solar_os_gfx_t *gfx,
                         int *digit_x,
                         int *digit_y,
                         int *digit_width,
                         int *digit_height,
                         int *thick,
                         int *gap,
                         int *colon_width)
{
    const int screen_width = (int)solar_os_gfx_width(gfx);
    const int screen_height = (int)solar_os_gfx_height(gfx);
    const int margin_x = screen_width > 80 ? screen_width / 20 : 4;

    *gap = screen_width > 160 ? screen_width / 40 : 4;
    *colon_width = screen_width > 200 ? screen_width / 18 : 10;

    const int available_width = screen_width - (2 * margin_x) - *colon_width - (4 * (*gap));
    *digit_width = available_width > 80 ? available_width / 4 : 18;
    *digit_height = (screen_height * 3) / 5;
    if (*digit_height > *digit_width * 2) {
        *digit_height = *digit_width * 2;
    }
    if (*digit_height < 36) {
        *digit_height = 36;
    }

    *thick = *digit_width / 4;
    if (*thick < 6) {
        *thick = 6;
    }
    if (*thick * 3 > *digit_width) {
        *thick = *digit_width / 3;
    }
    *colon_width = (2 * clock_colon_radius(*thick)) + 1;

    const int total_width = (4 * (*digit_width)) + *colon_width + (4 * (*gap));
    *digit_x = (screen_width - total_width) / 2;
    *digit_y = (screen_height - *digit_height) / 2;
    if (*digit_x < 0) {
        *digit_x = 0;
    }
    if (*digit_y < 0) {
        *digit_y = 0;
    }
}

static void clock_render_digits(solar_os_context_t *ctx,
                                int left_value,
                                int right_value,
                                bool colon_active)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    int digit_x;
    int digit_y;
    int digit_width;
    int digit_height;
    int thick;
    int gap;
    int colon_width;
    clock_layout(gfx, &digit_x, &digit_y, &digit_width, &digit_height, &thick, &gap, &colon_width);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    const bool left_valid = left_value >= 0 && left_value <= 99;
    const bool right_valid = right_value >= 0 && right_value <= 99;
    int x = digit_x;

    clock_draw_digit(gfx, left_valid ? left_value / 10 : -1, x, digit_y, digit_width, digit_height, thick);
    x += digit_width + gap;
    clock_draw_digit(gfx, left_valid ? left_value % 10 : -1, x, digit_y, digit_width, digit_height, thick);
    x += digit_width + gap;
    solar_os_gfx_set_color(gfx, colon_active ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_LIGHT);
    clock_draw_colon(gfx, x, digit_y, digit_height, thick);
    x += colon_width + gap;
    clock_draw_digit(gfx, right_valid ? right_value / 10 : -1, x, digit_y, digit_width, digit_height, thick);
    x += digit_width + gap;
    clock_draw_digit(gfx, right_valid ? right_value % 10 : -1, x, digit_y, digit_width, digit_height, thick);

    solar_os_gfx_present(gfx);
}

static uint32_t clock_stopwatch_elapsed_ms(uint32_t now_ms)
{
    uint32_t elapsed_ms = clock_state.stopwatch_accum_ms;
    if (clock_state.stopwatch_running) {
        elapsed_ms += now_ms - clock_state.stopwatch_start_ms;
    }
    return elapsed_ms;
}

static uint32_t clock_alarm_remaining_seconds(uint32_t now_ms)
{
    const uint32_t elapsed_seconds = (now_ms - clock_state.alarm_start_ms) / 1000U;
    if (elapsed_seconds >= clock_state.alarm_total_seconds) {
        return 0;
    }
    return clock_state.alarm_total_seconds - elapsed_seconds;
}

static void clock_render_mmss(solar_os_context_t *ctx, uint32_t total_seconds, bool colon_active)
{
    uint32_t minutes = total_seconds / 60U;
    const uint32_t seconds = total_seconds % 60U;
    if (minutes > CLOCK_DISPLAY_MAX_MINUTES) {
        minutes = CLOCK_DISPLAY_MAX_MINUTES;
    }
    clock_render_digits(ctx, (int)minutes, (int)seconds, colon_active);
}

static void clock_render_time(solar_os_context_t *ctx)
{
    solar_os_datetime_t datetime;
    const esp_err_t err = solar_os_time_get_datetime(&datetime);
    const bool valid = err == ESP_OK &&
        solar_os_time_datetime_is_valid(&datetime) &&
        datetime.clock_integrity;

    clock_render_digits(ctx,
                        valid ? datetime.hour : -1,
                        valid ? datetime.minute : -1,
                        !valid || (datetime.second % 2U) == 0);
}

static void clock_render(solar_os_context_t *ctx)
{
    if (clock_state.suspended) {
        return;
    }

    const uint32_t now_ms = clock_now_ms();

    switch (clock_state.mode) {
    case CLOCK_MODE_ALARM:
        clock_render_mmss(ctx, clock_alarm_remaining_seconds(now_ms), true);
        break;
    case CLOCK_MODE_STOPWATCH:
        clock_render_mmss(ctx, clock_stopwatch_elapsed_ms(now_ms) / 1000U, true);
        break;
    case CLOCK_MODE_TIME:
    default:
        clock_render_time(ctx);
        break;
    }
}

static bool clock_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool clock_parse_mmss(const char *text, uint32_t *total_seconds)
{
    if (text == NULL || total_seconds == NULL) {
        return false;
    }

    const char *colon = strchr(text, ':');
    if (colon == NULL || colon == text || colon[1] == '\0' || strchr(colon + 1, ':') != NULL) {
        return false;
    }

    char minutes_text[4];
    const size_t minutes_len = (size_t)(colon - text);
    if (minutes_len == 0 || minutes_len >= sizeof(minutes_text)) {
        return false;
    }
    memcpy(minutes_text, text, minutes_len);
    minutes_text[minutes_len] = '\0';

    uint32_t minutes = 0;
    uint32_t seconds = 0;
    if (!clock_parse_u32(minutes_text, 0, CLOCK_DISPLAY_MAX_MINUTES, &minutes) ||
        !clock_parse_u32(colon + 1, 0, 59, &seconds)) {
        return false;
    }

    const uint32_t total = (minutes * 60U) + seconds;
    if (total == 0) {
        return false;
    }
    *total_seconds = total;
    return true;
}

static bool clock_parse_args(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);

    clock_state.mode = CLOCK_MODE_TIME;
    clock_state.alarm_total_seconds = 0;
    if (argc == 1) {
        return true;
    }

    const char *arg1 = solar_os_context_argv(ctx, 1);
    if (argc == 2 && arg1 != NULL && strcmp(arg1, "-s") == 0) {
        clock_state.mode = CLOCK_MODE_STOPWATCH;
        return true;
    }
    if (argc == 3 && arg1 != NULL && strcmp(arg1, "-a") == 0) {
        const char *arg2 = solar_os_context_argv(ctx, 2);
        if (!clock_parse_mmss(arg2, &clock_state.alarm_total_seconds)) {
            return false;
        }
        clock_state.mode = CLOCK_MODE_ALARM;
        return true;
    }

    return false;
}

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
static void clock_alarm_sound_task(void *arg)
{
    (void)arg;

    clock_state.alarm_sound_running = true;
    bool sound_available = true;

    while (!clock_state.alarm_sound_stop_requested) {
        if (sound_available) {
            for (int i = 0; i < 4 && !clock_state.alarm_sound_stop_requested; i++) {
                const esp_err_t err = solar_os_audio_play_tone(1200,
                                                               70,
                                                               SOLAR_OS_AUDIO_VOLUME_GLOBAL);
                if (err != ESP_OK) {
                    sound_available = false;
                    SOLAR_OS_LOGW(TAG, "alarm sound unavailable: %s", esp_err_to_name(err));
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(45));
            }
        }

        for (int i = 0; i < 10 && !clock_state.alarm_sound_stop_requested; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    clock_state.alarm_sound_running = false;
    clock_state.alarm_sound_task = NULL;
    solar_os_task_delete_internal(NULL);
}

static void clock_alarm_sound_start(void)
{
    if (clock_state.alarm_sound_task != NULL || clock_state.alarm_sound_running) {
        return;
    }

    clock_state.alarm_sound_stop_requested = false;
    if (solar_os_task_create_pinned_internal(
            clock_alarm_sound_task,
            "clock_alarm",
            CLOCK_ALARM_SOUND_TASK_STACK,
            NULL,
            CLOCK_ALARM_SOUND_TASK_PRIORITY,
            &clock_state.alarm_sound_task,
            tskNO_AFFINITY) != pdPASS) {
        clock_state.alarm_sound_task = NULL;
        SOLAR_OS_LOGW(TAG, "alarm sound task allocation failed");
    }
}

static void clock_alarm_sound_stop(void)
{
    clock_state.alarm_sound_stop_requested = true;
    for (uint32_t i = 0; i < 40 &&
         (clock_state.alarm_sound_task != NULL || clock_state.alarm_sound_running); i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
#else
static void clock_alarm_sound_start(void)
{
}

static void clock_alarm_sound_stop(void)
{
    clock_state.alarm_sound_stop_requested = true;
}
#endif

static esp_err_t clock_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!clock_parse_args(ctx)) {
        return ESP_ERR_INVALID_ARG;
    }

    clock_state.last_second = UINT32_MAX;
    clock_state.alarm_start_ms = clock_now_ms();
    clock_state.alarm_done = false;
    clock_state.alarm_sound_stop_requested = false;
    clock_state.suspended = false;
    clock_state.stopwatch_running = false;
    clock_state.stopwatch_accum_ms = 0;
    clock_state.stopwatch_start_ms = clock_now_ms();
    solar_os_context_set_graphics_active(ctx, true);
    clock_render(ctx);
    return ESP_OK;
}

static void clock_stop(solar_os_context_t *ctx)
{
    clock_alarm_sound_stop();
    clock_state.suspended = false;
    solar_os_context_set_graphics_active(ctx, false);
}

static void clock_suspend(solar_os_context_t *ctx)
{
    clock_state.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void clock_resume(solar_os_context_t *ctx)
{
    clock_state.suspended = false;
    clock_state.last_second = UINT32_MAX;
    solar_os_context_set_graphics_active(ctx, true);
    clock_render(ctx);
}

static void clock_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    switch (clock_state.mode) {
    case CLOCK_MODE_ALARM:
        strlcpy(buffer, "clock alarm", buffer_len);
        break;
    case CLOCK_MODE_STOPWATCH:
        strlcpy(buffer, "clock stopwatch", buffer_len);
        break;
    case CLOCK_MODE_TIME:
    default:
        strlcpy(buffer, "clock", buffer_len);
        break;
    }
}

static void clock_stopwatch_toggle(void)
{
    const uint32_t now_ms = clock_now_ms();
    if (clock_state.stopwatch_running) {
        clock_state.stopwatch_accum_ms += now_ms - clock_state.stopwatch_start_ms;
        clock_state.stopwatch_running = false;
    } else {
        clock_state.stopwatch_start_ms = now_ms;
        clock_state.stopwatch_running = true;
    }
    clock_state.last_second = UINT32_MAX;
}

static void clock_stopwatch_zero(void)
{
    clock_state.stopwatch_running = false;
    clock_state.stopwatch_accum_ms = 0;
    clock_state.stopwatch_start_ms = clock_now_ms();
    clock_state.last_second = UINT32_MAX;
}

static bool clock_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t ch = (uint8_t)event->data.ch;
        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_request_exit(ctx);
        } else if (clock_state.mode == CLOCK_MODE_STOPWATCH) {
            if (event->data.ch == ' ') {
                clock_stopwatch_toggle();
            } else {
                clock_stopwatch_zero();
            }
            clock_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        uint32_t second = event->data.tick_ms / 1000U;
        if (clock_state.mode == CLOCK_MODE_ALARM) {
            const uint32_t remaining = clock_alarm_remaining_seconds(event->data.tick_ms);
            second = remaining;
            if (remaining == 0 && !clock_state.alarm_done) {
                clock_state.alarm_done = true;
                clock_alarm_sound_start();
            }
        } else if (clock_state.mode == CLOCK_MODE_STOPWATCH) {
            second = clock_stopwatch_elapsed_ms(event->data.tick_ms) / 1000U;
        }
        if (second != clock_state.last_second) {
            clock_state.last_second = second;
            if (!clock_state.suspended) {
                clock_render(ctx);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        clock_resume(ctx);
        return true;
    }

    return false;
}

const solar_os_app_t solar_os_clock_app = {
    .name = "clock",
    .summary = "clock, countdown alarm, stopwatch",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = clock_start,
    .suspend = clock_suspend,
    .resume = clock_resume,
    .stop = clock_stop,
    .event = clock_event,
    .title = clock_title,
    .tick_interval_ms = 250U,
    .tick_deadline_ms = 25U,
};
