#include "solar_os_player.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_media_widgets.h"
#include "solar_os_player_playlist.h"
#include "solar_os_shell_io.h"
#include "solar_os_signal_widgets.h"
#include "solar_os_storage.h"
#include "solar_os_storage_browser.h"
#include "solar_os_task.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define PLAYER_TASK_STACK 28672U
#define PLAYER_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define PLAYER_WORKER_POLL_MS 20U
#define PLAYER_REFRESH_MS 40U
#define PLAYER_DISPLAY_HPM_HZ_TENTHS 255U
#define PLAYER_SCOPE_SAMPLES 256U
#define PLAYER_SPECTRUM_FFT_SIZE 256U
#define PLAYER_HEADER_HEIGHT 28
#define PLAYER_ROW_HEIGHT 24
#define PLAYER_FOOTER_HEIGHT 20
#define PLAYER_VOLUME_STEP 5
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PLAYER_TASK_STACK);
#endif

typedef enum { PLAYER_MODE_TUI, PLAYER_MODE_GRAPHICS } player_mode_t;
typedef enum { PLAYER_TAB_PLAY, PLAYER_TAB_PLAYLIST, PLAYER_TAB_COUNT } player_tab_t;
typedef enum {
    PLAYER_VISUALIZER_CASSETTE,
    PLAYER_VISUALIZER_SCOPE,
    PLAYER_VISUALIZER_SPECTRUM,
    PLAYER_VISUALIZER_COUNT,
} player_visualizer_t;
typedef enum {
    PLAYER_STOPPED,
    PLAYER_STARTING,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_ERROR,
} player_playback_state_t;

typedef struct {
    player_mode_t mode;
    player_tab_t tab;
    player_visualizer_t visualizer;
    solar_os_tui_t tui;
    solar_os_shell_io_t fallback_io;
    solar_os_storage_browser_t *browser;
    solar_os_cassette_widget_t *cassette;
    solar_os_oscilloscope_widget_t *scope;
    solar_os_spectrum_widget_t *spectrum;
    size_t cursor;
    size_t top;
    size_t active_index;
    size_t track_count;
    uint32_t playlist_generation;
    uint32_t elapsed_ms;
    uint32_t total_ms;
    uint32_t sample_rate;
    uint64_t played_frames;
    uint32_t last_visualizer_ms;
    uint8_t volume;
    bool browsing;
    bool ui_started;
    bool suspended;
    bool high_refresh_active;
    bool redraw;
    bool natural_completion;
    volatile bool stop_requested;
    volatile bool paused;
    volatile bool task_done;
    TaskHandle_t task;
    player_playback_state_t playback_state;
    esp_err_t playback_error;
    char active_path[SOLAR_OS_STORAGE_PATH_MAX];
    char active_device_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    uint32_t active_device_capabilities;
    char message[96];
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
} player_state_t;

static const char *TAG = "solar_os_player";
static void *player_state;
#define player (*(player_state_t *)player_state)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("audio worker callback spinlock")
static portMUX_TYPE player_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *player_basename(const char *path)
{
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    return slash != NULL ? slash + 1U : (path != NULL ? path : "");
}

static bool player_audio_file(const char *name, void *user)
{
    (void)user;
    const char *dot = name != NULL ? strrchr(name, '.') : NULL;
    return dot != NULL && (strcasecmp(dot, ".wav") == 0 ||
                           strcasecmp(dot, ".mp3") == 0);
}

static solar_os_shell_io_t *player_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&player.fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &player.fallback_io);
        io = &player.fallback_io;
    }
    return io;
}

static bool player_graphical_session(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = player_io(ctx);
    return solar_os_context_gfx(ctx) != NULL &&
        (io == NULL || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT);
}

static void player_set_message(const char *message)
{
    strlcpy(player.message, message != NULL ? message : "",
            sizeof(player.message));
    player.redraw = true;
}

static void player_refresh_playlist(void)
{
    player.track_count = solar_os_player_playlist_count();
    player.playlist_generation = solar_os_player_playlist_generation();
    if (player.track_count == 0U) {
        player.cursor = 0U;
        player.top = 0U;
    } else if (player.cursor >= player.track_count) {
        player.cursor = player.track_count - 1U;
    }
}

