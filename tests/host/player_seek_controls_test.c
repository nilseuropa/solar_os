#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_audio.h"
#include "media_preferences_mock.h"

enum { PLAYER_SEEKING, PLAYER_PAUSED, PLAYER_PLAYING };
static struct {
    void *task;
    bool task_done, paused, seeking, redraw;
    int playback_state;
    uint32_t elapsed_ms, total_ms;
    uint32_t sample_rate, start_ms, active_device_capabilities;
    uint64_t played_frames;
    char active_device_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    size_t active_index;
    solar_os_media_preferences_t *preferences;
} player;
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
static unsigned restarts;
static uint32_t start_ms;
static bool restart_paused;
static esp_err_t player_play_index_at(size_t index, uint32_t target, bool paused)
{
    assert(index == player.active_index);
    restarts++; start_ms = target; restart_paused = paused;
    return ESP_OK;
}
static void player_set_message(const char *message) { (void)message; }
static size_t strlcpy(char *dest, const char *source, size_t capacity)
{
    snprintf(dest, capacity, "%s", source);
    return strlen(source);
}

typedef struct {
    bool stopped, done, paused;
    const char *path;
    void *mutex;
    int64_t clock;
} mpeg_player_t;
static struct { mpeg_player_t *player; double restart_time; bool restart_paused; } vplay_state;
#define portMAX_DELAY 0
static void xSemaphoreTake(void *lock, int delay) { (void)lock; (void)delay; }
static void xSemaphoreGive(void *lock) { (void)lock; }
static int64_t clock_locked(mpeg_player_t *p) { return p->clock; }
static void vplay_restart(const char *path)
{
    assert(path == vplay_state.player->path);
    restarts++;
    vplay_state.player->stopped = true;
    /* Closing the old worker can clear pause: seek must snapshot it first. */
    vplay_state.player->paused = false;
}
#include "player_seek_controls.inc"

int main(void)
{
    player.task = &player;
    player.seeking = true;
    player.playback_state = PLAYER_SEEKING;
    assert(strcmp(player_state_symbol(), "SEEKING") == 0);
    player_toggle_pause();
    assert(player.paused && strcmp(player_state_symbol(), "||") == 0);
    player_toggle_pause();
    assert(!player.paused && player.playback_state == PLAYER_SEEKING);
    player.seeking = false;
    player_toggle_pause();
    player_toggle_pause();
    assert(player.playback_state == PLAYER_PLAYING);
    solar_os_audio_device_info_t device = {.id = "sink", .native_format = {.sample_rate = 16000}};
    player_device_callback(&device, NULL);
    solar_os_audio_wav_progress_t progress = {.info = {.sample_rate = 44100, .duration_ms = 12000}};
    player_progress_callback(&progress, NULL);
    assert(player.sample_rate == 16000 && player.elapsed_ms == 12000);
    player.played_frames = 16000;
    player.elapsed_ms = 11000;
    player_progress_callback(&progress, NULL);
    assert(player.elapsed_ms == 11000); /* queued source progress is not audible position */
    player.task = &player;
    player.active_index = 7;
    player.total_ms = 30000;
    player.elapsed_ms = 5000;
    player.paused = true;
    player_seek(-1);
    assert(restarts == 1 && start_ms == 0 && restart_paused);
    player_seek(1);
    assert(start_ms == 15000 && restart_paused);
    player.elapsed_ms = 28000;
    player_seek(1);
    assert(start_ms == 29999);
    player.total_ms = 0; /* MP3 has no known duration. */
    player_seek(1);
    assert(start_ms == 38000);
    player.elapsed_ms = UINT32_MAX - 1;
    player_seek(1);
    assert(start_ms == UINT32_MAX);
    player.task_done = true;
    unsigned before = restarts;
    player_seek(-1);
    assert(before == restarts);

    mpeg_player_t video = {.path = "clip.mpg", .clock = 25000000, .paused = true};
    vplay_state.player = &video;
    vplay_seek(-1);
    assert(vplay_state.restart_time == 15 && vplay_state.restart_paused);
    video.stopped = false;
    video.clock = 3000000;
    video.paused = false;
    vplay_seek(-1);
    assert(vplay_state.restart_time == 0 && !vplay_state.restart_paused);
    video.stopped = false;
    vplay_seek(1);
    assert(vplay_state.restart_time == 13);
    before = restarts;
    vplay_seek(1);
    assert(before == restarts);
    puts("Player/VPlay seek targets, pause preservation, limits and stopped state passed");
}
