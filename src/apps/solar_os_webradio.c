#include "solar_os_webradio.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_audio_codec.h"
#include "solar_os_audio_pcm.h"
#include "solar_os_audio_player.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_http_client.h"
#include "solar_os_log.h"
#include "solar_os_media_widgets.h"
#include "solar_os_memory.h"
#include "solar_os_shell_io.h"
#include "solar_os_signal_widgets.h"
#include "solar_os_stream.h"
#include "solar_os_task.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"
#include "solar_os_webradio_catalog.h"

#define WEBRADIO_TASK_STACK 20480U
#define WEBRADIO_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(WEBRADIO_TASK_STACK);
#endif
#define WEBRADIO_HTTP_TIMEOUT_MS 10000U
#define WEBRADIO_HTTP_READ_POLL_MS 250U
#define WEBRADIO_RECONNECT_DELAY_MS 2000U
#define WEBRADIO_INPUT_BYTES 16384U
#define WEBRADIO_PCM_BUFFER_BYTES 4096U
#define WEBRADIO_OUTPUT_SAMPLES (WEBRADIO_PCM_BUFFER_BYTES / sizeof(int16_t))
#define WEBRADIO_JITTER_EXTERNAL_BYTES (128U * 1024U)
#define WEBRADIO_JITTER_INTERNAL_BYTES (32U * 1024U)
#define WEBRADIO_JITTER_TARGET_MS 500U
#define WEBRADIO_WORKER_POLL_MS 20U
#define WEBRADIO_INVALID_STREAM_BYTES (64U * 1024U)
#define WEBRADIO_GUI_HEADER_HEIGHT 28
#define WEBRADIO_GUI_ROW_HEIGHT 24
#define WEBRADIO_GUI_FOOTER_HEIGHT 20
#define WEBRADIO_SCOPE_SAMPLES 256U
#define WEBRADIO_SPECTRUM_FFT_SIZE 256U
#define WEBRADIO_VISUALIZER_REFRESH_MS 40U
#define WEBRADIO_DISPLAY_HPM_HZ_TENTHS 255U
#define WEBRADIO_VOLUME_STEP 5U
#define WEBRADIO_GUI_INPUT_MAX SOLAR_OS_WEBRADIO_URL_MAX

typedef enum {
    WEBRADIO_MODE_TUI = 0,
    WEBRADIO_MODE_GRAPHICS,
} webradio_mode_t;

typedef enum {
    WEBRADIO_PLAYBACK_IDLE = 0,
    WEBRADIO_PLAYBACK_CONNECTING,
    WEBRADIO_PLAYBACK_BUFFERING,
    WEBRADIO_PLAYBACK_PLAYING,
    WEBRADIO_PLAYBACK_RECONNECTING,
    WEBRADIO_PLAYBACK_ERROR,
} webradio_playback_state_t;

typedef enum {
    WEBRADIO_TAB_PLAYER = 0,
    WEBRADIO_TAB_CHANNELS,
    WEBRADIO_TAB_COUNT,
} webradio_tab_t;

typedef enum {
    WEBRADIO_VISUALIZER_SCOPE = 0,
    WEBRADIO_VISUALIZER_SPECTRUM,
} webradio_visualizer_t;

typedef enum {
    WEBRADIO_DIALOG_NONE = 0,
    WEBRADIO_DIALOG_ADD_NAME,
    WEBRADIO_DIALOG_ADD_URL,
    WEBRADIO_DIALOG_EDIT_NAME,
    WEBRADIO_DIALOG_EDIT_URL,
} webradio_dialog_t;

typedef struct {
    solar_os_audio_player_t *player;
    solar_os_stream_audio_format_t output_format;
    solar_os_audio_mp3_decoder_t *decoder;
    solar_os_audio_s16_converter_t converter;
    uint8_t *input;
    size_t input_len;
    int16_t *decoded;
    int16_t *output;
    int16_t *playback;
    size_t playback_samples;
    uint64_t network_bytes;
    uint64_t output_bytes;
    bool decoded_any;
    volatile bool playback_started;
    volatile esp_err_t playback_error;
    char content_type[64];
} webradio_worker_t;

typedef struct {
    webradio_mode_t mode;
    solar_os_tui_t tui;
    solar_os_shell_io_t fallback_io;
    solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
    size_t station_count;
    size_t cursor;
    size_t top;
    uint32_t catalog_generation;
    bool ui_started;
    bool suspended;
    bool high_refresh_active;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    bool redraw;
    volatile bool stop_requested;
    volatile bool task_done;
    TaskHandle_t task;
    webradio_playback_state_t playback_state;
    char playback_message[96];
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    char active_url[SOLAR_OS_WEBRADIO_URL_MAX];
    uint32_t source_rate;
    uint8_t source_channels;
    char active_device_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    uint32_t active_device_capabilities;
    webradio_tab_t tab;
    webradio_visualizer_t visualizer;
    webradio_dialog_t dialog;
    solar_os_oscilloscope_widget_t *scope;
    solar_os_spectrum_widget_t *spectrum;
    uint32_t last_visualizer_ms;
    uint8_t volume;
    char ui_message[96];
    char edit_original_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    char edit_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    char edit_url[SOLAR_OS_WEBRADIO_URL_MAX];
    char dialog_input[WEBRADIO_GUI_INPUT_MAX];
    size_t dialog_input_len;
} webradio_app_state_t;

static const char *TAG = "solar_os_webradio";
static void *webradio_state;
#define webradio (*(webradio_app_state_t *)webradio_state)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("audio worker callback spinlock")
static portMUX_TYPE webradio_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *webradio_dialog_label(void);

static void webradio_enable_high_refresh(solar_os_context_t *ctx)
{
    if (webradio.high_refresh_active) {
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (!solar_os_gfx_display_target_name(
            gfx, webradio.display_target, sizeof(webradio.display_target))) {
        return;
    }
    const esp_err_t err = solar_os_display_set_high_refresh_override(
        webradio.display_target, true, WEBRADIO_DISPLAY_HPM_HZ_TENTHS);
    if (err == ESP_OK) {
        webradio.high_refresh_active = true;
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        SOLAR_OS_LOGW(TAG,
                      "25.5 Hz display refresh unavailable: %s",
                      esp_err_to_name(err));
    }
}

static void webradio_disable_high_refresh(void)
{
    if (!webradio.high_refresh_active) {
        webradio.display_target[0] = '\0';
        return;
    }
    const esp_err_t err = solar_os_display_set_high_refresh_override(
        webradio.display_target, false, 0U);
    webradio.high_refresh_active = false;
    webradio.display_target[0] = '\0';
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "display refresh restore failed: %s",
                      esp_err_to_name(err));
    }
}

static const char *webradio_state_name(webradio_playback_state_t state)
{
    switch (state) {
    case WEBRADIO_PLAYBACK_CONNECTING:
        return "connecting";
    case WEBRADIO_PLAYBACK_BUFFERING:
        return "buffering";
    case WEBRADIO_PLAYBACK_PLAYING:
        return "playing";
    case WEBRADIO_PLAYBACK_RECONNECTING:
        return "reconnecting";
    case WEBRADIO_PLAYBACK_ERROR:
        return "error";
    case WEBRADIO_PLAYBACK_IDLE:
    default:
        return "stopped";
    }
}

static void webradio_set_playback_state(webradio_playback_state_t state,
                                        const char *message)
{
    portENTER_CRITICAL(&webradio_lock);
    webradio.playback_state = state;
    if (message != NULL) {
        strlcpy(webradio.playback_message,
                message,
                sizeof(webradio.playback_message));
    } else {
        webradio.playback_message[0] = '\0';
    }
    webradio.redraw = true;
    portEXIT_CRITICAL(&webradio_lock);
}

static void webradio_publish_source(
    const solar_os_stream_audio_format_t *source)
{
    portENTER_CRITICAL(&webradio_lock);
    if (source != NULL) {
        const bool format_changed =
            webradio.source_rate != source->sample_rate ||
            webradio.source_channels != source->channels;
        webradio.source_rate = source->sample_rate;
        webradio.source_channels = source->channels;
        if (format_changed) {
            webradio.redraw = true;
        }
    }
    portEXIT_CRITICAL(&webradio_lock);
}