static void player_enable_high_refresh(solar_os_context_t *ctx)
{
    if (player.high_refresh_active) {
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || !solar_os_gfx_display_target_name(
            gfx, player.display_target, sizeof(player.display_target))) {
        return;
    }
    if (solar_os_display_set_high_refresh_override(
            player.display_target, true, PLAYER_DISPLAY_HPM_HZ_TENTHS) == ESP_OK) {
        player.high_refresh_active = true;
    }
}

static void player_disable_high_refresh(void)
{
    if (player.high_refresh_active) {
        (void)solar_os_display_set_high_refresh_override(
            player.display_target, false, PLAYER_DISPLAY_HPM_HZ_TENTHS);
        player.high_refresh_active = false;
    }
}

static bool player_cancel_callback(void *user)
{
    (void)user;
    return player.stop_requested;
}

static bool player_pause_callback(void *user)
{
    (void)user;
    return player.paused;
}

static void player_progress_callback(
    const solar_os_audio_wav_progress_t *progress, void *user)
{
    (void)user;
    if (progress != NULL) {
        if (progress->info.sample_rate != 0U) {
            player.sample_rate = progress->info.sample_rate;
        }
        if (player.played_frames == 0U) {
            player.elapsed_ms = progress->info.duration_ms;
        }
    }
}

static void player_samples_callback(const int16_t *samples,
                                    size_t sample_count,
                                    uint8_t channels,
                                    void *user)
{
    (void)user;
    if (samples == NULL || channels == 0U) {
        return;
    }
    const size_t frames = sample_count / channels;
    if (player.scope != NULL) {
        (void)solar_os_oscilloscope_widget_submit_s16(
            player.scope, samples, frames, channels);
    }
    if (player.spectrum != NULL) {
        (void)solar_os_spectrum_widget_submit_s16(
            player.spectrum, samples, frames, channels);
    }
    portENTER_CRITICAL(&player_lock);
    player.played_frames += frames;
    if (player.sample_rate != 0U) {
        player.elapsed_ms = (uint32_t)((player.played_frames * 1000U) /
                                       player.sample_rate);
    }
    player.playback_state = player.paused ? PLAYER_PAUSED : PLAYER_PLAYING;
    portEXIT_CRITICAL(&player_lock);
}

static void player_device_callback(const solar_os_audio_device_info_t *device,
                                   void *user)
{
    (void)user;
    if (device == NULL) return;
    portENTER_CRITICAL(&player_lock);
    strlcpy(player.active_device_id, device->id,
            sizeof(player.active_device_id));
    player.active_device_capabilities = device->capabilities;
    portEXIT_CRITICAL(&player_lock);
}

static void player_worker(void *arg)
{
    (void)arg;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    strlcpy(path, player.active_path, sizeof(path));
    solar_os_audio_wav_info_t info = {0};
    const char *dot = strrchr(path, '.');
    esp_err_t err = dot != NULL && strcasecmp(dot, ".mp3") == 0 ?
        solar_os_audio_get_mp3_info(path, &info) :
        solar_os_audio_get_wav_info(path, &info);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&player_lock);
        player.sample_rate = info.sample_rate;
        player.total_ms = info.duration_ms;
        player.playback_state = PLAYER_STARTING;
        portEXIT_CRITICAL(&player_lock);
        const solar_os_audio_wav_options_t options = {
            .owner = "player",
            .should_cancel = player_cancel_callback,
            .progress = player_progress_callback,
            .should_pause = player_pause_callback,
            .samples = player_samples_callback,
            .device = player_device_callback,
            .user = NULL,
            .progress_interval_ms = 250U,
        };
        err = dot != NULL && strcasecmp(dot, ".mp3") == 0 ?
            solar_os_audio_play_mp3(path, player.volume, &options, &info) :
            solar_os_audio_play_wav(path, player.volume, &options, &info);
    }
    portENTER_CRITICAL(&player_lock);
    player.playback_error = err;
    player.natural_completion = err == ESP_OK && !player.stop_requested;
    player.playback_state = err == ESP_OK || player.stop_requested ?
        PLAYER_STOPPED : PLAYER_ERROR;
    if (err != ESP_OK && !player.stop_requested) {
        snprintf(player.message, sizeof(player.message), "Playback failed: %s",
                 esp_err_to_name(err));
    }
    player.task_done = true;
    player.redraw = true;
    portEXIT_CRITICAL(&player_lock);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void player_stop_playback(void)
{
    player.stop_requested = true;
    player.paused = false;
    TaskHandle_t task = player.task;
    if (task != NULL && !solar_os_task_wait_done(
            task, &player.task_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "player task did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        while (!player.task_done) {
            vTaskDelay(pdMS_TO_TICKS(PLAYER_WORKER_POLL_MS));
        }
    }
    if (task != NULL) {
        solar_os_task_delete_external(task);
    }
    player.task = NULL;
    player.task_done = true;
    player.stop_requested = false;
    player.natural_completion = false;
    player.playback_state = PLAYER_STOPPED;
    player.elapsed_ms = 0U;
    player.played_frames = 0U;
    player.redraw = true;
}

