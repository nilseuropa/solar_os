#include "media_storage_fixture.h"
#include <dirent.h>
#include "solar_os_native_m3u.h"
#include "solar_os_native_m3u_abi.h"

_Static_assert(__builtin_types_compatible_p(solar_os_m3u_read_fn, solar_os_native_m3u_accept_fn), "canonical callback signature");
typedef struct { solar_os_m3u_entry_t entries[2]; size_t count, calls; int result; } records_t;
static int accept(const solar_os_m3u_entry_t *entry, void *user)
{
    records_t *records = user;
    assert(records->calls < 2);
    records->entries[records->calls++] = *entry;
    return records->result;
}
static bool lookup(size_t index, solar_os_m3u_entry_t *entry, void *user)
{
    records_t *records = user;
    records->calls++;
    if (index >= records->count) return false;
    *entry = records->entries[index];
    return true;
}
static size_t file_descriptors(void)
{
    DIR *directory = opendir("/proc/self/fd"); assert(directory);
    size_t count = 0;
    while (readdir(directory)) count++;
    assert(!closedir(directory)); return count;
}

int main(void)
{
    const solar_os_native_m3u_api_v1_t *api = solar_os_native_m3u_get_service("media.m3u", 1, 8);
    assert(api && api->struct_size == sizeof(*api) && api->path == solar_os_m3u_path);
    assert(!solar_os_native_m3u_get_service("media.m3u", 2, 8));
    assert(!solar_os_native_m3u_get_service("media.m3u", 1, sizeof(*api) + 1));
    assert(!solar_os_native_m3u_get_service("other", 1, 8));
    assert(api->path("TRACKS.M3U8") && !api->path(NULL) && !api->path("track.mp3"));
    assert(mkdtemp(root));
    char path[160], expected[160]; snprintf(path, sizeof(path), "%s/output.m3u", root);
    snprintf(expected, sizeof(expected), "%s/first.mp3", root);
    records_t input = {.entries = {{.path="first.mp3", .title="T\xc3\xb6ne", .duration=3},
        {.path="rtsp://example.com/media", .duration=-1}}, .count=2};
    const size_t descriptors = file_descriptors();
    assert(!api->write(path, 2, lookup, &input) && input.calls == 2);
    records_t output = {0};
    assert(!api->read(path, accept, &output) && output.calls == 2);
    assert(!strcmp(output.entries[0].path, expected) && !strcmp(output.entries[0].title, "T\xc3\xb6ne"));
    assert(output.entries[0].duration == 3 && output.entries[1].duration == -1);
    const int errors[] = {SOLAR_OS_NATIVE_ERROR_FAILED, SOLAR_OS_NATIVE_ERROR_CANCELLED,
        SOLAR_OS_NATIVE_ERROR_BUSY, SOLAR_OS_NATIVE_ERROR_TIMEOUT, SOLAR_OS_NATIVE_ERROR_PERMISSION};
    for (size_t i = 0; i < sizeof(errors)/sizeof(*errors); i++) {
        output = (records_t){.result=errors[i]};
        assert(api->read(path, accept, &output) == errors[i] && output.calls == 1);
        assert(file_descriptors() == descriptors);
        assert(api->path(path) && output.calls == 1); /* No retained callback. */
    }
    fail_save = true;
    assert(api->write(path, 0, lookup, &input) == SOLAR_OS_NATIVE_ERROR_FAILED);
    fail_save = false;
    input.entries[0].path[0] = '#';
    assert(api->write(path, 2, lookup, &input) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    output = (records_t){0};
    assert(!api->read(path, accept, &output) && output.calls == 2 && !strcmp(output.entries[0].path, expected));
    assert(api->read(NULL, accept, &output) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->read(path, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->write(path, 0, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(file_descriptors() == descriptors);
    assert(api->read(text_fixture("#EXT-X-VERSION:3\ntrack.ts\n"), accept, &output) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(api->read("/missing/m3u/file", accept, &output) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!unlink(path)); snprintf(path, sizeof(path), "%s/input.m3u", root); assert(!unlink(path));
    assert(!rmdir(root));
    puts("native M3U: canonical records, copied entries, borrowed callbacks, cancellation, closed files and transactional writes passed");
    return 0;
}