static void webradio_publish_device(const solar_os_audio_device_info_t *device)
{
    portENTER_CRITICAL(&webradio_lock);
    if (device != NULL) {
        strlcpy(webradio.active_device_id,
                device->id,
                sizeof(webradio.active_device_id));
        webradio.active_device_capabilities = device->capabilities;
    } else {
        webradio.active_device_id[0] = '\0';
        webradio.active_device_capabilities = 0U;
    }
    portEXIT_CRITICAL(&webradio_lock);
}

static void webradio_publish_visualizer(const int16_t *samples,
                                        size_t sample_count,
                                        uint8_t channels)
{
    if (samples == NULL || channels == 0U) {
        return;
    }
    const size_t frames = sample_count / channels;
    if (webradio.scope != NULL) {
        (void)solar_os_oscilloscope_widget_submit_s16(
            webradio.scope, samples, frames, channels);
    }
    if (webradio.spectrum != NULL) {
        (void)solar_os_spectrum_widget_submit_s16(
            webradio.spectrum, samples, frames, channels);
    }
}

static void webradio_set_ui_message(const char *message)
{
    strlcpy(webradio.ui_message,
            message != NULL ? message : "",
            sizeof(webradio.ui_message));
    webradio.redraw = true;
}

static solar_os_shell_io_t *webradio_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&webradio.fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &webradio.fallback_io);
        io = &webradio.fallback_io;
    }
    return io;
}

static bool webradio_graphical_session(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = webradio_io(ctx);
    return solar_os_context_gfx(ctx) != NULL &&
        (io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT);
}

static void webradio_clip(char *destination,
                          size_t destination_len,
                          const char *source,
                          size_t width)
{
    if (destination == NULL || destination_len == 0U) {
        return;
    }
    if (source == NULL) {
        source = "";
    }
    const size_t limit = width < destination_len - 1U ? width : destination_len - 1U;
    const size_t length = strnlen(source, limit);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void webradio_refresh_catalog(void)
{
    webradio.station_count = solar_os_webradio_catalog_snapshot(
        webradio.stations,
        SOLAR_OS_WEBRADIO_STATION_MAX,
        &webradio.catalog_generation);
    if (webradio.station_count == 0U) {
        webradio.cursor = 0U;
        webradio.top = 0U;
    } else if (webradio.cursor >= webradio.station_count) {
        webradio.cursor = webradio.station_count - 1U;
    }
}

static void webradio_snapshot_status(webradio_playback_state_t *state,
                                     char *message,
                                     size_t message_len,
                                     char *active_name,
                                     size_t active_name_len,
                                     uint32_t *source_rate,
                                     uint8_t *source_channels,
                                     bool *redraw)
{
    portENTER_CRITICAL(&webradio_lock);
    if (state != NULL) {
        *state = webradio.playback_state;
    }
    if (message != NULL && message_len > 0U) {
        strlcpy(message, webradio.playback_message, message_len);
    }
    if (active_name != NULL && active_name_len > 0U) {
        strlcpy(active_name, webradio.active_name, active_name_len);
    }
    if (source_rate != NULL) {
        *source_rate = webradio.source_rate;
    }
    if (source_channels != NULL) {
        *source_channels = webradio.source_channels;
    }
    if (redraw != NULL) {
        *redraw = webradio.redraw;
        webradio.redraw = false;
    }
    portEXIT_CRITICAL(&webradio_lock);
}

static void webradio_render_tui(void)
{
    const size_t rows = solar_os_tui_rows(&webradio.tui);
    const size_t cols = solar_os_tui_cols(&webradio.tui);
    if (rows < 5U || cols < 20U) {
        solar_os_tui_draw_too_small(&webradio.tui, "webradio");
        solar_os_tui_refresh(&webradio.tui);
        return;
    }

    webradio_playback_state_t state;
    char message[96];
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    uint32_t sample_rate = 0U;
    uint8_t channels = 0U;
    webradio_snapshot_status(&state,
                             message,
                             sizeof(message),
                             active_name,
                             sizeof(active_name),
                             &sample_rate,
                             &channels,
                             NULL);

    solar_os_tui_clear(&webradio.tui);
    solar_os_tui_draw_title(&webradio.tui, "WebRadio", NULL);

    char status[192];
    if (state == WEBRADIO_PLAYBACK_PLAYING) {
        snprintf(status,
                 sizeof(status),
                 "%s: %s | %" PRIu32 " Hz, %u ch | volume %u%%",
                 webradio_state_name(state),
                 active_name,
                 sample_rate,
                 (unsigned)channels,
                 (unsigned)webradio.volume);
    } else {
        snprintf(status,
                 sizeof(status),
                 "%s%s%s | volume %u%%",
                 webradio_state_name(state),
                 message[0] != '\0' ? ": " : "",
                 message,
                 (unsigned)webradio.volume);
    }
    char clipped[192];
    webradio_clip(clipped, sizeof(clipped), status, cols > 2U ? cols - 2U : 0U);
    solar_os_tui_addstr(&webradio.tui,
                        1U,
                        1U,
                        clipped,
                        state == WEBRADIO_PLAYBACK_ERROR ?
                            SOLAR_OS_TUI_ATTR_BOLD : SOLAR_OS_TUI_ATTR_NORMAL);

    const bool editing = webradio.dialog != WEBRADIO_DIALOG_NONE;
    const size_t list_rows = editing ?
        (rows > 6U ? rows - 6U : 0U) : rows - 4U;
    if (webradio.cursor < webradio.top) {
        webradio.top = webradio.cursor;
    }
    if (webradio.cursor >= webradio.top + list_rows) {
        webradio.top = webradio.cursor - list_rows + 1U;
    }
    if (webradio.station_count == 0U) {
        solar_os_tui_addstr(&webradio.tui,
                            3U,
                            2U,
                            "Catalog is empty. Use: webradio add NAME URL",
                            SOLAR_OS_TUI_ATTR_NORMAL);
    } else {
        for (size_t row = 0U; row < list_rows; row++) {
            const size_t index = webradio.top + row;
            if (index >= webradio.station_count) {
                break;
            }
            char line[192];
            const bool active = active_name[0] != '\0' &&
                strcmp(active_name, webradio.stations[index].name) == 0 &&
                state != WEBRADIO_PLAYBACK_IDLE &&
                state != WEBRADIO_PLAYBACK_ERROR;
            snprintf(line,
                     sizeof(line),
                     "%c %-20s %s",
                     active ? '*' : ' ',
                     webradio.stations[index].name,
                     webradio.stations[index].url);
            webradio_clip(clipped,
                          sizeof(clipped),
                          line,
                          cols > 2U ? cols - 2U : 0U);
            solar_os_tui_addstr(&webradio.tui,
                                row + 3U,
                                1U,
                                clipped,
                                index == webradio.cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE :
                                    SOLAR_OS_TUI_ATTR_NORMAL);
        }
    }
    if (editing) {
        char editor_label[192];
        snprintf(editor_label,
                 sizeof(editor_label),
                 "%s%s%s",
                 webradio_dialog_label(),
                 webradio.ui_message[0] != '\0' ? " - " : "",
                 webradio.ui_message);
        webradio_clip(clipped,
                      sizeof(clipped),
                      editor_label,
                      cols > 2U ? cols - 2U : 0U);
        solar_os_tui_addstr(&webradio.tui,
                            rows - 3U,
                            1U,
                            clipped,
                            SOLAR_OS_TUI_ATTR_BOLD);

        const size_t visible_chars = cols > 4U ? cols - 4U : 1U;
        const size_t input_length = strlen(webradio.dialog_input);
        const char *visible = input_length > visible_chars ?
            webradio.dialog_input + input_length - visible_chars :
            webradio.dialog_input;
        solar_os_tui_addstr(&webradio.tui,
                            rows - 2U,
                            1U,
                            "> ",
                            SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_addstr(&webradio.tui,
                            rows - 2U,
                            3U,
                            visible,
                            SOLAR_OS_TUI_ATTR_NORMAL);
        size_t cursor_col = 3U + strlen(visible);
        if (cursor_col >= cols) {
            cursor_col = cols - 1U;
        }
        solar_os_tui_move(&webradio.tui, rows - 2U, cursor_col);
        solar_os_tui_set_cursor_visible(&webradio.tui, true);
    } else {
        if (webradio.ui_message[0] != '\0') {
            webradio_clip(clipped,
                          sizeof(clipped),
                          webradio.ui_message,
                          cols > 2U ? cols - 2U : 0U);
            solar_os_tui_addstr(&webradio.tui,
                                2U,
                                1U,
                                clipped,
                                SOLAR_OS_TUI_ATTR_BOLD);
        }
        solar_os_tui_set_cursor_visible(&webradio.tui, false);
    }

    const char *controls = editing ?
        "Enter next/save  Esc cancel" :
        "Up/Down select  +/- volume  A add  E edit  Del remove  Enter play  Space stop";
    solar_os_tui_draw_help(&webradio.tui, controls);
    solar_os_tui_refresh(&webradio.tui);
}

static void webradio_draw_centered(solar_os_gfx_t *gfx,
                                   int width,
                                   int baseline,
                                   const char *text)
{
    const int text_width = (int)solar_os_gfx_text_width(gfx, text);
    solar_os_gfx_text(gfx, (width - text_width) / 2, baseline, text);
}

static void webradio_draw_graphics_header(solar_os_gfx_t *gfx, int width)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, WEBRADIO_GUI_HEADER_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, 7, 19, "WebRadio");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    const char *tabs = webradio.tab == WEBRADIO_TAB_PLAYER ?
        "[PLAYER]  CHANNELS" : "PLAYER  [CHANNELS]";
    const int tabs_width = (int)solar_os_gfx_text_width(gfx, tabs);
    solar_os_gfx_text(gfx, width - tabs_width - 7, 18, tabs);
}

