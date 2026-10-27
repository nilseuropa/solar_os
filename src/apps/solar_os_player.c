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
    PLAYER_SEEKING,
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
    uint32_t start_ms;
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
    volatile bool seeking;
    int16_t pointer_x, pointer_y;
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
        /* Samples callbacks count output frames, not source-file frames. */
        if (player.active_device_id[0] == '\0' && progress->info.sample_rate != 0U) {
            player.sample_rate = progress->info.sample_rate;
        }
        if (player.played_frames == 0U && progress->info.duration_ms >= player.start_ms) {
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
    player.seeking = false;
    player.played_frames += frames;
    if (player.sample_rate != 0U) {
        player.elapsed_ms = player.start_ms + (uint32_t)((player.played_frames * 1000U) /
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
    if (device->native_format.sample_rate != 0U)
        player.sample_rate = device->native_format.sample_rate;
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
        player.playback_state = player.paused ? PLAYER_PAUSED :
            player.start_ms ? PLAYER_SEEKING : PLAYER_STARTING;
        portEXIT_CRITICAL(&player_lock);
        const solar_os_audio_wav_options_t options = {
            .owner = "player",
            .start_ms = player.start_ms,
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
    player.seeking = false;
    player.natural_completion = false;
    player.playback_state = PLAYER_STOPPED;
    player.elapsed_ms = 0U;
    player.played_frames = 0U;
    player.redraw = true;
}

static esp_err_t player_play_index_at(size_t index, uint32_t start_ms, bool paused)
{
    solar_os_player_track_t track;
    if (!solar_os_player_playlist_get(index, &track)) {
        return ESP_ERR_INVALID_ARG;
    }
    player_stop_playback();
    strlcpy(player.active_path, track.path, sizeof(player.active_path));
    player.active_index = index;
    player.cursor = index;
    player.start_ms = start_ms;
    player.elapsed_ms = start_ms;
    player.total_ms = 0U;
    player.sample_rate = 0U;
    player.active_device_id[0] = '\0';
    player.active_device_capabilities = 0U;
    player.played_frames = 0U;
    player.playback_error = ESP_OK;
    player.playback_state = paused ? PLAYER_PAUSED : start_ms ? PLAYER_SEEKING : PLAYER_STARTING;
    player.task_done = false;
    player.stop_requested = false;
    player.paused = paused;
    player.seeking = start_ms != 0;
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

static esp_err_t player_play_index(size_t index)
{
    return player_play_index_at(index, 0U, false);
}

static void player_seek(int direction)
{
    if (player.task == NULL || player.task_done) return;
    portENTER_CRITICAL(&player_lock);
    int64_t target = (int64_t)player.elapsed_ms + direction * 10000;
    const uint32_t total = player.total_ms;
    const bool paused = player.paused;
    portEXIT_CRITICAL(&player_lock);
    if (target < 0) target = 0;
    /* MP3 duration is unknown until EOF; never use a bitrate estimate. */
    if (total != 0U && target >= total) target = total - 1U;
    if (target > UINT32_MAX) target = UINT32_MAX;
    esp_err_t err = player_play_index_at(player.active_index, (uint32_t)target, paused);
    if (err != ESP_OK) player_set_message("Cannot seek: audio task unavailable");
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
    player.playback_state = player.paused ? PLAYER_PAUSED :
        player.seeking ? PLAYER_SEEKING : PLAYER_PLAYING;
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
    if (player.playback_state == PLAYER_SEEKING) return "SEEKING";
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
    const size_t list_rows = solar_os_tui_screen_content_rows(
        &player.tui, 1U, 1U);
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
        snprintf(status, sizeof(status), "%s %s / %s | Up/Down  Enter play/stop  Space pause  </> seek  A add  Del remove  Esc exit",
                 player_state_symbol(), elapsed, total);
    }
    solar_os_tui_draw_footer(
        &player.tui,
        player.playback_state == PLAYER_ERROR ? player.message : NULL,
        status);
    solar_os_tui_set_cursor_visible(&player.tui, false);
    solar_os_tui_refresh(&player.tui);
}

static void player_draw_header(solar_os_gfx_t *gfx, int width)
{
    const char *tabs = player.tab == PLAYER_TAB_PLAY ?
        "[PLAY]  PLAYLIST" : "PLAY  [PLAYLIST]";
    solar_os_media_player_header_draw(gfx, width, "Player", tabs);
}

static void player_draw_visualizer(solar_os_gfx_t *gfx, int width, int height)
{
    solar_os_media_player_layout_t layout;
    solar_os_media_player_layout(width, height, true, &layout);
    const int visual_y = layout.view_y, visual_height = layout.view_height;
    if (visual_height < 5) return;
    if (player.visualizer == PLAYER_VISUALIZER_CASSETTE && visual_height >= 50 && layout.view_width >= 80) {
        solar_os_cassette_widget_draw(player.cassette, gfx, layout.view_x, visual_y,
                                      layout.view_width, visual_height);
    } else if (player.visualizer != PLAYER_VISUALIZER_SPECTRUM) {
        solar_os_oscilloscope_widget_draw(player.scope, gfx, layout.view_x, visual_y,
                                          layout.view_width, visual_height);
    } else {
        solar_os_spectrum_widget_draw(player.spectrum, gfx, layout.view_x, visual_y,
                                      layout.view_width, visual_height);
    }
    if (layout.compact) return;
    static const char *labels[] = {"CASSETTE  V", "SCOPE  V", "SPECTRUM  V"};
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 9, visual_y + 4, 86, 15);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 13, visual_y + 15, labels[player.visualizer]);
}

static void player_progress_text(char *status, size_t capacity)
{
    char elapsed[12], total[12];
    player_format_time(player.elapsed_ms, true, elapsed, sizeof(elapsed));
    player_format_time(player.total_ms, player.total_ms != 0U, total, sizeof(total));
    if (player.playback_state == PLAYER_ERROR && player.message[0] != '\0') {
        snprintf(status, capacity, "%s", player.message);
    } else {
        static const char *states[] = {"STOPPED", "STARTING", "SEEKING", "PLAYING", "PAUSED", "ERROR"};
        snprintf(status, capacity, "%s %s / %s", states[player.playback_state], elapsed, total);
    }
}

static void player_render_play(solar_os_gfx_t *gfx, int width, int height)
{
    player_draw_visualizer(gfx, width, height);
    char status[96];
    player_progress_text(status, sizeof(status));
    solar_os_media_player_controls_draw(gfx, width, height,
        player.active_path[0] ? player_basename(player.active_path) : "No track selected",
        status, player.volume, true,
        player.task == NULL ? SOLAR_OS_MEDIA_TRANSPORT_PLAY :
        player.paused ? SOLAR_OS_MEDIA_TRANSPORT_PAUSE : SOLAR_OS_MEDIA_TRANSPORT_STOP);
}

static void player_render_list(solar_os_gfx_t *gfx, int width, int height)
{
    solar_os_media_list_layout_t list;
    solar_os_media_list_layout(width, height, &list);
    const int list_y = list.top;
    const size_t visible = (size_t)list.visible;
    const size_t count = player.browsing ?
        solar_os_storage_browser_count(player.browser) : player.track_count;
    const size_t cursor = player.browsing ?
        solar_os_storage_browser_cursor(player.browser) : player.cursor;
    if (cursor < player.top) player.top = cursor;
    if (visible && cursor >= player.top + visible) player.top = cursor - visible + 1U;
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = player.top + row;
        if (index >= count) break;
        const int y = list_y + (int)row * list.row_height;
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
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8, list.row_height - 1);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }
        solar_os_gfx_set_font(gfx, list.compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_media_text_draw(gfx, 6, y + list.baseline, width - 12, label);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, list.compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_SMALL);
    solar_os_media_text_draw(gfx, 3, height - 2, width - 6,
                      list.compact ? (player.browsing ? "Enter open  Esc back" : "Enter play  A+ Del-") :
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
    char status[96];
    player_progress_text(status, sizeof(status));
    solar_os_media_player_status_draw(gfx, width, height, status);
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
    if (key == '<' || key == '>') {
        player_seek(key == '<' ? -1 : 1);
    } else if (player.mode == PLAYER_MODE_GRAPHICS && key == '\t') {
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
    if (event->type == SOLAR_OS_EVENT_POINTER && player.mode == PLAYER_MODE_GRAPHICS &&
        player.tab == PLAYER_TAB_PLAY && !player.browsing) {
        const solar_os_input_pointer_event_t *pointer = &event->data.pointer;
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return false;
        const int width = solar_os_gfx_width(gfx), height = solar_os_gfx_height(gfx);
        if (pointer->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
            player.pointer_x = pointer->x;
            player.pointer_y = pointer->y;
        } else {
            int x = player.pointer_x + pointer->delta_x, y = player.pointer_y + pointer->delta_y;
            player.pointer_x = x < 0 ? 0 : x >= width ? width - 1 : x;
            player.pointer_y = y < 0 ? 0 : y >= height ? height - 1 : y;
        }
        if (pointer->action == SOLAR_OS_INPUT_POINTER_PRESS &&
            (pointer->buttons & SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY)) {
            int button = solar_os_media_player_button_at(width, height, true,
                                                          player.pointer_x, player.pointer_y);
            if (button >= 0) {
                if (button == 0 || button == 4) player_play_offset(button == 0 ? -1 : 1);
                else if (button == 2) player_toggle_play_stop();
                else player_seek(button == 1 ? -1 : 1);
                player.redraw = true;
                player_render(ctx);
                return true;
            }
        }
        return false;
    }
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
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
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