static esp_err_t player_play_index(size_t index)
{
    solar_os_player_track_t track;
    if (!solar_os_player_playlist_get(index, &track)) {
        return ESP_ERR_INVALID_ARG;
    }
    player_stop_playback();
    strlcpy(player.active_path, track.path, sizeof(player.active_path));
    player.active_index = index;
    player.cursor = index;
    player.elapsed_ms = 0U;
    player.total_ms = 0U;
    player.sample_rate = 0U;
    player.active_device_id[0] = '\0';
    player.active_device_capabilities = 0U;
    player.played_frames = 0U;
    player.playback_error = ESP_OK;
    player.playback_state = PLAYER_STARTING;
    player.task_done = false;
    player.stop_requested = false;
    solar_os_cassette_widget_reset(player.cassette);
    solar_os_oscilloscope_widget_reset(player.scope);
    solar_os_spectrum_widget_reset(player.spectrum);
    const BaseType_t created = solar_os_task_create_pinned_external(
        player_worker, "player_audio", PLAYER_TASK_STACK, NULL,
        PLAYER_TASK_PRIORITY, &player.task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        player.task = NULL;
        player.task_done = true;
        player.playback_state = PLAYER_ERROR;
        player.playback_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void player_play_offset(int offset)
{
    if (player.track_count == 0U) {
        player_set_message("Playlist is empty");
        return;
    }
    size_t index = player.task != NULL ? player.active_index : player.cursor;
    index = offset < 0 ?
        (index == 0U ? player.track_count - 1U : index - 1U) :
        (index + 1U) % player.track_count;
    (void)player_play_index(index);
}

static void player_reap_finished(void)
{
    if (player.task == NULL || !player.task_done) {
        return;
    }
    const bool advance = player.natural_completion;
    TaskHandle_t task = player.task;
    player.task = NULL;
    player.natural_completion = false;
    solar_os_task_delete_external(task);
    if (advance && player.track_count > 0U) {
        (void)player_play_index((player.active_index + 1U) % player.track_count);
    }
}

static void player_toggle_pause(void)
{
    if (player.task == NULL || player.task_done) {
        return;
    }
    player.paused = !player.paused;
    player.playback_state = player.paused ? PLAYER_PAUSED : PLAYER_PLAYING;
    player.redraw = true;
}

static void player_toggle_play_stop(void)
{
    if (player.task != NULL) {
        player_stop_playback();
    } else if (player.cursor < player.track_count) {
        (void)player_play_index(player.cursor);
    }
}

static void player_adjust_volume(int direction)
{
    int volume = (int)player.volume + direction * PLAYER_VOLUME_STEP;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    esp_err_t err = solar_os_audio_set_volume((uint8_t)volume);
    if (err == ESP_ERR_NOT_SUPPORTED && player.active_device_id[0] != '\0' &&
        (player.active_device_capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0U) {
        err = solar_os_audio_set_device_volume(player.active_device_id,
                                               (uint8_t)volume);
    }
    if (err == ESP_OK || player.task == NULL) {
        player.volume = (uint8_t)volume;
        player_set_message(NULL);
    } else {
        player_set_message("Volume control unavailable");
    }
}

static void player_format_time(uint32_t duration_ms, bool known,
                               char *text, size_t text_len)
{
    if (!known) {
        strlcpy(text, "--:--", text_len);
        return;
    }
    const uint32_t seconds = duration_ms / 1000U;
    snprintf(text, text_len, "%02" PRIu32 ":%02" PRIu32,
             seconds / 60U, seconds % 60U);
}

static const char *player_state_symbol(void)
{
    if (player.playback_state == PLAYER_PAUSED) return "||";
    if (player.task != NULL) return ">";
    return "[]";
}

static void player_render_tui(void)
{
    const size_t rows = solar_os_tui_rows(&player.tui);
    const size_t cols = solar_os_tui_cols(&player.tui);
    if (rows < 5U || cols < 20U) {
        solar_os_tui_draw_too_small(&player.tui, "player");
        solar_os_tui_refresh(&player.tui);
        return;
    }
    solar_os_tui_clear(&player.tui);
    solar_os_tui_draw_title(&player.tui,
                            player.browsing ? "Player - Add track" : "Player",
                            NULL);
    size_t count = player.browsing ? solar_os_storage_browser_count(player.browser) :
        player.track_count;
    size_t cursor = player.browsing ? solar_os_storage_browser_cursor(player.browser) :
        player.cursor;
    const size_t list_rows = rows - 3U;
    if (cursor < player.top) player.top = cursor;
    if (cursor >= player.top + list_rows) player.top = cursor - list_rows + 1U;
    for (size_t row = 0U; row < list_rows; row++) {
        const size_t index = player.top + row;
        if (index >= count) break;
        char line[192];
        if (player.browsing) {
            solar_os_storage_browser_entry_t entry;
            if (!solar_os_storage_browser_entry(player.browser, index, &entry)) continue;
            snprintf(line, sizeof(line), "%s%s",
                     entry.is_directory ? "[DIR] " : "      ", entry.name);
        } else {
            solar_os_player_track_t track;
            if (!solar_os_player_playlist_get(index, &track)) continue;
            snprintf(line, sizeof(line), "%c %s",
                     player.task != NULL && index == player.active_index ? '*' : ' ',
                     player_basename(track.path));
        }
        solar_os_tui_write_cell(
            &player.tui, row + 1U, 1U, cols > 2U ? cols - 2U : 0U, line,
            index == cursor ? SOLAR_OS_TUI_ATTR_INVERSE :
                              SOLAR_OS_TUI_ATTR_NORMAL);
    }
    char elapsed[12], total[12], status[192];
    player_format_time(player.elapsed_ms, true, elapsed, sizeof(elapsed));
    player_format_time(player.total_ms, player.total_ms != 0U, total, sizeof(total));
    if (player.browsing) {
        snprintf(status, sizeof(status), "%s | Enter open/add  Esc cancel",
                 solar_os_storage_browser_path(player.browser));
    } else if (player.playback_state == PLAYER_ERROR && player.message[0] != '\0') {
        snprintf(status, sizeof(status), "%s", player.message);
    } else {
        snprintf(status, sizeof(status), "%s %s / %s | Up/Down  Enter play/stop  Space pause  A add  Del remove  Esc exit",
                 player_state_symbol(), elapsed, total);
    }
    solar_os_tui_draw_help(&player.tui, status);
    solar_os_tui_set_cursor_visible(&player.tui, false);
    solar_os_tui_refresh(&player.tui);
}

static void player_draw_centered(solar_os_gfx_t *gfx, int width,
                                 int baseline, const char *text)
{
    solar_os_gfx_text(gfx,
                      (width - (int)solar_os_gfx_text_width(gfx, text)) / 2,
                      baseline, text);
}

static void player_draw_header(solar_os_gfx_t *gfx, int width)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, PLAYER_HEADER_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    solar_os_gfx_text(gfx, 7, 19, "Player");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    const char *tabs = player.tab == PLAYER_TAB_PLAY ?
        "[PLAY]  PLAYLIST" : "PLAY  [PLAYLIST]";
    solar_os_gfx_text(gfx, width - (int)solar_os_gfx_text_width(gfx, tabs) - 7,
                      18, tabs);
}

static void player_draw_visualizer(solar_os_gfx_t *gfx, int width, int height)
{
    const int media_top = height * 2 / 3;
    const int visual_y = PLAYER_HEADER_HEIGHT + 4;
    const int visual_height = media_top - visual_y - 4;
    if (player.visualizer == PLAYER_VISUALIZER_CASSETTE) {
        solar_os_cassette_widget_draw(player.cassette, gfx, 5, visual_y,
                                      width - 10, visual_height);
    } else if (player.visualizer == PLAYER_VISUALIZER_SCOPE) {
        solar_os_oscilloscope_widget_draw(player.scope, gfx, 5, visual_y,
                                          width - 10, visual_height);
    } else {
        solar_os_spectrum_widget_draw(player.spectrum, gfx, 5, visual_y,
                                      width - 10, visual_height);
    }
    static const char *labels[] = {"CASSETTE  V", "SCOPE  V", "SPECTRUM  V"};
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 9, visual_y + 4, 86, 15);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 13, visual_y + 15, labels[player.visualizer]);
}

