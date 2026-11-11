#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <freertos/semphr.h>
#include "solar_os_media_preferences.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_log.h"

static char root[] = "/tmp/solaros-media-prefs-XXXXXX";
static bool mounted = true, fail_sync, fail_publish, fail_device, fail_memory, fail_mutex;
static unsigned allocations, commits, device_calls, write_attempts, warnings;
static uint8_t global_volume = 91, speaker_volume, bluetooth_volume;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (fail_mutex) return NULL;
    SemaphoreHandle_t lock = malloc(sizeof(*lock)); assert(lock);
    assert(!pthread_mutex_init(lock, NULL)); return lock;
}
int xSemaphoreTake(SemaphoreHandle_t lock, uint32_t ticks)
{ (void)ticks; assert(!pthread_mutex_lock(lock)); return 1; }
int xSemaphoreGive(SemaphoreHandle_t lock) { assert(!pthread_mutex_unlock(lock)); return 1; }
void vSemaphoreDelete(SemaphoreHandle_t lock) { assert(!pthread_mutex_destroy(lock)); free(lock); }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)tag; assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    if (fail_memory) return NULL;
    void *p = calloc(n, size); assert(p); allocations++; return p;
}
void solar_os_memory_free(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; warnings++; return ESP_OK; }
size_t strlcpy(char *out, const char *in, size_t size)
{
    size_t len = strlen(in);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(out, in, n); out[n] = 0; }
    return len;
}
esp_err_t solar_os_audio_set_device_volume(const char *id, uint8_t volume)
{
    device_calls++;
    if (fail_device) return ESP_FAIL;
    if (!strcmp(id, "speaker")) speaker_volume = volume;
    else { assert(!strcmp(id, "bluetooth")); bluetooth_volume = volume; }
    return ESP_OK;
}
esp_err_t solar_os_audio_set_volume(uint8_t volume) { (void)volume; assert(!"global volume must not change"); return ESP_FAIL; }
void solar_os_audio_get_status(solar_os_audio_status_t *status)
{ (void)status; assert(!"player defaults must not read global volume"); }
esp_err_t solar_os_storage_default_path(const char *name, char *path, size_t size)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;
    return snprintf(path, size, "%s/%s", root, name) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t solar_os_storage_mkdir(const char *path)
{ write_attempts++; return !mkdir(path, 0700) ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_sibling_path(const char *path, const char *suffix, char *out, size_t size)
{ return snprintf(out, size, "%s%s", path, suffix) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_sync_file(FILE *file)
{ write_attempts++; return !fail_sync && !fflush(file) && !fsync(fileno(file)) ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_remove(const char *path)
{ write_attempts++; return !unlink(path) ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_replace_file(const char *staged, const char *active, const char *backup)
{
    write_attempts++;
    if (fail_publish) return ESP_FAIL;
    bool existed = access(active, F_OK) == 0;
    if (existed && rename(active, backup)) return ESP_FAIL;
    if (rename(staged, active)) { if (existed) assert(!rename(backup, active)); return ESP_FAIL; }
    if (existed) assert(!unlink(backup));
    commits++; return ESP_OK;
}

typedef enum { PLAYER_VISUALIZER_CASSETTE, PLAYER_VISUALIZER_SCOPE, PLAYER_VISUALIZER_SPECTRUM,
    PLAYER_VISUALIZER_COUNT } player_visualizer_t;
typedef enum { WEBRADIO_VISUALIZER_SCOPE, WEBRADIO_VISUALIZER_SPECTRUM } webradio_visualizer_t;
#define PLAYER_VOLUME_STEP 5
#define WEBRADIO_VOLUME_STEP 5
static struct { solar_os_media_preferences_t *preferences; uint8_t volume; player_visualizer_t visualizer; } player;
static struct { solar_os_media_preferences_t *preferences; uint8_t volume; webradio_visualizer_t visualizer; } webradio;
static char player_message[96];
static void player_set_message(const char *message)
{ snprintf(player_message, sizeof(player_message), "%s", message ? message : ""); }
static void webradio_set_ui_message(const char *message) { (void)message; }
#include "media_player_preference_controls.inc"

static void player_controls_restore_test(void)
{
    unsigned writes = write_attempts, saves = commits;
    assert(player_load_preferences() == ESP_OK && player.volume == 50 && player.visualizer == PLAYER_VISUALIZER_CASSETTE);
    assert(webradio_load_preferences() == ESP_OK && webradio.volume == 30 && webradio.visualizer == WEBRADIO_VISUALIZER_SPECTRUM);
    player_cycle_visualizer(); webradio_cycle_visualizer();
    player_adjust_volume(1); webradio_adjust_volume(1);
    assert(player.volume == 55 && webradio.volume == 35);
    fail_sync = true; player_adjust_volume(1);
    assert(player.volume == 60 && !player_message[0]);
    assert(write_attempts == writes && commits == saves);
    fail_sync = false;
    solar_os_media_preferences_destroy(player.preferences);
    solar_os_media_preferences_destroy(webradio.preferences);
    assert(commits == saves + 2);
    writes = write_attempts;
    memset(&player, 0, sizeof(player)); memset(&webradio, 0, sizeof(webradio));
    assert(player_load_preferences() == ESP_OK && player.volume == 60 && player.visualizer == PLAYER_VISUALIZER_SCOPE);
    assert(webradio_load_preferences() == ESP_OK && webradio.volume == 35 && webradio.visualizer == WEBRADIO_VISUALIZER_SCOPE);
    assert(global_volume == 0);
    solar_os_media_preferences_destroy(player.preferences);
    solar_os_media_preferences_destroy(webradio.preferences);
    assert(write_attempts == writes && commits == saves + 2);
}

int main(void)
{
    assert(mkdtemp(root));
    char path[160]; snprintf(path, sizeof(path), "%s/.settings", root);
    solar_os_media_preferences_t *settings[4] = {0};
    const uint8_t first_volumes[] = {20, 30, 40, 60};
    fail_memory = true;
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_PLAYER, &settings[0]) == ESP_ERR_NO_MEM);
    fail_memory = false; fail_mutex = true;
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_PLAYER, &settings[0]) == ESP_ERR_NO_MEM && !allocations);
    fail_mutex = false;
    for (int i = 0; i < 4; i++) {
        assert(solar_os_media_preferences_create(i, &settings[i]) == ESP_OK);
        assert(solar_os_media_preferences_volume(settings[i]) == 50);
        assert(solar_os_media_preferences_visualizer(settings[i]) == 0);
        for (uint8_t volume = 0; volume <= 100; volume += 5)
            assert(solar_os_media_preferences_set_volume(settings[i], volume) == ESP_OK);
        assert(solar_os_media_preferences_set_volume(settings[i], first_volumes[i]) == ESP_OK);
    }
    assert(!device_calls && !commits && !write_attempts && global_volume == 91);
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    assert(solar_os_media_preferences_set_visualizer(settings[0], 2) == ESP_OK);
    assert(solar_os_media_preferences_set_visualizer(settings[1], 1) == ESP_OK);
    assert(solar_os_media_preferences_set_visualizer(settings[1], 1) == ESP_OK);
    assert(solar_os_media_preferences_set_volume(settings[0], 101) == ESP_ERR_INVALID_ARG);
    assert(solar_os_media_preferences_set_visualizer(settings[0], 3) == ESP_ERR_INVALID_ARG);
    assert(solar_os_media_preferences_set_visualizer(settings[1], 2) == ESP_ERR_INVALID_ARG);
    assert(solar_os_media_preferences_set_visualizer(settings[2], 1) == ESP_ERR_INVALID_ARG);
    assert(!write_attempts);
    solar_os_audio_device_info_t speaker = {.id = "speaker"}, bluetooth = {.id = "bluetooth"};
    solar_os_media_preferences_device(&speaker, settings[0]);
    solar_os_media_preferences_device(&bluetooth, settings[1]);
    assert(speaker_volume == 20 && bluetooth_volume == 30);
    assert(solar_os_media_preferences_set_volume(settings[0], 45) == ESP_OK);
    assert(speaker_volume == 45 && bluetooth_volume == 30 && global_volume == 91);
    fail_device = true;
    assert(solar_os_media_preferences_set_volume(settings[0], 60) == ESP_FAIL);
    assert(solar_os_media_preferences_volume(settings[0]) == 45 && !write_attempts);
    fail_device = false;
    solar_os_media_preferences_device(NULL, settings[0]);
    unsigned previous_calls = device_calls;
    assert(solar_os_media_preferences_set_volume(settings[0], 55) == ESP_OK);
    assert(device_calls == previous_calls && speaker_volume == 45); /* Idle settings cannot change another output. */
    solar_os_media_preferences_device(NULL, settings[1]);
    assert(!write_attempts); /* Starting/stopping tracks must not save settings. */
    for (int i = 0; i < 4; i++) {
        solar_os_media_preferences_destroy(settings[i]);
        assert(commits == (unsigned)i + 1); /* One combined volume/visualizer save per app exit. */
    }
    unsigned writes = write_attempts, saves = commits;
    for (int i = 0; i < 4; i++) {
        assert(solar_os_media_preferences_create(i, &settings[i]) == ESP_OK);
        assert(solar_os_media_preferences_volume(settings[i]) == (i == 0 ? 55 : first_volumes[i]));
        assert(solar_os_media_preferences_visualizer(settings[i]) == (i == 0 ? 2 : i == 1 ? 1 : 0));
        assert(solar_os_media_preferences_set_volume(settings[i], solar_os_media_preferences_volume(settings[i])) == ESP_OK);
        solar_os_media_preferences_destroy(settings[i]);
    }
    assert(write_attempts == writes && commits == saves); /* Unchanged sessions do not write at exit. */

    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_PLAYER, &settings[0]) == ESP_OK);
    fail_sync = true;
    assert(solar_os_media_preferences_set_volume(settings[0], 70) == ESP_OK);
    assert(solar_os_media_preferences_set_visualizer(settings[0], 1) == ESP_OK);
    assert(solar_os_media_preferences_volume(settings[0]) == 70);
    solar_os_media_preferences_t *saved;
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_PLAYER, &saved) == ESP_OK);
    assert(solar_os_media_preferences_volume(saved) == 55 && solar_os_media_preferences_visualizer(saved) == 2);
    solar_os_media_preferences_destroy(saved);
    assert(write_attempts == writes && commits == saves); /* Live choices have not reached disk. */
    solar_os_media_preferences_destroy(settings[0]);
    assert(commits == saves && warnings == 1);
    fail_sync = false;
    global_volume = 0;
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_PLAYER, &settings[0]) == ESP_OK);
    assert(solar_os_media_preferences_volume(settings[0]) == 55);
    assert(solar_os_media_preferences_visualizer(settings[0]) == 2);
    fail_publish = true;
    writes = write_attempts;
    assert(solar_os_media_preferences_set_visualizer(settings[0], 1) == ESP_OK);
    assert(write_attempts == writes);
    solar_os_media_preferences_destroy(settings[0]); fail_publish = false;
    assert(commits == saves && warnings == 2);
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_PLAYER, &settings[0]) == ESP_OK);
    assert(solar_os_media_preferences_visualizer(settings[0]) == 2);
    solar_os_media_preferences_destroy(settings[0]);
    mounted = false;
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_RTSP, &settings[0]) == ESP_OK);
    writes = write_attempts;
    assert(solar_os_media_preferences_set_volume(settings[0], 0) == ESP_OK);
    assert(solar_os_media_preferences_volume(settings[0]) == 0);
    solar_os_media_preferences_destroy(settings[0]);
    assert(write_attempts == writes && commits == saves && warnings == 3);
    mounted = true;
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_RTSP, &settings[0]) == ESP_OK);
    assert(solar_os_media_preferences_volume(settings[0]) == 60); /* Unavailable storage at exit preserved the old setting. */
    solar_os_media_preferences_destroy(settings[0]);
    mounted = false;
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_RTSP, &settings[0]) == ESP_OK);
    assert(solar_os_media_preferences_set_volume(settings[0], 0) == ESP_OK);
    mounted = true;
    solar_os_media_preferences_destroy(settings[0]);
    assert(commits == saves + 1); /* Storage need only be available when exiting. */
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_RTSP, &settings[0]) == ESP_OK);
    assert(solar_os_media_preferences_volume(settings[0]) == 0);
    solar_os_media_preferences_destroy(settings[0]);
    snprintf(path, sizeof(path), "%s/.settings/media-player.bin", root);
    FILE *file = fopen(path, "wb"); assert(file); assert(fputs("corrupt", file) >= 0); assert(!fclose(file));
    assert(solar_os_media_preferences_create(SOLAR_OS_MEDIA_PLAYER, &settings[0]) == ESP_OK);
    assert(solar_os_media_preferences_volume(settings[0]) == 50 && solar_os_media_preferences_visualizer(settings[0]) == 0);
    solar_os_media_preferences_destroy(settings[0]);
    player_controls_restore_test();
    const char *names[] = {"player", "webradio", "vplay", "rtsp"};
    for (size_t i = 0; i < 4; i++) { snprintf(path, sizeof(path), "%s/.settings/media-%s.bin", root, names[i]); assert(!unlink(path)); }
    snprintf(path, sizeof(path), "%s/.settings", root); assert(!rmdir(path)); assert(!rmdir(root));
    assert(!allocations);
    puts("Per-player volume and visualizers: no writes during playback, one save on exit, reload and failed saves passed");
}
