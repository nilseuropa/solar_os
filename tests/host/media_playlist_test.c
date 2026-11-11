#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_media_playlist.h"
#include "solar_os_memory.h"
#include "freertos/semphr.h"

static char root[] = "/tmp/solaros-playlist-XXXXXX";
static bool mounted = true, fail_save;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int mutex; return &mutex; }
#include "../../src/services/solar_os_player_playlist.c"

size_t strlcpy(char *out, const char *text, size_t size)
{
    size_t len = strlen(text);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(out, text, n); out[n] = 0; }
    return len;
}
void *solar_os_memory_realloc(void *ptr, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return realloc(ptr, size); }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(n, size); }
void solar_os_memory_free(void *ptr) { free(ptr); }
esp_err_t solar_os_storage_default_path(const char *relative, char *path, size_t size)
{
    if (!mounted) return ESP_ERR_INVALID_STATE;
    return snprintf(path, size, "%s/%s", root, relative) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
esp_err_t solar_os_storage_join_path(const char *base, const char *relative, char *path, size_t size)
{ return snprintf(path, size, "%s/%s", base, relative) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_resolve_path_at(const char *cwd, const char *arg, char *path, size_t size)
{
    char raw[512];
    int n = arg[0] == '/' ? snprintf(raw, sizeof(raw), "%s", arg) :
        snprintf(raw, sizeof(raw), "%s/%s", cwd ? cwd : root, arg);
    if (n < 0 || n >= SOLAR_OS_STORAGE_PATH_MAX) return ESP_ERR_INVALID_SIZE;
    char *parts[80], *save, *part; size_t count = 0;
    for (part = strtok_r(raw, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (count) count--; }
        else parts[count++] = part;
    }
    size_t used = 0;
    path[0] = 0;
    for (size_t i = 0; i < count; i++) {
        n = snprintf(path + used, size - used, "/%s", parts[i]);
        if (n < 0 || (size_t)n >= size - used) return ESP_ERR_INVALID_SIZE;
        used += n;
    }
    return ESP_OK;
}
esp_err_t solar_os_storage_resolve_path(const char *arg, char *path, size_t size)
{ return solar_os_storage_resolve_path_at(root, arg, path, size); }
esp_err_t solar_os_storage_makedirs(const char *path, bool exist_ok)
{ return mkdir(path, 0700) == 0 || (exist_ok && errno == EEXIST) ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_exists(const char *path, bool *exists)
{ struct stat st; *exists = stat(path, &st) == 0; return ESP_OK; }
esp_err_t solar_os_storage_sibling_path(const char *path, const char *suffix, char *out, size_t size)
{ return snprintf(out, size, "%s%s", path, suffix) < (int)size ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_sync_file(FILE *file)
{ return fflush(file) == 0 && fsync(fileno(file)) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_remove(const char *path) { return unlink(path) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_replace_file(const char *staged, const char *active, const char *backup)
{
    if (fail_save) return ESP_FAIL;
    struct stat st; bool exists = stat(active, &st) == 0;
    if (exists && rename(active, backup)) return ESP_FAIL;
    if (rename(staged, active)) { if (exists) assert(rename(backup, active) == 0); return ESP_FAIL; }
    if (exists) assert(unlink(backup) == 0);
    return ESP_OK;
}

static char *fixture(const void *data, size_t len)
{
    static char path[160];
    snprintf(path, sizeof(path), "%s/input.m3u", root);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(data, 1, len, file) == len); assert(!fclose(file));
    return path;
}
static const char *text_fixture(const char *text) { return fixture(text, strlen(text)); }

static void parsing_and_rollback(void)
{
    solar_os_media_playlist_t list;
    const char *input = text_fixture("\xef\xbb\xbf#EXTM3U\r\n# comment\r\n"
        "#EXTINF:12.5,T\xc3\xb6ne, two\r\n./music/../first song.mp3\r\n\r\nsecond.wav");
    assert(solar_os_media_playlist_open(&list, SOLAR_OS_MEDIA_PLAYLIST_AUDIO, input, NULL) == ESP_OK);
    assert(list.count == 2 && list.entries[0].duration == 12 && list.entries[1].duration == -1);
    char expected[160]; snprintf(expected, sizeof(expected), "%s/first song.mp3", root);
    assert(!strcmp(list.entries[0].path, expected));
    assert(!strcmp(list.entries[0].title, "T\xc3\xb6ne, two"));
    assert(!list.entries[1].title[0]);
    input = text_fixture("first.mp3\nhttps://example.com/wrong.mp3\n");
    assert(solar_os_media_playlist_import(&list, input) == ESP_ERR_NOT_SUPPORTED);
    assert(list.count == 2 && !strcmp(list.entries[0].path, expected));
    input = text_fixture("#EXTM3U\n#EXT-X-TARGETDURATION:10\nsegment.mp3\n");
    assert(solar_os_media_playlist_import(&list, input) == ESP_ERR_NOT_SUPPORTED);
    input = text_fixture("#EXTINF:nonsense,title\nfirst.mp3\n");
    assert(solar_os_media_playlist_import(&list, input) == ESP_ERR_INVALID_ARG);
    input = text_fixture("#EXTINF:nan,title\nfirst.mp3\n");
    assert(solar_os_media_playlist_import(&list, input) == ESP_ERR_INVALID_ARG);
    const char nul[] = "first.mp3\0hidden\n";
    input = fixture(nul, sizeof(nul) - 1);
    assert(solar_os_media_playlist_import(&list, input) == ESP_ERR_INVALID_ARG);
    char large[1100]; memset(large, 'a', sizeof(large)); large[sizeof(large) - 1] = 0;
    input = text_fixture(large);
    assert(solar_os_media_playlist_import(&list, input) == ESP_ERR_INVALID_SIZE);
    fail_save = true;
    input = text_fixture("replacement.wav\n");
    assert(solar_os_media_playlist_import(&list, input) == ESP_FAIL);
    assert(solar_os_media_playlist_add(&list, "new.mp3", NULL) == ESP_FAIL);
    assert(solar_os_media_playlist_remove(&list, 0) == ESP_FAIL);
    assert(list.count == 2 && !strcmp(list.entries[0].path, expected));
    fail_save = false;
    solar_os_media_playlist_close(&list);
    assert(solar_os_media_playlist_open(&list, SOLAR_OS_MEDIA_PLAYLIST_AUDIO, NULL, NULL) == ESP_OK);
    assert(list.count == 2 && !strcmp(list.entries[0].path, expected));
    char uri[300]; snprintf(uri, sizeof(uri), "file://%s/third%%20song.mp3\n", root);
    assert(solar_os_media_playlist_import(&list, text_fixture(uri)) == ESP_OK);
    assert(strstr(list.entries[0].path, "/third song.mp3"));
    char many[1024] = {0};
    for (int i = 0; i < 33; i++) strcat(many, "same.wav\n");
    assert(solar_os_media_playlist_import(&list, text_fixture(many)) == ESP_ERR_INVALID_SIZE);
    assert(list.count == 1);
    assert(solar_os_media_playlist_import(&list, text_fixture("#EXTM3U\n")) == ESP_OK);
    assert(list.count == 0);
    solar_os_media_playlist_close(&list);
}

static void legacy_player(void)
{
    char path[160]; snprintf(path, sizeof(path), "%s/.player/playlist.m3u", root); assert(!unlink(path));
    snprintf(path, sizeof(path), "%s/.player/playlist.bin", root);
    FILE *file = fopen(path, "wb"); assert(file);
    player_playlist_store_t store = {.magic = 0x50534f53U, .version = 1, .count = 1, .generation = 42};
    char track[SOLAR_OS_STORAGE_PATH_MAX] = {0}; snprintf(track, sizeof(track), "%s/legacy.mp3", root);
    assert(fwrite(&store, sizeof(store), 1, file) == 1);
    assert(fwrite(track, sizeof(track), 1, file) == 1); assert(!fclose(file));
    assert(solar_os_player_playlist_init() == ESP_OK);
    solar_os_player_track_t loaded;
    assert(solar_os_player_playlist_count() == 1 && solar_os_player_playlist_get(0, &loaded));
    assert(!strcmp(loaded.path, track));
    assert(access(path, F_OK) == 0); /* Legacy backup remains. */
    fail_save = true;
    assert(solar_os_player_playlist_remove(0) == ESP_FAIL);
    assert(solar_os_player_playlist_count() == 1);
    fail_save = false;
    assert(solar_os_player_playlist_add(text_fixture("#EXTINF:4,Imported\nnew.wav\n"), NULL) == ESP_OK);
    assert(solar_os_player_playlist_get(0, &loaded) && !strcmp(loaded.title, "Imported"));
    solar_os_media_playlist_close(&playlist);
}

static void video_and_rtsp(void)
{
    solar_os_media_playlist_t list;
    size_t index;
    assert(solar_os_media_playlist_open(&list, SOLAR_OS_MEDIA_PLAYLIST_VIDEO,
        text_fixture("one.mpg\ntwo.mpeg\n"), &index) == ESP_OK);
    assert(list.count == 2 && index == 0);
    assert(solar_os_media_playlist_add(&list, list.entries[1].path, &index) == ESP_OK);
    assert(index == 1 && list.count == 2);
    assert(solar_os_media_playlist_remove(&list, 0) == ESP_OK && list.count == 1);
    solar_os_media_playlist_close(&list);
    assert(solar_os_media_playlist_open(&list, SOLAR_OS_MEDIA_PLAYLIST_RTSP,
        text_fixture("#EXTINF:-1,Camera\nrtsp://example.com/media\n"), &index) == ESP_OK);
    assert(list.count == 1 && !strcmp(list.entries[0].title, "Camera"));
    solar_os_m3u_entry_t entry = {.path = "rtsp://example.net/new", .title = "New camera", .duration = -1};
    fail_save = true;
    assert(solar_os_media_playlist_set(&list, 1, &entry) == ESP_FAIL && list.count == 1);
    assert(solar_os_media_playlist_set(&list, 0, &entry) == ESP_FAIL);
    assert(!strcmp(list.entries[0].title, "Camera"));
    fail_save = false;
    assert(solar_os_media_playlist_set(&list, 1, &entry) == ESP_OK && list.count == 2);
    assert(solar_os_media_playlist_set(&list, 0, &entry) == ESP_OK);
    solar_os_media_playlist_close(&list);
    assert(solar_os_media_playlist_open(&list, SOLAR_OS_MEDIA_PLAYLIST_RTSP, NULL, &index) == ESP_OK);
    assert(list.count == 2 && !strcmp(list.entries[0].title, "New camera"));
    solar_os_media_playlist_close(&list);
    mounted = false;
    assert(solar_os_media_playlist_open(&list, SOLAR_OS_MEDIA_PLAYLIST_RTSP, NULL, &index) == ESP_OK);
    assert(list.count == 0 && !list.path[0]);
    assert(solar_os_media_playlist_set(&list, 0, &entry) == ESP_OK && list.count == 1);
    solar_os_media_playlist_close(&list);
    assert(solar_os_media_playlist_open(&list, SOLAR_OS_MEDIA_PLAYLIST_RTSP,
        "rtsp://example.com/live", &index) == ESP_OK);
    assert(list.count == 1 && !list.path[0]);
    solar_os_media_playlist_close(&list); mounted = true;
}

int main(void)
{
    assert(mkdtemp(root));
    parsing_and_rollback(); legacy_player(); video_and_rtsp();
    const char *files[] = {"input.m3u", ".player/playlist.m3u", ".player/playlist.bin",
        ".vplay/playlist.m3u", ".rtsp/playlist.m3u"};
    char path[160];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i]); assert(!unlink(path));
    }
    const char *dirs[] = {".player", ".vplay", ".rtsp"};
    for (size_t i = 0; i < 3; i++) { snprintf(path, sizeof(path), "%s/%s", root, dirs[i]); assert(!rmdir(path)); }
    assert(!rmdir(root));
    puts("M3U parsing, migration, persistence and rollback: passed");
}
