#include "solar_os_logic_app.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_logic.h"
#include "solar_os_memory.h"

#define LOGIC_REFRESH_MS 250U

typedef struct {
    bool running;
    bool suspended;
    uint8_t *samples;
    size_t sample_capacity;
    solar_os_logic_status_t capture;
    size_t view_start;
    size_t visible_samples;
    uint32_t next_refresh_ms;
    char message[64];
} logic_app_state_t;

static logic_app_state_t logic_app;

static bool logic_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static esp_err_t logic_parse_pins(const char *text, solar_os_logic_config_t *config)
{
    if (text == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char pins[SOLAR_OS_APP_ARG_LEN];
    strlcpy(pins, text, sizeof(pins));
    char *save = NULL;
    for (char *token = strtok_r(pins, ",", &save);
         token != NULL;
         token = strtok_r(NULL, ",", &save)) {
        uint32_t pin = 0;
        if (config->channel_count >= SOLAR_OS_LOGIC_MAX_CHANNELS ||
            !logic_parse_u32(token, 0, UINT8_MAX, &pin)) {
            return ESP_ERR_INVALID_ARG;
        }
        config->pins[config->channel_count++] = (uint8_t)pin;
    }
    return config->channel_count > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t logic_parse_config(solar_os_context_t *ctx, solar_os_logic_config_t *config)
{
    if (ctx == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || argc > 4) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->sample_rate_hz = SOLAR_OS_LOGIC_DEFAULT_RATE_HZ;
    config->sample_count = SOLAR_OS_LOGIC_DEFAULT_SAMPLES;

    esp_err_t err = logic_parse_pins(solar_os_context_argv(ctx, 1), config);
    if (err != ESP_OK) {
        return err;
    }
    if (argc >= 3 &&
        !logic_parse_u32(solar_os_context_argv(ctx, 2),
                         SOLAR_OS_LOGIC_MIN_RATE_HZ,
                         SOLAR_OS_LOGIC_MAX_RATE_HZ,
                         &config->sample_rate_hz)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (argc >= 4 &&
        !logic_parse_u32(solar_os_context_argv(ctx, 3),
                         1U,
                         SOLAR_OS_LOGIC_MAX_SAMPLES,
                         &config->sample_count)) {
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_logic_validate_config(config);
}

static bool logic_sump_running(void)
{
    solar_os_job_status_t status;
    return solar_os_jobs_get_by_name("sump", &status) && status.state == SOLAR_OS_JOB_RUNNING;
}

static void logic_free_samples(void)
{
    solar_os_memory_free(logic_app.samples);
    logic_app.samples = NULL;
    logic_app.sample_capacity = 0;
}

static esp_err_t logic_reserve_samples(size_t count)
{
    if (logic_app.samples != NULL && logic_app.sample_capacity >= count) {
        return ESP_OK;
    }

    uint8_t *replacement = solar_os_memory_alloc(count,
                                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                  "logic.samples");
    if (replacement == NULL) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_memory_free(logic_app.samples);
    logic_app.samples = replacement;
    logic_app.sample_capacity = count;
    return ESP_OK;
}

static esp_err_t logic_load_latest(void)
{
    solar_os_logic_status_t status;
    esp_err_t err = solar_os_logic_get_status(&status);
    if (err != ESP_OK || !status.has_capture) {
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    err = logic_reserve_samples(status.config.sample_count);
    if (err != ESP_OK) {
        return err;
    }

    size_t copied = 0;
    err = solar_os_logic_copy_samples(0,
                                      logic_app.samples,
                                      status.config.sample_count,
                                      &copied);
    if (err != ESP_OK || copied != status.config.sample_count) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    const bool first_capture = !logic_app.capture.has_capture;
    logic_app.capture = status;
    if (first_capture || logic_app.visible_samples == 0) {
        logic_app.visible_samples = status.config.sample_count;
        logic_app.view_start = 0;
    } else {
        if (logic_app.visible_samples > status.config.sample_count) {
            logic_app.visible_samples = status.config.sample_count;
        }
        if (logic_app.view_start + logic_app.visible_samples > status.config.sample_count) {
            logic_app.view_start = status.config.sample_count - logic_app.visible_samples;
        }
    }
    logic_app.message[0] = '\0';
    return ESP_OK;
}

static int logic_level_y(int row_top, int row_height, bool high)
{
    const int pad = row_height > 8 ? 3 : 1;
    return high ? row_top + pad : row_top + row_height - pad - 1;
}

static void logic_draw_channel(solar_os_gfx_t *gfx,
                               uint8_t channel,
                               int left,
                               int top,
                               int width,
                               int height)
{
    const size_t count = logic_app.capture.config.sample_count;
    if (count == 0 || logic_app.visible_samples == 0 || width <= 1 || height <= 3) {
        return;
    }

    char label[12];
    snprintf(label, sizeof(label), "G%u", (unsigned)logic_app.capture.config.pins[channel]);
    solar_os_gfx_text(gfx, 2, top + (height / 2) + 3, label);

    size_t sample_index = logic_app.view_start;
    bool level = (logic_app.samples[sample_index] & (1U << channel)) != 0;
    int previous_x = left;
    int previous_y = logic_level_y(top, height, level);

    for (int x = 1; x < width; x++) {
        sample_index = logic_app.view_start +
            ((size_t)x * (logic_app.visible_samples - 1U)) / (size_t)(width - 1);
        if (sample_index >= count) {
            sample_index = count - 1U;
        }
        const bool next_level = (logic_app.samples[sample_index] & (1U << channel)) != 0;
        const int screen_x = left + x;
        const int next_y = logic_level_y(top, height, next_level);
        solar_os_gfx_line(gfx, previous_x, previous_y, screen_x, previous_y);
        if (next_y != previous_y) {
            solar_os_gfx_line(gfx, screen_x, previous_y, screen_x, next_y);
        }
        previous_x = screen_x;
        previous_y = next_y;
    }
}

static void logic_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || logic_app.suspended) {
        return;
    }

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    if (!logic_app.capture.has_capture) {
        solar_os_gfx_text(gfx, 4, 12, "logic analyzer");
        solar_os_gfx_text(gfx,
                          4,
                          height / 2,
                          logic_app.message[0] ? logic_app.message : "no capture");
        solar_os_gfx_present(gfx);
        return;
    }

    char header[96];
    snprintf(header,
             sizeof(header),
             "logic %uch %luHz %lu samples",
             (unsigned)logic_app.capture.config.channel_count,
             (unsigned long)logic_app.capture.effective_rate_hz,
             (unsigned long)logic_app.capture.config.sample_count);
    solar_os_gfx_text(gfx, 2, 10, header);

    const int left = width > 120 ? 32 : 22;
    const int top = 16;
    const int bottom = 18;
    const int plot_height = height - top - bottom;
    const int row_height = plot_height / logic_app.capture.config.channel_count;
    const int plot_width = width - left - 2;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
    for (int i = 0; i <= 4; i++) {
        const int x = left + (plot_width * i) / 4;
        solar_os_gfx_line(gfx, x, top, x, top + plot_height - 1);
    }
    for (uint8_t channel = 0; channel < logic_app.capture.config.channel_count; channel++) {
        const int y = top + channel * row_height;
        solar_os_gfx_line(gfx, left, y + row_height - 1, left + plot_width, y + row_height - 1);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (uint8_t channel = 0; channel < logic_app.capture.config.channel_count; channel++) {
        logic_draw_channel(gfx,
                           channel,
                           left,
                           top + channel * row_height,
                           plot_width,
                           row_height);
    }

    const size_t end = logic_app.view_start + logic_app.visible_samples;
    const uint64_t start_us =
        ((uint64_t)logic_app.view_start * 1000000ULL) / logic_app.capture.effective_rate_hz;
    const uint64_t span_us =
        ((uint64_t)logic_app.visible_samples * 1000000ULL) / logic_app.capture.effective_rate_hz;
    char footer[96];
    snprintf(footer,
             sizeof(footer),
             "%u-%u  +%lluus  span %lluus%s%s",
             (unsigned)(logic_app.view_start + 1U),
             (unsigned)end,
             (unsigned long long)start_us,
             (unsigned long long)span_us,
             logic_app.message[0] ? "  " : "",
             logic_app.message);
    solar_os_gfx_text(gfx, 2, height - 3, footer);
    solar_os_gfx_present(gfx);
}

static esp_err_t logic_capture_local(const solar_os_logic_config_t *config)
{
    if (logic_sump_running()) {
        strlcpy(logic_app.message, "SUMP owns capture", sizeof(logic_app.message));
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = solar_os_logic_capture(config);
    if (err != ESP_OK) {
        snprintf(logic_app.message, sizeof(logic_app.message), "%s", esp_err_to_name(err));
        return err;
    }
    return logic_load_latest();
}

static esp_err_t logic_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&logic_app, 0, sizeof(logic_app));
    esp_err_t err = solar_os_logic_init();
    if (err != ESP_OK) {
        return err;
    }

    if (solar_os_context_argc(ctx) > 1) {
        solar_os_logic_config_t config;
        err = logic_parse_config(ctx, &config);
        if (err == ESP_OK) {
            err = logic_capture_local(&config);
        }
    } else {
        err = logic_load_latest();
        if (err == ESP_ERR_NOT_FOUND && !logic_sump_running()) {
            solar_os_logic_config_t config;
            err = solar_os_logic_default_config(&config);
            if (err == ESP_OK) {
                err = logic_capture_local(&config);
            }
        }
    }

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        logic_free_samples();
        return err;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        strlcpy(logic_app.message, "waiting for SUMP capture", sizeof(logic_app.message));
    }

    logic_app.running = true;
    solar_os_context_set_graphics_active(ctx, true);
    logic_render(ctx);
    return ESP_OK;
}

static void logic_stop(solar_os_context_t *ctx)
{
    logic_app.running = false;
    logic_app.suspended = false;
    logic_free_samples();
    solar_os_context_set_graphics_active(ctx, false);
}

static void logic_suspend(solar_os_context_t *ctx)
{
    logic_app.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void logic_resume(solar_os_context_t *ctx)
{
    logic_app.suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    logic_render(ctx);
}

static void logic_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0) {
        strlcpy(buffer, "logic", buffer_len);
    }
}

static void logic_zoom(bool in)
{
    const size_t total = logic_app.capture.config.sample_count;
    if (total == 0) {
        return;
    }
    const size_t center = logic_app.view_start + logic_app.visible_samples / 2U;
    size_t visible = in ? logic_app.visible_samples / 2U : logic_app.visible_samples * 2U;
    if (visible < 16U) {
        visible = total < 16U ? total : 16U;
    }
    if (visible > total) {
        visible = total;
    }
    logic_app.visible_samples = visible;
    logic_app.view_start = center > visible / 2U ? center - visible / 2U : 0;
    if (logic_app.view_start + visible > total) {
        logic_app.view_start = total - visible;
    }
}

static void logic_pan(bool right)
{
    const size_t total = logic_app.capture.config.sample_count;
    const size_t step = logic_app.visible_samples > 8U ? logic_app.visible_samples / 4U : 1U;
    if (right) {
        logic_app.view_start = logic_app.view_start + logic_app.visible_samples + step >= total ?
            total - logic_app.visible_samples :
            logic_app.view_start + step;
    } else {
        logic_app.view_start = logic_app.view_start > step ? logic_app.view_start - step : 0;
    }
}

static bool logic_handle_char(solar_os_context_t *ctx, char ch)
{
    const uint8_t key = (uint8_t)ch;
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (!logic_app.capture.has_capture) {
        return false;
    }
    if (key == SOLAR_OS_KEY_LEFT) {
        logic_pan(false);
    } else if (key == SOLAR_OS_KEY_RIGHT) {
        logic_pan(true);
    } else if (key == SOLAR_OS_KEY_PAGE_UP || ch == '+' || ch == '=') {
        logic_zoom(true);
    } else if (key == SOLAR_OS_KEY_PAGE_DOWN || ch == '-') {
        logic_zoom(false);
    } else if (ch == 'r' || ch == 'R') {
        (void)logic_capture_local(&logic_app.capture.config);
    } else if (ch == 'a' || ch == 'A' || key == SOLAR_OS_KEY_HOME) {
        logic_app.view_start = 0;
        logic_app.visible_samples = logic_app.capture.config.sample_count;
    } else {
        return false;
    }

    logic_render(ctx);
    return true;
}

static bool logic_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return logic_handle_char(ctx, event->data.ch);
    }
    if (event->type != SOLAR_OS_EVENT_TICK || logic_app.suspended) {
        return false;
    }
    if (logic_app.next_refresh_ms != 0 &&
        (int32_t)(event->data.tick_ms - logic_app.next_refresh_ms) < 0) {
        return false;
    }
    logic_app.next_refresh_ms = event->data.tick_ms + LOGIC_REFRESH_MS;

    solar_os_logic_status_t status;
    if (solar_os_logic_get_status(&status) == ESP_OK &&
        status.has_capture &&
        status.generation != logic_app.capture.generation &&
        logic_load_latest() == ESP_OK) {
        logic_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_logic_app = {
    .name = "logic",
    .summary = "logic analyzer waveform viewer",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = logic_start,
    .suspend = logic_suspend,
    .resume = logic_resume,
    .stop = logic_stop,
    .event = logic_event,
    .title = logic_title,
};