static void webradio_draw_visualizer(solar_os_gfx_t *gfx,
                                     int width,
                                     int height)
{
    const int player_top = (height * 2) / 3;
    const int visualizer_y = WEBRADIO_GUI_HEADER_HEIGHT + 4;
    const int visualizer_height = player_top - visualizer_y - 4;
    if (webradio.visualizer == WEBRADIO_VISUALIZER_SPECTRUM) {
        solar_os_spectrum_widget_draw(
            webradio.spectrum, gfx, 5, visualizer_y, width - 10, visualizer_height);
    } else {
        solar_os_oscilloscope_widget_draw(
            webradio.scope, gfx, 5, visualizer_y, width - 10, visualizer_height);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 9, visualizer_y + 4, 78, 15);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx,
                      13,
                      visualizer_y + 15,
                      webradio.visualizer == WEBRADIO_VISUALIZER_SPECTRUM ?
                          "SPECTRUM  V" : "SCOPE  V");
}

static void webradio_render_player(solar_os_gfx_t *gfx,
                                   int width,
                                   int height)
{
    webradio_playback_state_t state;
    char message[96];
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    uint32_t sample_rate = 0U;
    uint8_t channels = 0U;
    webradio_snapshot_status(&state,
                             message,
                             sizeof(message),
                             active_name,
                             sizeof(active_name),
                             &sample_rate,
                             &channels,
                             NULL);

    const int player_top = (height * 2) / 3;
    webradio_draw_visualizer(gfx, width, height);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 0, player_top, width - 1, player_top);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    webradio_draw_centered(gfx,
                           width,
                           player_top + 20,
                           active_name[0] != '\0' ? active_name :
                                                   "No channel selected");

    char status[96];
    if (state == WEBRADIO_PLAYBACK_PLAYING) {
        snprintf(status,
                 sizeof(status),
                 "playing  %" PRIu32 " Hz  %u ch",
                 sample_rate,
                 (unsigned)channels);
    } else {
        snprintf(status,
                 sizeof(status),
                 "%s%s%s",
                 webradio_state_name(state),
                 message[0] != '\0' ? " - " : "",
                 message);
    }
    char clipped[96];
    webradio_clip(clipped, sizeof(clipped), status, (size_t)(width / 7));
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    webradio_draw_centered(gfx, width, player_top + 36, clipped);

    const int volume_width = width / 2;
    const int volume_height = 10;
    const int volume_x = (width - volume_width) / 2;
    const int volume_y = player_top + 43;
    const int volume_fill =
        ((volume_width - 4) * (int)webradio.volume) / 100;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, volume_x - 29, volume_y + 9, "VOL");
    solar_os_gfx_rect(gfx,
                      volume_x,
                      volume_y,
                      volume_width,
                      volume_height);
    if (volume_fill > 0) {
        solar_os_gfx_fill_rect(gfx,
                               volume_x + 2,
                               volume_y + 2,
                               volume_fill,
                               volume_height - 4);
    }
    char volume_percent[8];
    snprintf(volume_percent,
             sizeof(volume_percent),
             "%u%%",
             (unsigned)webradio.volume);
    solar_os_gfx_text(gfx,
                      volume_x + volume_width + 5,
                      volume_y + 9,
                      volume_percent);

    const int button_y = height - 25;
    const int gap = 5;
    const int button_width = (width - 4 * gap) / 3;
    const bool stopped = state == WEBRADIO_PLAYBACK_IDLE ||
                         state == WEBRADIO_PLAYBACK_ERROR;
    solar_os_media_transport_button_draw(
        gfx, gap, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_PREVIOUS, false);
    solar_os_media_transport_button_draw(
        gfx, gap * 2 + button_width, button_y, button_width, 21,
        stopped ? SOLAR_OS_MEDIA_TRANSPORT_PLAY :
                  SOLAR_OS_MEDIA_TRANSPORT_STOP,
        false);
    solar_os_media_transport_button_draw(
        gfx, gap * 3 + button_width * 2, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_NEXT, false);
}

static const char *webradio_dialog_label(void)
{
    switch (webradio.dialog) {
    case WEBRADIO_DIALOG_ADD_NAME:
        return "Add channel - name";
    case WEBRADIO_DIALOG_ADD_URL:
        return "Add channel - stream URL";
    case WEBRADIO_DIALOG_EDIT_NAME:
        return "Edit channel - name";
    case WEBRADIO_DIALOG_EDIT_URL:
        return "Edit channel - stream URL";
    case WEBRADIO_DIALOG_NONE:
    default:
        return "";
    }
}

static void webradio_draw_dialog(solar_os_gfx_t *gfx, int width, int height)
{
    if (webradio.dialog == WEBRADIO_DIALOG_NONE) {
        return;
    }
    const int dialog_width = width - 24;
    const int dialog_height = 76;
    const int x = 12;
    const int y = (height - dialog_height) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, dialog_width, dialog_height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, dialog_width, dialog_height);
    solar_os_gfx_rect(gfx, x + 2, y + 2, dialog_width - 4, dialog_height - 4);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_gfx_text(gfx, x + 9, y + 20, webradio_dialog_label());
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    char input[WEBRADIO_GUI_INPUT_MAX + 2U];
    snprintf(input, sizeof(input), "%s_", webradio.dialog_input);
    const size_t max_chars = dialog_width > 26 ?
        (size_t)((dialog_width - 26) / 7) : 1U;
    const size_t length = strlen(input);
    const char *visible = length > max_chars ? input + length - max_chars : input;
    solar_os_gfx_text(gfx, x + 9, y + 43, visible);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, x + 9, y + 64, "Enter next/save   Esc cancel");
}