static void player_draw_progress(solar_os_gfx_t *gfx, int width, int height,
                                 bool clear_background)
{
    const int media_top = height * 2 / 3;
    if (clear_background) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_fill_rect(gfx, 1, media_top + 23, width - 2, 17);
    }
    char elapsed[12], total[12], status[40];
    player_format_time(player.elapsed_ms, true, elapsed, sizeof(elapsed));
    player_format_time(player.total_ms, player.total_ms != 0U, total, sizeof(total));
    if (player.playback_state == PLAYER_ERROR && player.message[0] != '\0') {
        snprintf(status, sizeof(status), "%s", player.message);
    } else {
        snprintf(status, sizeof(status), "%s %s / %s", player_state_symbol(), elapsed, total);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    player_draw_centered(gfx, width, media_top + 37, status);
}

static void player_render_play(solar_os_gfx_t *gfx, int width, int height)
{
    const int media_top = height * 2 / 3;
    player_draw_visualizer(gfx, width, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 0, media_top, width - 1, media_top);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
    player_draw_centered(gfx, width, media_top + 20,
                         player.active_path[0] != '\0' ?
                            player_basename(player.active_path) : "No track selected");
    player_draw_progress(gfx, width, height, false);
    const int volume_width = width / 2;
    const int volume_x = (width - volume_width) / 2;
    const int volume_y = media_top + 43;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, volume_x - 29, volume_y + 9, "VOL");
    solar_os_gfx_rect(gfx, volume_x, volume_y, volume_width, 10);
    if (player.volume > 0U) {
        solar_os_gfx_fill_rect(gfx, volume_x + 2, volume_y + 2,
                               (volume_width - 4) * player.volume / 100, 6);
    }
    const int gap = 5;
    const int button_width = (width - 4 * gap) / 3;
    const int button_y = height - 25;
    solar_os_media_transport_button_draw(
        gfx, gap, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_PREVIOUS, false);
    solar_os_media_transport_button_draw(
        gfx, gap * 2 + button_width, button_y, button_width, 21,
        player.task != NULL ? SOLAR_OS_MEDIA_TRANSPORT_STOP :
                              SOLAR_OS_MEDIA_TRANSPORT_PLAY,
        false);
    solar_os_media_transport_button_draw(
        gfx, gap * 3 + button_width * 2, button_y, button_width, 21,
        SOLAR_OS_MEDIA_TRANSPORT_NEXT, false);
}

