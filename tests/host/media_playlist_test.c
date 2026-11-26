#include "media_storage_fixture.h"
#include "solar_os.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_m3u.h"

static const solar_os_app_t app;
const solar_os_app_t *solar_os_application_descriptor(void) { return &app; }

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
    const struct { uint32_t magic; uint16_t version, count; uint32_t generation; } store = {
        .magic = 0x50534f53U, .version = 1, .count = 1, .generation = 42,
    };
    char track[SOLAR_OS_STORAGE_PATH_MAX] = {0}; snprintf(track, sizeof(track), "%s/legacy.mp3", root);
    assert(fwrite(&store, sizeof(store), 1, file) == 1);
    assert(fwrite(track, sizeof(track), 1, file) == 1); assert(!fclose(file));
    solar_os_media_playlist_t other;
    assert(solar_os_media_playlist_open(&other, SOLAR_OS_MEDIA_PLAYLIST_AUDIO, NULL, NULL) == ESP_OK);
    assert(other.count == 1 && !strcmp(other.entries[0].path, track));
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
    assert(solar_os_media_playlist_remove(&other, 0) == ESP_ERR_INVALID_STATE);
    assert(other.count == 1 && !strcmp(other.entries[0].path, track));
    assert(solar_os_media_playlist_refresh(&other) == ESP_OK);
    assert(other.count == 1 && !strcmp(other.entries[0].title, "Imported"));
    assert(solar_os_media_playlist_remove(&other, 0) == ESP_OK);
    assert(solar_os_player_playlist_init() == ESP_OK && solar_os_player_playlist_count() == 0);
    solar_os_media_playlist_close(&other);
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

static void native_playlist(void)
{
    const solar_os_native_playlist_api_v1_t *api = solar_os_native_m3u_get_service(
        SOLAR_OS_NATIVE_PLAYLIST_SERVICE, 1, sizeof(*api));
    assert(api && api->abi_version == 1 && api->struct_size == sizeof(*api));
    solar_os_native_handle_t handle = 1, other;
    assert(api->open((solar_os_media_playlist_kind_t)-1, NULL, &handle, NULL) ==
        SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !handle && !solar_os_native_handle_count());
    assert(!api->open(SOLAR_OS_MEDIA_PLAYLIST_VIDEO, text_fixture("a.mpg\nb.mpg\n"), &handle, NULL));
    assert(!api->open(SOLAR_OS_MEDIA_PLAYLIST_VIDEO, NULL, &other, NULL));
    size_t count;
    uint32_t revision, updated;
    assert(!api->info(other, &count, &revision) && count == 2);
    assert(!api->remove(handle, 0));
    assert(api->remove(other, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->refresh(other));
    assert(!api->info(other, &count, &updated) && count == 1 && updated != revision);
    solar_os_m3u_entry_t entry;
    assert(!api->get(other, 0, &entry) && strstr(entry.path, "/b.mpg"));
    strlcpy(entry.title, "Updated title", sizeof(entry.title));
    assert(!api->set(other, 0, &entry));
    assert(!api->add(other, "c.mpeg", NULL));
    assert(!api->import_m3u(other, text_fixture("#EXTINF:3,Replacement\nc.mpeg\n")));
    assert(!api->info(other, &count, NULL) && count == 1);
    assert(!api->get(other, 0, &entry) && !strcmp(entry.title, "Replacement"));
    assert(!api->close(other));
    assert(api->get(other, 0, &entry) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(solar_os_native_handle_count() == 1);
    assert(!solar_os_native_handle_close_owner(&app) && !solar_os_native_handle_count());
    assert(api->refresh(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}

int main(void)
{
    assert(mkdtemp(root));
    parsing_and_rollback(); legacy_player(); video_and_rtsp(); native_playlist();
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