static void webradio_render_channels(solar_os_gfx_t *gfx,
                                     int width,
                                     int height,
                                     webradio_playback_state_t state,
                                     const char *active_name)
{
    const int list_y = WEBRADIO_GUI_HEADER_HEIGHT + 5;
    const int list_bottom = height - WEBRADIO_GUI_FOOTER_HEIGHT - 16;
    const size_t visible_rows = list_bottom > list_y ?
        (size_t)((list_bottom - list_y) / WEBRADIO_GUI_ROW_HEIGHT) : 0U;
    if (webradio.cursor < webradio.top) {
        webradio.top = webradio.cursor;
    }
    if (visible_rows > 0U && webradio.cursor >= webradio.top + visible_rows) {
        webradio.top = webradio.cursor - visible_rows + 1U;
    }

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    for (size_t row = 0U; row < visible_rows; row++) {
        const size_t index = webradio.top + row;
        if (index >= webradio.station_count) {
            break;
        }
        const int row_y = list_y + (int)row * WEBRADIO_GUI_ROW_HEIGHT;
        if (index == webradio.cursor) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx,
                                   6,
                                   row_y,
                                   width - 12,
                                   WEBRADIO_GUI_ROW_HEIGHT - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
        const bool active = active_name[0] != '\0' &&
            strcmp(active_name, webradio.stations[index].name) == 0 &&
            state != WEBRADIO_PLAYBACK_IDLE && state != WEBRADIO_PLAYBACK_ERROR;
        solar_os_gfx_text(gfx, 14, row_y + 17, active ? ">" : " ");
        solar_os_gfx_text(gfx, 30, row_y + 17, webradio.stations[index].name);
    }
    if (webradio.station_count == 0U) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_text(gfx, 14, list_y + 17, "Catalog is empty");
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    if (webradio.ui_message[0] != '\0') {
        char clipped[96];
        webradio_clip(clipped,
                      sizeof(clipped),
                      webradio.ui_message,
                      (size_t)(width / 6));
        solar_os_gfx_text(gfx, 8, height - WEBRADIO_GUI_FOOTER_HEIGHT - 3, clipped);
    }
    solar_os_gfx_text(gfx,
                      8,
                      height - 5,
                      "A add  E edit  Del remove  Enter select");
    webradio_draw_dialog(gfx, width, height);
}

static void webradio_render_graphics(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    webradio_draw_graphics_header(gfx, width);
    if (webradio.tab == WEBRADIO_TAB_PLAYER) {
        webradio_render_player(gfx, width, height);
    } else {
        webradio_playback_state_t state;
        char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
        webradio_snapshot_status(&state,
                                 NULL,
                                 0U,
                                 active_name,
                                 sizeof(active_name),
                                 NULL,
                                 NULL,
                                 NULL);
        webradio_render_channels(gfx, width, height, state, active_name);
    }
    solar_os_gfx_present(gfx);
}

static void webradio_render_visualizer(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || webradio.suspended ||
        webradio.mode != WEBRADIO_MODE_GRAPHICS ||
        webradio.tab != WEBRADIO_TAB_PLAYER) {
        return;
    }
    webradio_draw_visualizer(gfx,
                             (int)solar_os_gfx_width(gfx),
                             (int)solar_os_gfx_height(gfx));
    solar_os_gfx_present(gfx);
}

static void webradio_render(solar_os_context_t *ctx)
{
    if (webradio.suspended) {
        return;
    }
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        webradio_render_graphics(ctx);
    } else {
        webradio_render_tui();
    }
}

static void webradio_consume_input(webradio_worker_t *worker, size_t consumed)
{
    if (consumed >= worker->input_len) {
        worker->input_len = 0U;
        return;
    }
    memmove(worker->input,
            worker->input + consumed,
            worker->input_len - consumed);
    worker->input_len -= consumed;
}

static esp_err_t webradio_playback_flush(webradio_worker_t *worker, bool pad_tail)
{
    if (worker->playback_samples == 0U) {
        return ESP_OK;
    }
    if (pad_tail) {
        const size_t frames_per_block =
            worker->output_format.frames_per_block != 0U ?
                worker->output_format.frames_per_block : 256U;
        const size_t quantum =
            frames_per_block * worker->output_format.channels;
        size_t padded =
            ((worker->playback_samples + quantum - 1U) / quantum) * quantum;
        if (padded > WEBRADIO_OUTPUT_SAMPLES) {
            padded = WEBRADIO_OUTPUT_SAMPLES;
        }
        while (worker->playback_samples < padded) {
            worker->playback[worker->playback_samples++] = 0;
        }
    }
    const size_t bytes = worker->playback_samples * sizeof(worker->playback[0]);
    const esp_err_t err = solar_os_audio_player_write(
        worker->player,
        worker->playback,
        bytes,
        &webradio.stop_requested);
    if (err != ESP_OK) {
        worker->playback_error = err;
        webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                    "audio output failed");
        return err;
    }
    worker->playback_samples = 0U;
    return ESP_OK;
}

static esp_err_t webradio_playback_append(webradio_worker_t *worker,
                                          const int16_t *samples,
                                          size_t sample_count)
{
    while (sample_count > 0U) {
        if (worker->playback_samples == WEBRADIO_OUTPUT_SAMPLES) {
            const esp_err_t err = webradio_playback_flush(worker, false);
            if (err != ESP_OK) {
                return err;
            }
        }
        const size_t space = WEBRADIO_OUTPUT_SAMPLES - worker->playback_samples;
        const size_t count = sample_count < space ? sample_count : space;
        memcpy(worker->playback + worker->playback_samples,
               samples,
               count * sizeof(samples[0]));
        worker->playback_samples += count;
        samples += count;
        sample_count -= count;
    }
    return ESP_OK;
}