static void player_render_list(solar_os_gfx_t *gfx, int width, int height)
{
    const int list_y = PLAYER_HEADER_HEIGHT + 5;
    const int bottom = height - PLAYER_FOOTER_HEIGHT;
    const size_t visible = bottom > list_y ?
        (size_t)((bottom - list_y) / PLAYER_ROW_HEIGHT) : 0U;
    const size_t count = player.browsing ?
        solar_os_storage_browser_count(player.browser) : player.track_count;
    const size_t cursor = player.browsing ?
        solar_os_storage_browser_cursor(player.browser) : player.cursor;
    if (cursor < player.top) player.top = cursor;
    if (cursor >= player.top + visible) player.top = cursor - visible + 1U;
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = player.top + row;
        if (index >= count) break;
        const int y = list_y + (int)row * PLAYER_ROW_HEIGHT;
        char label[192];
        if (player.browsing) {
            solar_os_storage_browser_entry_t entry;
            if (!solar_os_storage_browser_entry(player.browser, index, &entry)) continue;
            snprintf(label, sizeof(label), "%s%s",
                     entry.is_directory ? "[DIR] " : "", entry.name);
        } else {
            solar_os_player_track_t track;
            if (!solar_os_player_playlist_get(index, &track)) continue;
            snprintf(label, sizeof(label), "%c %s",
                     player.task != NULL && index == player.active_index ? '>' : ' ',
                     player_basename(track.path));
        }
        if (index == cursor) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8, PLAYER_ROW_HEIGHT - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_text(gfx, 9, y + 17, label);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 6, height - 6,
                      player.browsing ? "Enter open/add   Esc cancel" :
                                        "A add   Del remove   Enter play");
}