static esp_err_t webradio_decode_available(webradio_worker_t *worker)
{
    while (worker->input_len >= SOLAR_OS_AUDIO_MP3_STREAM_WINDOW_BYTES &&
           !webradio.stop_requested) {
        solar_os_audio_decoded_frame_t frame;
        size_t consumed = 0U;
        esp_err_t err = solar_os_audio_mp3_decode(
            worker->decoder,
            worker->input,
            worker->input_len,
            &consumed,
            worker->decoded,
            SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES,
            &frame);
        if (err != ESP_OK) {
            return err;
        }

        if (frame.frames > 0U) {
            const bool first_frame = !worker->decoded_any;
            if (first_frame) {
                worker->decoded_any = true;
                if (worker->playback_started) {
                    webradio_set_playback_state(WEBRADIO_PLAYBACK_PLAYING, NULL);
                }
            }
            bool source_done = false;
            do {
                size_t output_samples = 0U;
                err = solar_os_audio_s16_convert(
                    &worker->converter,
                    worker->decoded,
                    frame.frames,
                    &frame.format,
                    &worker->output_format,
                    worker->output,
                    WEBRADIO_OUTPUT_SAMPLES,
                    &output_samples,
                    &source_done);
                if (err != ESP_OK) {
                    return err;
                }
                err = webradio_playback_append(worker,
                                               worker->output,
                                               output_samples);
                if (err != ESP_OK) {
                    return err;
                }
                worker->output_bytes += output_samples * sizeof(worker->output[0]);
            } while (!source_done && !webradio.stop_requested);
            if (first_frame) {
                webradio_publish_source(&frame.format);
            }
        }

        if (consumed == 0U) {
            break;
        }
        webradio_consume_input(worker, consumed);
    }
    return webradio.stop_requested ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t webradio_feed_mp3(webradio_worker_t *worker,
                                   const uint8_t *data,
                                   size_t length)
{
    while (length > 0U && !webradio.stop_requested) {
        if (worker->input_len == WEBRADIO_INPUT_BYTES) {
            esp_err_t err = webradio_decode_available(worker);
            if (err != ESP_OK) {
                return err;
            }
            if (worker->input_len == WEBRADIO_INPUT_BYTES) {
                webradio_consume_input(worker, 1U);
            }
        }
        const size_t space = WEBRADIO_INPUT_BYTES - worker->input_len;
        const size_t count = length < space ? length : space;
        memcpy(worker->input + worker->input_len, data, count);
        worker->input_len += count;
        worker->network_bytes += count;
        data += count;
        length -= count;

        const esp_err_t err = webradio_decode_available(worker);
        if (err != ESP_OK) {
            return err;
        }
        if (!worker->decoded_any && worker->network_bytes >= WEBRADIO_INVALID_STREAM_BYTES) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return webradio.stop_requested ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t webradio_http_event(const solar_os_http_event_t *event,
                                     void *user_data)
{
    webradio_worker_t *worker = user_data;
    if (event == NULL || worker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (webradio.stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) {
        if (event->header_name != NULL && event->header_value != NULL &&
            strcasecmp(event->header_name, "Content-Type") == 0) {
            strlcpy(worker->content_type,
                    event->header_value,
                    sizeof(worker->content_type));
        }
        return ESP_OK;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA) {
        return ESP_OK;
    }
    if (event->status_code < 200 || event->status_code >= 300) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strncasecmp(worker->content_type, "text/", 5U) == 0 ||
        strncasecmp(worker->content_type, "application/json", 16U) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return webradio_feed_mp3(worker, event->data, event->data_len);
}

static void webradio_player_state(bool playing, void *user)
{
    webradio_worker_t *worker = user;
    worker->playback_started = playing;
    if (playing) {
        webradio_set_playback_state(WEBRADIO_PLAYBACK_PLAYING, NULL);
    } else if (!webradio.stop_requested) {
        worker->playback_error = solar_os_audio_player_error(worker->player);
        webradio_set_playback_state(
            worker->playback_error == ESP_OK ? WEBRADIO_PLAYBACK_BUFFERING :
                                               WEBRADIO_PLAYBACK_ERROR,
            worker->playback_error == ESP_OK ? NULL : "audio output failed");
    }
}

static void webradio_player_samples(const int16_t *samples,
                                    size_t sample_count,
                                    uint8_t channels,
                                    void *user)
{
    (void)user;
    webradio_publish_visualizer(samples, sample_count, channels);
}

static void webradio_worker_free(webradio_worker_t *worker)
{
    solar_os_audio_player_destroy(worker->player);
    webradio_publish_device(NULL);
    solar_os_audio_mp3_decoder_destroy(worker->decoder);
    solar_os_memory_free(worker->input);
    solar_os_memory_free(worker->decoded);
    solar_os_memory_free(worker->output);
    solar_os_memory_free(worker->playback);
    memset(worker, 0, sizeof(*worker));
}

static esp_err_t webradio_worker_init(webradio_worker_t *worker)
{
    memset(worker, 0, sizeof(*worker));
    worker->playback_error = ESP_OK;
    worker->input = solar_os_memory_alloc(WEBRADIO_INPUT_BYTES,
                                           SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "webradio.input");
    worker->decoded = solar_os_memory_alloc(
        SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES * sizeof(worker->decoded[0]),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "webradio.decoded");
    worker->output = solar_os_memory_alloc(WEBRADIO_PCM_BUFFER_BYTES,
                                            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                            "webradio.output");
    worker->playback = solar_os_memory_alloc(WEBRADIO_PCM_BUFFER_BYTES,
                                              SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                              "webradio.playback");
    if (worker->input == NULL || worker->decoded == NULL ||
        worker->output == NULL || worker->playback == NULL) {
        webradio_worker_free(worker);
        return ESP_ERR_NO_MEM;
    }

    const solar_os_audio_player_options_t options = {
        .owner = "webradio",
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
        .volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL,
        .buffered = true,
        .external_buffer_bytes = WEBRADIO_JITTER_EXTERNAL_BYTES,
        .internal_buffer_bytes = WEBRADIO_JITTER_INTERNAL_BYTES,
        .target_ms = WEBRADIO_JITTER_TARGET_MS,
        .state = webradio_player_state,
        .samples = webradio_player_samples,
        .user = worker,
    };
    solar_os_audio_device_info_t device;
    const esp_err_t err = solar_os_audio_player_create(
        &options,
        &worker->player,
        &worker->output_format,
        &device);
    if (err != ESP_OK) {
        webradio_worker_free(worker);
        return err;
    }
    webradio_publish_device(&device);
    return ESP_OK;
}

static void webradio_network_task(void *arg)
{
    (void)arg;
    webradio_worker_t worker;
    esp_err_t err = webradio_worker_init(&worker);
    if (err != ESP_OK) {
        webradio_set_playback_state(
            WEBRADIO_PLAYBACK_ERROR,
            err == ESP_ERR_NOT_FOUND ? "no audio output device" :
            err == ESP_ERR_NO_MEM ? "not enough memory" :
            "audio output unavailable");
        goto done;
    }

    const solar_os_http_header_t headers[] = {
        {"Accept", "audio/mpeg, audio/mp3, application/octet-stream"},
    };
    while (!webradio.stop_requested) {
        worker.input_len = 0U;
        worker.playback_samples = 0U;
        worker.decoded_any = false;
        worker.content_type[0] = '\0';
        solar_os_audio_s16_converter_reset(&worker.converter);
        solar_os_audio_mp3_decoder_destroy(worker.decoder);
        worker.decoder = NULL;
        err = solar_os_audio_mp3_decoder_create(&worker.decoder);
        if (err != ESP_OK) {
            webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                        "decoder allocation failed");
            break;
        }

        webradio_set_playback_state(WEBRADIO_PLAYBACK_CONNECTING, NULL);
        const solar_os_http_request_options_t options = {
            .url = webradio.active_url,
            .method = SOLAR_OS_HTTP_METHOD_GET,
            .headers = headers,
            .header_count = sizeof(headers) / sizeof(headers[0]),
            .user_agent = "SolarOS-WebRadio/0.1",
            .follow_redirects = true,
            .max_redirects = 5U,
            .timeout_ms = WEBRADIO_HTTP_TIMEOUT_MS,
            .read_poll_ms = WEBRADIO_HTTP_READ_POLL_MS,
            .cancel_flag = &webradio.stop_requested,
            .receive_buffer_size = 2048U,
            .transmit_buffer_size = 512U,
            .event_handler = webradio_http_event,
            .user_data = &worker,
        };
        solar_os_http_request_t *request = NULL;
        err = solar_os_http_request_create(&options, &request);
        if (err != ESP_OK) {
            webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                        "HTTP client allocation failed");
            break;
        }
        webradio_set_playback_state(WEBRADIO_PLAYBACK_BUFFERING, NULL);
        solar_os_http_response_t response;
        err = solar_os_http_request_perform(request, &response);
        (void)solar_os_http_request_destroy(request);

        (void)webradio_playback_flush(&worker, true);
        if (webradio.stop_requested) {
            break;
        }
        worker.playback_error = solar_os_audio_player_error(worker.player);
        if (worker.playback_error != ESP_OK) {
            break;
        }
        const bool http_rejected = response.status_code >= 0 &&
            (response.status_code < 200 || response.status_code >= 300);
        if (err == ESP_ERR_NOT_SUPPORTED ||
            (!worker.decoded_any && err == ESP_OK) ||
            http_rejected) {
            webradio_set_playback_state(
                WEBRADIO_PLAYBACK_ERROR,
                err == ESP_ERR_NOT_SUPPORTED ? "URL is not an MP3 stream" :
                "stream returned no playable MP3 audio");
            break;
        }

        char reconnect_message[96];
        snprintf(reconnect_message,
                 sizeof(reconnect_message),
                 "connection ended (%s)",
                 esp_err_to_name(err));
        webradio_set_playback_state(WEBRADIO_PLAYBACK_RECONNECTING,
                                    reconnect_message);
        for (uint32_t waited = 0U;
             waited < WEBRADIO_RECONNECT_DELAY_MS && !webradio.stop_requested;
             waited += 50U) {
            vTaskDelay(pdMS_TO_TICKS(50U));
        }
    }
    webradio_worker_free(&worker);

done:
    if (webradio.stop_requested) {
        webradio_set_playback_state(WEBRADIO_PLAYBACK_IDLE, NULL);
    }
    webradio.task_done = true;
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void webradio_stop_playback(void)
{
    webradio.stop_requested = true;
    TaskHandle_t task = webradio.task;
    if (task != NULL &&
        !solar_os_task_wait_done(task,
                                 &webradio.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "radio task did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        while (!webradio.task_done) {
            vTaskDelay(pdMS_TO_TICKS(WEBRADIO_WORKER_POLL_MS));
        }
    }
    if (task != NULL) {
        solar_os_task_delete_external(task);
    }
    webradio.task = NULL;
    webradio.task_done = true;
    webradio.stop_requested = false;
    webradio_set_playback_state(WEBRADIO_PLAYBACK_IDLE, NULL);
}

static void webradio_reap_finished_task(void)
{
    TaskHandle_t task = webradio.task;
    if (task == NULL || !webradio.task_done) {
        return;
    }
    solar_os_task_delete_external(task);
    webradio.task = NULL;
}

static esp_err_t webradio_start_playback(const char *name, const char *url)
{
    if (name == NULL || url == NULL || !solar_os_webradio_url_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    webradio_stop_playback();
    solar_os_oscilloscope_widget_reset(webradio.scope);
    solar_os_spectrum_widget_reset(webradio.spectrum);
    portENTER_CRITICAL(&webradio_lock);
    strlcpy(webradio.active_name, name, sizeof(webradio.active_name));
    strlcpy(webradio.active_url, url, sizeof(webradio.active_url));
    webradio.source_rate = 0U;
    webradio.source_channels = 0U;
    webradio.active_device_id[0] = '\0';
    webradio.active_device_capabilities = 0U;
    webradio.stop_requested = false;
    webradio.task_done = false;
    webradio.redraw = true;
    portEXIT_CRITICAL(&webradio_lock);

    const BaseType_t created = solar_os_task_create_pinned_external(
        webradio_network_task,
        "webradio_net",
        WEBRADIO_TASK_STACK,
        NULL,
        WEBRADIO_TASK_PRIORITY,
        &webradio.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        webradio.task = NULL;
        webradio.task_done = true;
        webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                    "task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void webradio_play_selected(void)
{
    if (webradio.cursor >= webradio.station_count) {
        return;
    }
    (void)webradio_start_playback(webradio.stations[webradio.cursor].name,
                                  webradio.stations[webradio.cursor].url);
}

static void webradio_play_catalog_offset(int direction)
{
    if (webradio.station_count == 0U) {
        webradio_set_ui_message("Catalog is empty");
        return;
    }
    char active_url[SOLAR_OS_WEBRADIO_URL_MAX];
    portENTER_CRITICAL(&webradio_lock);
    strlcpy(active_url, webradio.active_url, sizeof(active_url));
    portEXIT_CRITICAL(&webradio_lock);

    size_t index = webradio.cursor < webradio.station_count ?
        webradio.cursor : 0U;
    for (size_t i = 0U; i < webradio.station_count; i++) {
        if (active_url[0] != '\0' &&
            strcmp(active_url, webradio.stations[i].url) == 0) {
            index = i;
            break;
        }
    }
    if (direction < 0) {
        index = index == 0U ? webradio.station_count - 1U : index - 1U;
    } else {
        index = (index + 1U) % webradio.station_count;
    }
    webradio.cursor = index;
    webradio_play_selected();
}

static void webradio_toggle_playback(void)
{
    webradio_playback_state_t state;
    webradio_snapshot_status(&state, NULL, 0U, NULL, 0U, NULL, NULL, NULL);
    if (state != WEBRADIO_PLAYBACK_IDLE && state != WEBRADIO_PLAYBACK_ERROR) {
        webradio_stop_playback();
        return;
    }

    char name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    char url[SOLAR_OS_WEBRADIO_URL_MAX];
    portENTER_CRITICAL(&webradio_lock);
    strlcpy(name, webradio.active_name, sizeof(name));
    strlcpy(url, webradio.active_url, sizeof(url));
    portEXIT_CRITICAL(&webradio_lock);
    if (url[0] != '\0') {
        (void)webradio_start_playback(name, url);
    } else {
        webradio_play_selected();
    }
}

static void webradio_adjust_volume(int direction)
{
    int volume = (int)webradio.volume + direction * (int)WEBRADIO_VOLUME_STEP;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    esp_err_t err = solar_os_audio_set_volume((uint8_t)volume);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        char device_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
        uint32_t capabilities;
        portENTER_CRITICAL(&webradio_lock);
        strlcpy(device_id,
                webradio.active_device_id,
                sizeof(device_id));
        capabilities = webradio.active_device_capabilities;
        portEXIT_CRITICAL(&webradio_lock);
        if (device_id[0] != '\0' &&
            (capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0U) {
            err = solar_os_audio_set_device_volume(device_id, (uint8_t)volume);
        }
    }
    if (err == ESP_OK) {
        webradio.volume = (uint8_t)volume;
        webradio_set_ui_message(NULL);
    } else {
        webradio_set_ui_message("Volume control unavailable");
    }
}

static size_t webradio_find_station(const char *name)
{
    for (size_t i = 0U; i < webradio.station_count; i++) {
        if (strcmp(webradio.stations[i].name, name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void webradio_dialog_set_input(const char *value)
{
    strlcpy(webradio.dialog_input,
            value != NULL ? value : "",
            sizeof(webradio.dialog_input));
    webradio.dialog_input_len = strlen(webradio.dialog_input);
}

static void webradio_start_add_dialog(void)
{
    if (webradio.station_count >= SOLAR_OS_WEBRADIO_STATION_MAX) {
        webradio_set_ui_message("Catalog is full");
        return;
    }
    webradio.edit_original_name[0] = '\0';
    webradio.edit_name[0] = '\0';
    webradio.edit_url[0] = '\0';
    webradio.dialog = WEBRADIO_DIALOG_ADD_NAME;
    webradio_dialog_set_input(NULL);
    webradio_set_ui_message(NULL);
}

static void webradio_start_edit_dialog(void)
{
    if (webradio.cursor >= webradio.station_count) {
        return;
    }
    strlcpy(webradio.edit_original_name,
            webradio.stations[webradio.cursor].name,
            sizeof(webradio.edit_original_name));
    strlcpy(webradio.edit_name,
            webradio.stations[webradio.cursor].name,
            sizeof(webradio.edit_name));
    strlcpy(webradio.edit_url,
            webradio.stations[webradio.cursor].url,
            sizeof(webradio.edit_url));
    webradio.dialog = WEBRADIO_DIALOG_EDIT_NAME;
    webradio_dialog_set_input(webradio.edit_name);
    webradio_set_ui_message(NULL);
}

static void webradio_cancel_dialog(void)
{
    webradio.dialog = WEBRADIO_DIALOG_NONE;
    webradio.dialog_input[0] = '\0';
    webradio.dialog_input_len = 0U;
    webradio.redraw = true;
}

static void webradio_finish_dialog(void)
{
    esp_err_t err;
    bool update_active = false;
    bool restart_active = false;
    if (webradio.edit_original_name[0] == '\0') {
        err = solar_os_webradio_catalog_add(
            webradio.edit_name, webradio.edit_url);
    } else {
        webradio_playback_state_t playback_state;
        char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
        webradio_snapshot_status(&playback_state,
                                 NULL,
                                 0U,
                                 active_name,
                                 sizeof(active_name),
                                 NULL,
                                 NULL,
                                 NULL);
        update_active = strcmp(active_name, webradio.edit_original_name) == 0;
        restart_active = update_active &&
            playback_state != WEBRADIO_PLAYBACK_IDLE &&
            playback_state != WEBRADIO_PLAYBACK_ERROR;
        err = solar_os_webradio_catalog_update(webradio.edit_original_name,
                                               webradio.edit_name,
                                               webradio.edit_url);
    }

    if (err == ESP_OK) {
        webradio_refresh_catalog();
        const size_t index = webradio_find_station(webradio.edit_name);
        if (index != SIZE_MAX) {
            webradio.cursor = index;
        }
        webradio_set_ui_message("Channel saved");
        if (restart_active) {
            webradio_stop_playback();
        }
        if (update_active) {
            portENTER_CRITICAL(&webradio_lock);
            strlcpy(webradio.active_name,
                    webradio.edit_name,
                    sizeof(webradio.active_name));
            strlcpy(webradio.active_url,
                    webradio.edit_url,
                    sizeof(webradio.active_url));
            portEXIT_CRITICAL(&webradio_lock);
        }
        if (restart_active) {
            webradio_play_selected();
        }
        webradio_cancel_dialog();
    } else {
        char message[96];
        snprintf(message,
                 sizeof(message),
                 "Cannot save channel: %s",
                 esp_err_to_name(err));
        webradio_set_ui_message(message);
    }
}

static bool webradio_handle_dialog_key(uint8_t key)
{
    if (webradio.dialog == WEBRADIO_DIALOG_NONE) {
        return false;
    }
    if (key == SOLAR_OS_KEY_ESCAPE) {
        webradio_cancel_dialog();
        return true;
    }
    if (key == '\b' || key == 0x7fU || key == SOLAR_OS_KEY_DELETE) {
        if (webradio.dialog_input_len > 0U) {
            webradio.dialog_input[--webradio.dialog_input_len] = '\0';
        }
        webradio.redraw = true;
        return true;
    }
    if (key == '\r' || key == '\n') {
        if (webradio.dialog_input_len == 0U) {
            webradio_set_ui_message("Value cannot be empty");
            return true;
        }
        switch (webradio.dialog) {
        case WEBRADIO_DIALOG_ADD_NAME:
            strlcpy(webradio.edit_name,
                    webradio.dialog_input,
                    sizeof(webradio.edit_name));
            webradio.dialog = WEBRADIO_DIALOG_ADD_URL;
            webradio_dialog_set_input(NULL);
            break;
        case WEBRADIO_DIALOG_EDIT_NAME:
            strlcpy(webradio.edit_name,
                    webradio.dialog_input,
                    sizeof(webradio.edit_name));
            webradio.dialog = WEBRADIO_DIALOG_EDIT_URL;
            webradio_dialog_set_input(webradio.edit_url);
            break;
        case WEBRADIO_DIALOG_ADD_URL:
        case WEBRADIO_DIALOG_EDIT_URL:
            strlcpy(webradio.edit_url,
                    webradio.dialog_input,
                    sizeof(webradio.edit_url));
            webradio_finish_dialog();
            break;
        case WEBRADIO_DIALOG_NONE:
        default:
            break;
        }
        webradio.redraw = true;
        return true;
    }

    const size_t limit =
        (webradio.dialog == WEBRADIO_DIALOG_ADD_NAME ||
         webradio.dialog == WEBRADIO_DIALOG_EDIT_NAME) ?
        SOLAR_OS_WEBRADIO_STATION_NAME_MAX - 1U :
        SOLAR_OS_WEBRADIO_URL_MAX - 1U;
    if ((isprint(key) || key >= 0xa0U) &&
        webradio.dialog_input_len < limit) {
        webradio.dialog_input[webradio.dialog_input_len++] = (char)key;
        webradio.dialog_input[webradio.dialog_input_len] = '\0';
        webradio.redraw = true;
    }
    return true;
}

static void webradio_remove_selected(void)
{
    if (webradio.cursor >= webradio.station_count) {
        return;
    }
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    webradio_snapshot_status(NULL,
                             NULL,
                             0U,
                             active_name,
                             sizeof(active_name),
                             NULL,
                             NULL,
                             NULL);
    if (strcmp(active_name, webradio.stations[webradio.cursor].name) == 0) {
        webradio_stop_playback();
        portENTER_CRITICAL(&webradio_lock);
        webradio.active_name[0] = '\0';
        webradio.active_url[0] = '\0';
        portEXIT_CRITICAL(&webradio_lock);
    }
    const esp_err_t err = solar_os_webradio_catalog_remove(
        webradio.stations[webradio.cursor].name);
    webradio_refresh_catalog();
    webradio_set_ui_message(err == ESP_OK ? "Channel removed" :
                                            "Cannot remove channel");
    portENTER_CRITICAL(&webradio_lock);
    webradio.redraw = true;
    portEXIT_CRITICAL(&webradio_lock);
}

static bool webradio_manage_command(solar_os_context_t *ctx,
                                    int argc,
                                    const char *const *argv,
                                    esp_err_t *result)
{
    if (argc < 1) {
        return false;
    }
    solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
    const char *command = argv[0];
    solar_os_shell_io_t *io = webradio_io(ctx);
    esp_err_t err = ESP_OK;
    bool handled = true;
    if (strcmp(command, "list") == 0 && argc == 1) {
        solar_os_webradio_station_t stations[SOLAR_OS_WEBRADIO_STATION_MAX];
        const size_t count = solar_os_webradio_catalog_snapshot(
            stations, SOLAR_OS_WEBRADIO_STATION_MAX, NULL);
        for (size_t i = 0U; i < count; i++) {
            solar_os_shell_io_printf(io, "%s\t%s\n", stations[i].name, stations[i].url);
        }
        if (count == 0U) {
            solar_os_shell_io_writeln(io, "webradio: catalog is empty");
        }
    } else if (strcmp(command, "add") == 0 && argc == 3) {
        err = solar_os_webradio_catalog_add(argv[1], argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "webradio: saved %s\n",
                                     argv[1]);
        }
    } else if (strcmp(command, "remove") == 0 && argc == 2) {
        err = solar_os_webradio_catalog_remove(argv[1]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "webradio: removed %s\n",
                                     argv[1]);
        }
    } else if (strcmp(command, "reset") == 0 && argc == 1) {
        err = solar_os_webradio_catalog_reset();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "webradio: restored default stations");
        }
    } else {
        handled = false;
    }

    if (!handled) {
        return false;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "webradio: %s\n",
                                 esp_err_to_name(err));
    }
    solar_os_shell_io_flush(io);
    if (err == ESP_OK) {
        solar_os_context_finish(ctx, 0, NULL);
    } else {
        char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
        snprintf(message,
                 sizeof(message),
                 "webradio: %s",
                 esp_err_to_name(err));
        solar_os_context_finish(ctx, 1, message);
    }
    if (result != NULL) {
        *result = err;
    }
    return true;
}

static esp_err_t webradio_start(solar_os_context_t *ctx)
{
    memset(&webradio, 0, sizeof(webradio));
    webradio.task_done = true;
    bool force_tui = false;
    const char *args[3] = {0};
    int arg_count = 0;
    const int argc = solar_os_context_argc(ctx);
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "--tui") == 0) {
            force_tui = true;
        } else if (arg_count < (int)(sizeof(args) / sizeof(args[0]))) {
            args[arg_count++] = arg;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }

    const bool management_command = arg_count > 0 &&
        (strcmp(args[0], "list") == 0 || strcmp(args[0], "add") == 0 ||
         strcmp(args[0], "remove") == 0 || strcmp(args[0], "reset") == 0);
    if (management_command || arg_count > 1 ||
        (arg_count == 1 && !solar_os_webradio_url_valid(args[0]))) {
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
    } else {
        solar_os_context_set_app_class(
            ctx,
            !force_tui && webradio_graphical_session(ctx) ?
                SOLAR_OS_APP_CLASS_GUI : SOLAR_OS_APP_CLASS_TUI);
    }

    esp_err_t err = solar_os_webradio_catalog_init();
    if (err != ESP_OK) {
        return err;
    }

    if (webradio_manage_command(ctx, arg_count, args, &err)) {
        return ESP_OK;
    }
    if (arg_count > 1 ||
        (arg_count == 1 && !solar_os_webradio_url_valid(args[0]))) {
        return ESP_ERR_INVALID_ARG;
    }
    webradio.mode = !force_tui && webradio_graphical_session(ctx) ?
        WEBRADIO_MODE_GRAPHICS : WEBRADIO_MODE_TUI;
    solar_os_context_set_app_class(
        ctx,
        webradio.mode == WEBRADIO_MODE_GRAPHICS ?
            SOLAR_OS_APP_CLASS_GUI : SOLAR_OS_APP_CLASS_TUI);
    solar_os_audio_status_t audio_status;
    solar_os_audio_get_status(&audio_status);
    webradio.volume = audio_status.volume <= 100U ? audio_status.volume : 50U;
    webradio.tab = WEBRADIO_TAB_PLAYER;
    webradio.visualizer = WEBRADIO_VISUALIZER_SCOPE;
    if (webradio.mode == WEBRADIO_MODE_TUI) {
        err = solar_os_tui_screen_begin(&webradio.tui, ctx);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        err = solar_os_oscilloscope_widget_create(
            WEBRADIO_SCOPE_SAMPLES, &webradio.scope);
        if (err == ESP_OK) {
            err = solar_os_spectrum_widget_create(
                WEBRADIO_SPECTRUM_FFT_SIZE, &webradio.spectrum);
        }
        if (err != ESP_OK) {
            solar_os_oscilloscope_widget_destroy(webradio.scope);
            solar_os_spectrum_widget_destroy(webradio.spectrum);
            webradio.scope = NULL;
            webradio.spectrum = NULL;
            return err;
        }
        webradio_enable_high_refresh(ctx);
        solar_os_context_set_graphics_active(ctx, true);
    }
    webradio.ui_started = true;
    webradio_refresh_catalog();
    webradio.redraw = true;
    if (arg_count == 1) {
        err = webradio_start_playback("Direct stream", args[0]);
        if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
            webradio_set_playback_state(WEBRADIO_PLAYBACK_ERROR,
                                        esp_err_to_name(err));
        }
    }
    webradio_render(ctx);
    return ESP_OK;
}

static void webradio_stop(solar_os_context_t *ctx)
{
    webradio_stop_playback();
    if (!webradio.ui_started) {
        return;
    }
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        webradio_disable_high_refresh();
        solar_os_oscilloscope_widget_destroy(webradio.scope);
        solar_os_spectrum_widget_destroy(webradio.spectrum);
        webradio.scope = NULL;
        webradio.spectrum = NULL;
        solar_os_context_set_graphics_active(ctx, false);
    } else {
        solar_os_tui_set_cursor_visible(&webradio.tui, true);
        solar_os_tui_refresh(&webradio.tui);
        solar_os_tui_end(&webradio.tui);
    }
    webradio.ui_started = false;
}

static void webradio_suspend(solar_os_context_t *ctx)
{
    webradio.suspended = true;
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        webradio_disable_high_refresh();
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static void webradio_resume(solar_os_context_t *ctx)
{
    webradio.suspended = false;
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        webradio_enable_high_refresh(ctx);
        solar_os_context_set_graphics_active(ctx, true);
    }
    webradio.redraw = true;
    webradio_render(ctx);
}

static void webradio_title(solar_os_context_t *ctx,
                           char *buffer,
                           size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }
    char active_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
    webradio_snapshot_status(NULL,
                             NULL,
                             0U,
                             active_name,
                             sizeof(active_name),
                             NULL,
                             NULL,
                             NULL);
    if (active_name[0] != '\0') {
        snprintf(buffer, buffer_len, "webradio: %s", active_name);
    } else {
        strlcpy(buffer, "webradio", buffer_len);
    }
}