static void player_render(solar_os_context_t *ctx)
{
    if (!player.ui_started || player.suspended) return;
    if (player.mode == PLAYER_MODE_TUI) {
        player_render_tui();
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    player_draw_header(gfx, width);
    if (player.tab == PLAYER_TAB_PLAY && !player.browsing) {
        player_render_play(gfx, width, height);
    } else {
        player_render_list(gfx, width, height);
    }
    solar_os_gfx_present(gfx);
    player.redraw = false;
}

static void player_render_dynamic(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || player.suspended ||
        player.mode != PLAYER_MODE_GRAPHICS ||
        player.tab != PLAYER_TAB_PLAY || player.browsing) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    player_draw_visualizer(gfx, width, height);
    player_draw_progress(gfx, width, height, true);
    solar_os_gfx_present(gfx);
}

static void player_begin_browser(void)
{
    const char *path = solar_os_storage_mount_point();
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    if (player.active_path[0] != '\0') {
        strlcpy(directory, player.active_path, sizeof(directory));
        char *slash = strrchr(directory, '/');
        if (slash != NULL && slash != directory) *slash = '\0';
        path = directory;
    }
    if (solar_os_storage_browser_open(player.browser, path) == ESP_OK) {
        player.browsing = true;
        player.top = 0U;
        player_set_message(NULL);
    } else {
        player_set_message("Cannot open storage");
    }
}

static void player_activate_browser(void)
{
    char selected[SOLAR_OS_STORAGE_PATH_MAX];
    bool file_selected = false;
    esp_err_t err = solar_os_storage_browser_activate(
        player.browser, selected, sizeof(selected), &file_selected);
    if (err == ESP_OK && file_selected) {
        size_t index = 0U;
        err = solar_os_player_playlist_add(selected, &index);
        if (err == ESP_OK) {
            player_refresh_playlist();
            player.cursor = index;
            player.browsing = false;
            player.top = 0U;
            player_set_message("Track added");
        }
    }
    if (err != ESP_OK) player_set_message("Cannot add track");
}

static void player_remove_selected(void)
{
    if (player.cursor >= player.track_count) return;
    const bool active = player.task != NULL && player.cursor == player.active_index;
    if (active) player_stop_playback();
    if (solar_os_player_playlist_remove(player.cursor) == ESP_OK) {
        player_refresh_playlist();
        player_set_message("Track removed");
    } else {
        player_set_message("Cannot remove track");
    }
}

static bool player_handle_key(solar_os_context_t *ctx, uint8_t key)
{
    if (key == SOLAR_OS_KEY_APP_EXIT ||
        (!player.browsing && (key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q'))) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (player.browsing) {
        if (key == SOLAR_OS_KEY_ESCAPE) {
            player.browsing = false;
            player.top = 0U;
        } else if (key == SOLAR_OS_KEY_UP || key == 'k') {
            solar_os_storage_browser_move(player.browser, -1);
        } else if (key == SOLAR_OS_KEY_DOWN || key == 'j') {
            solar_os_storage_browser_move(player.browser, 1);
        } else if (key == '\r' || key == '\n') {
            player_activate_browser();
        }
        player.redraw = true;
        player_render(ctx);
        return true;
    }
    if (player.mode == PLAYER_MODE_GRAPHICS && key == '\t') {
        player.tab = (player_tab_t)((player.tab + 1U) % PLAYER_TAB_COUNT);
    } else if (player.mode == PLAYER_MODE_GRAPHICS && player.tab == PLAYER_TAB_PLAY) {
        if (key == SOLAR_OS_KEY_LEFT) player_play_offset(-1);
        else if (key == SOLAR_OS_KEY_RIGHT) player_play_offset(1);
        else if (key == SOLAR_OS_KEY_UP) player_adjust_volume(1);
        else if (key == SOLAR_OS_KEY_DOWN) player_adjust_volume(-1);
        else if (key == '\r' || key == '\n') player_toggle_play_stop();
        else if (key == ' ') player_toggle_pause();
        else if (key == 'v' || key == 'V')
            player.visualizer = (player_visualizer_t)((player.visualizer + 1U) %
                                                       PLAYER_VISUALIZER_COUNT);
    } else {
        if (key == SOLAR_OS_KEY_UP || key == 'k') {
            if (player.cursor > 0U) player.cursor--;
        } else if (key == SOLAR_OS_KEY_DOWN || key == 'j') {
            if (player.cursor + 1U < player.track_count) player.cursor++;
        } else if (key == '\r' || key == '\n') {
            if (player.mode == PLAYER_MODE_TUI) player_toggle_play_stop();
            else if (player.cursor < player.track_count) {
                (void)player_play_index(player.cursor);
                player.tab = PLAYER_TAB_PLAY;
            }
        } else if (key == ' ') player_toggle_pause();
        else if (key == 'a' || key == 'A') player_begin_browser();
        else if (key == SOLAR_OS_KEY_DELETE || key == 0x7fU) player_remove_selected();
    }
    player.redraw = true;
    player_render(ctx);
    return true;
}

static esp_err_t player_start(solar_os_context_t *ctx)
{
    memset(&player, 0, sizeof(player));
    const int argc = solar_os_context_argc(ctx);
    bool force_tui = false;
    const char *path_arg = NULL;
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "--tui") == 0) {
            force_tui = true;
        } else if (path_arg == NULL) {
            path_arg = arg;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    const player_mode_t launch_mode =
        !force_tui && player_graphical_session(ctx) ?
            PLAYER_MODE_GRAPHICS : PLAYER_MODE_TUI;
    solar_os_context_set_app_class(
        ctx,
        launch_mode == PLAYER_MODE_GRAPHICS ?
            SOLAR_OS_APP_CLASS_GUI : SOLAR_OS_APP_CLASS_TUI);
    player.task_done = true;
    player.active_index = SIZE_MAX;
    solar_os_audio_status_t audio_status;
    solar_os_audio_get_status(&audio_status);
    player.volume = audio_status.volume <= 100U ? audio_status.volume : 50U;
    player.visualizer = PLAYER_VISUALIZER_CASSETTE;
    esp_err_t err = solar_os_player_playlist_init();
    if (err != ESP_OK) return err;
    err = solar_os_storage_browser_create(player_audio_file, NULL, &player.browser);
    if (err != ESP_OK) {
        solar_os_cassette_widget_destroy(player.cassette);
        solar_os_oscilloscope_widget_destroy(player.scope);
        solar_os_spectrum_widget_destroy(player.spectrum);
        solar_os_storage_browser_destroy(player.browser);
        player.browser = NULL;
        return err;
    }
    player.mode = launch_mode;
    if (player.mode == PLAYER_MODE_TUI) {
        err = solar_os_tui_screen_begin(&player.tui, ctx);
    } else {
        err = solar_os_cassette_widget_create(&player.cassette);
        if (err == ESP_OK) err = solar_os_oscilloscope_widget_create(
            PLAYER_SCOPE_SAMPLES, &player.scope);
        if (err == ESP_OK) err = solar_os_spectrum_widget_create(
            PLAYER_SPECTRUM_FFT_SIZE, &player.spectrum);
        if (err == ESP_OK) {
            player_enable_high_refresh(ctx);
            solar_os_context_set_graphics_active(ctx, true);
        }
    }
    if (err != ESP_OK) return err;
    player.ui_started = true;
    player_refresh_playlist();
    if (path_arg != NULL) {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        err = solar_os_storage_resolve_path(path_arg, path, sizeof(path));
        size_t index = 0U;
        if (err == ESP_OK) err = solar_os_player_playlist_add(path, &index);
        if (err == ESP_OK) {
            player_refresh_playlist();
            player.cursor = index;
            err = player_play_index(index);
        }
        if (err != ESP_OK) {
            char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
            snprintf(message,
                     sizeof(message),
                     "player: cannot open %s: %s",
                     path_arg,
                     esp_err_to_name(err));
            solar_os_context_finish(ctx, 1, message);
            err = ESP_OK;
            return err;
        }
    }
    player_render(ctx);
    return err;
}

static void player_stop(solar_os_context_t *ctx)
{
    player_stop_playback();
    if (player.mode == PLAYER_MODE_GRAPHICS) {
        player_disable_high_refresh();
        solar_os_cassette_widget_destroy(player.cassette);
        solar_os_oscilloscope_widget_destroy(player.scope);
        solar_os_spectrum_widget_destroy(player.spectrum);
        solar_os_context_set_graphics_active(ctx, false);
    } else if (player.ui_started) {
        solar_os_tui_set_cursor_visible(&player.tui, true);
        solar_os_tui_refresh(&player.tui);
        solar_os_tui_end(&player.tui);
    }
    solar_os_storage_browser_destroy(player.browser);
    player.browser = NULL;
    player.ui_started = false;
}

static void player_suspend(solar_os_context_t *ctx)
{
    player.suspended = true;
    if (player.mode == PLAYER_MODE_GRAPHICS) {
        player_disable_high_refresh();
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static void player_resume(solar_os_context_t *ctx)
{
    player.suspended = false;
    if (player.mode == PLAYER_MODE_GRAPHICS) {
        player_enable_high_refresh(ctx);
        solar_os_context_set_graphics_active(ctx, true);
    }
    player_render(ctx);
}

static bool player_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        player_resume(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        player_reap_finished();
        player_refresh_playlist();
        const bool playing = player.task != NULL && !player.paused;
        solar_os_cassette_widget_update(player.cassette, playing,
            player.elapsed_ms, player.total_ms, event->data.tick_ms);
        const bool refresh_due = playing &&
            event->data.tick_ms - player.last_visualizer_ms >= PLAYER_REFRESH_MS;
        if (!player.suspended && player.redraw) {
            player_render(ctx);
        } else if (!player.suspended && refresh_due) {
            player.last_visualizer_ms = event->data.tick_ms;
            if (player.mode == PLAYER_MODE_GRAPHICS &&
                player.tab == PLAYER_TAB_PLAY && !player.browsing) {
                player_render_dynamic(ctx);
            } else if (player.mode == PLAYER_MODE_TUI) {
                player_render(ctx);
            }
        }
        return true;
    }
    return event->type == SOLAR_OS_EVENT_CHAR ?
        player_handle_key(ctx, (uint8_t)event->data.ch) : false;
}

static void player_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        snprintf(buffer, buffer_len, "player%s%s",
                 player.active_path[0] != '\0' ? ": " : "",
                 player.active_path[0] != '\0' ? player_basename(player.active_path) : "");
    }
}

const solar_os_app_t solar_os_player_app = {
    .name = "player",
    .summary = "playlist audio player",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = player_start,
    .suspend = player_suspend,
    .resume = player_resume,
    .stop = player_stop,
    .event = player_event,
    .title = player_title,
    .state_slot = &player_state,
    .state_size = sizeof(player_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = PLAYER_REFRESH_MS,
    .worker_stack_bytes = PLAYER_TASK_STACK,
    .worker_stack_external = true,
};