static bool webradio_handle_graphics_key(solar_os_context_t *ctx, uint8_t key)
{
    if (webradio_handle_dialog_key(key)) {
        webradio_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE ||
        key == 'q' || key == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (key == '\t') {
        webradio.tab = (webradio_tab_t)((webradio.tab + 1U) %
                                        WEBRADIO_TAB_COUNT);
        webradio.redraw = true;
        webradio_render(ctx);
        return true;
    }

    if (webradio.tab == WEBRADIO_TAB_PLAYER) {
        switch (key) {
        case SOLAR_OS_KEY_LEFT:
            webradio_play_catalog_offset(-1);
            break;
        case SOLAR_OS_KEY_RIGHT:
            webradio_play_catalog_offset(1);
            break;
        case SOLAR_OS_KEY_UP:
            webradio_adjust_volume(1);
            break;
        case SOLAR_OS_KEY_DOWN:
            webradio_adjust_volume(-1);
            break;
        case '\r':
        case '\n':
        case ' ':
            webradio_toggle_playback();
            break;
        case 'v':
        case 'V':
            webradio.visualizer =
                webradio.visualizer == WEBRADIO_VISUALIZER_SCOPE ?
                    WEBRADIO_VISUALIZER_SPECTRUM : WEBRADIO_VISUALIZER_SCOPE;
            break;
        default:
            return true;
        }
    } else {
        switch (key) {
        case SOLAR_OS_KEY_UP:
        case 'k':
            if (webradio.cursor > 0U) {
                webradio.cursor--;
            }
            break;
        case SOLAR_OS_KEY_DOWN:
        case 'j':
            if (webradio.cursor + 1U < webradio.station_count) {
                webradio.cursor++;
            }
            break;
        case '\r':
        case '\n':
            if (webradio.cursor < webradio.station_count) {
                webradio_play_selected();
                webradio.tab = WEBRADIO_TAB_PLAYER;
            }
            break;
        case 'a':
        case 'A':
            webradio_start_add_dialog();
            break;
        case 'e':
        case 'E':
            webradio_start_edit_dialog();
            break;
        case SOLAR_OS_KEY_DELETE:
        case 0x7f:
        case 'd':
        case 'D':
            webradio_remove_selected();
            break;
        default:
            return true;
        }
    }
    webradio.redraw = true;
    webradio_render(ctx);
    return true;
}

static bool webradio_event(solar_os_context_t *ctx,
                           const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        webradio_resume(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        webradio_reap_finished_task();
        uint32_t generation = 0U;
        (void)solar_os_webradio_catalog_snapshot(NULL, 0U, &generation);
        if (generation != webradio.catalog_generation) {
            webradio_refresh_catalog();
            webradio.redraw = true;
        }
        bool redraw = false;
        webradio_snapshot_status(NULL,
                                 NULL,
                                 0U,
                                 NULL,
                                 0U,
                                 NULL,
                                 NULL,
                                 &redraw);
        const bool visualizer_due =
            webradio.mode == WEBRADIO_MODE_GRAPHICS &&
            webradio.tab == WEBRADIO_TAB_PLAYER &&
            webradio.task != NULL && !webradio.suspended &&
            event->data.tick_ms - webradio.last_visualizer_ms >=
                WEBRADIO_VISUALIZER_REFRESH_MS;
        if (visualizer_due) {
            webradio.last_visualizer_ms = event->data.tick_ms;
        }
        if (redraw || webradio.redraw) {
            webradio_render(ctx);
        } else if (visualizer_due) {
            webradio_render_visualizer(ctx);
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (webradio.mode == WEBRADIO_MODE_GRAPHICS) {
        return webradio_handle_graphics_key(ctx, key);
    }
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (webradio_handle_dialog_key(key)) {
        webradio_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_ESCAPE ||
        key == 'q' || key == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    switch (key) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        if (webradio.cursor > 0U) {
            webradio.cursor--;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        if (webradio.cursor + 1U < webradio.station_count) {
            webradio.cursor++;
        }
        break;
    case '+':
        webradio_adjust_volume(1);
        break;
    case '-':
        webradio_adjust_volume(-1);
        break;
    case '\r':
    case '\n':
        webradio_play_selected();
        break;
    case ' ':
        webradio_stop_playback();
        break;
    case SOLAR_OS_KEY_DELETE:
    case 0x7f:
        webradio_remove_selected();
        break;
    case 'a':
    case 'A':
        webradio_start_add_dialog();
        break;
    case 'e':
    case 'E':
        webradio_start_edit_dialog();
        break;
    case 'r':
    case 'R': {
        char name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX];
        char url[SOLAR_OS_WEBRADIO_URL_MAX];
        portENTER_CRITICAL(&webradio_lock);
        strlcpy(name, webradio.active_name, sizeof(name));
        strlcpy(url, webradio.active_url, sizeof(url));
        portEXIT_CRITICAL(&webradio_lock);
        if (url[0] != '\0') {
            (void)webradio_start_playback(name, url);
        }
        break;
    }
    default:
        return true;
    }
    webradio.redraw = true;
    webradio_render(ctx);
    return true;
}

const solar_os_app_t solar_os_webradio_app = {
    .name = "webradio",
    .summary = "streaming internet radio",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = webradio_start,
    .suspend = webradio_suspend,
    .resume = webradio_resume,
    .stop = webradio_stop,
    .event = webradio_event,
    .title = webradio_title,
    .state_slot = &webradio_state,
    .state_size = sizeof(webradio_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = WEBRADIO_VISUALIZER_REFRESH_MS,
    .worker_stack_bytes = WEBRADIO_TASK_STACK,
    .worker_stack_external = true,
};
