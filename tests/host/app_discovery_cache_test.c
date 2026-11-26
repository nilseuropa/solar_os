#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
#define SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES 1
#endif
#ifndef SOLAR_OS_NATIVE_FOREGROUND_ENABLED
#define SOLAR_OS_NATIVE_FOREGROUND_ENABLED SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
#endif
#define SOLAR_OS_MODULE_PACKAGE_COUNT_MAX 256U
#define SOLAR_OS_MODULE_PACKAGE_ID_MAX 32U
#define SOLAR_OS_STORAGE_PATH_MAX 256U
#define SOLAR_OS_MODULE_TYPE_APP 1
#define SOLAR_OS_MEMORY_TRANSIENT 1
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 2
#define SOLAR_OS_APP_STATIC_SRAM_EXCEPTION(reason)
#define EXT_RAM_BSS_ATTR
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106

static unsigned locks;
#define portENTER_CRITICAL(lock) ((void)(lock), assert(locks++ == 0))
#define portEXIT_CRITICAL(lock) ((void)(lock), assert(--locks == 0))

typedef struct { const char *name; } solar_os_app_registry_entry_t;
static const solar_os_app_registry_entry_t registered_apps[] = {{"clock"}, {"edit"}};
static const size_t registered_app_count = 2;
static const solar_os_app_registry_entry_t external[] = {{"apple2"}, {"clock"}, {"tiny386"}};
typedef struct { int type; const char *id; } solar_os_component_t;
static bool compiled_app;
static const solar_os_component_t component = {SOLAR_OS_MODULE_TYPE_APP, "tiny386"};
static size_t solar_os_component_count(void) { return compiled_app ? 1U : 0U; }
static const solar_os_component_t *solar_os_component_get(size_t index)
{ return compiled_app && !index ? &component : NULL; }
static uint32_t revision;
static int64_t now;
static bool mounted = true, fail_alloc, fail_scan, racing_change, reentrant;
static bool apple2_installed = true, tiny386_installed;
static const char *root = "/sdcard/modules/apps";
static unsigned scans, file_checks;

static size_t strlcpy(char *out, const char *in, size_t size)
{
    size_t length = strlen(in);
    if (size) {
        size_t count = length < size - 1 ? length : size - 1;
        memcpy(out, in, count);
        out[count] = 0;
    }
    return length;
}
static uint32_t solar_os_storage_revision(void) { return revision; }
static int64_t esp_timer_get_time(void) { return now; }
static bool solar_os_storage_is_mounted(void) { return mounted; }
static esp_err_t solar_os_storage_default_path(const char *relative, char *out, size_t size)
{
    assert(!strcmp(relative, "modules/apps"));
    strlcpy(out, root, size);
    return ESP_OK;
}
static void *solar_os_memory_calloc(size_t count, size_t size, int kind, const char *tag)
{
    assert(!locks && kind == SOLAR_OS_MEMORY_TRANSIENT && !strcmp(tag, "app.discovery"));
    return fail_alloc ? NULL : calloc(count, size);
}
static void *solar_os_memory_alloc(size_t bytes, int kind, const char *tag)
{ assert(!locks && kind == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED && tag); return fail_alloc ? NULL : malloc(bytes); }
static void solar_os_memory_free(void *p) { assert(!locks); free(p); }
static const solar_os_app_registry_entry_t *solar_os_native_foreground_find(const char *id)
{
    assert(!locks);
    file_checks++;
    for (size_t i = 0; i < 3; ++i) if (!strcmp(id, external[i].name)) return &external[i];
    return NULL; /* lifecycle-1 command, not a foreground app */
}
size_t solar_os_app_registry_count(void);
static esp_err_t solar_os_module_package_foreach_installed(int type,
    bool (*visit)(const char *, void *), void *user)
{
    assert(!locks && type == SOLAR_OS_MODULE_TYPE_APP);
    scans++;
    if (reentrant) {
        unsigned previous = scans;
        (void)solar_os_app_registry_count();
        assert(scans == previous);
    }
    if (racing_change) { revision++; racing_change = false; }
    if (fail_scan) return ESP_FAIL;
    if (apple2_installed && !visit("apple2", user)) return ESP_OK;
    if (!visit("clock", user)) return ESP_OK;
    if (!visit("hello", user)) return ESP_OK;
    if (tiny386_installed) (void)visit("tiny386", user);
    return ESP_OK;
}

#include "app_discovery.inc"

static unsigned commands;
static bool command_visit(const char *id, void *user)
{
    assert(!locks && user == &commands);
    assert(!strcmp(id, "apple2") || !strcmp(id, "clock") ||
           !strcmp(id, "hello") || !strcmp(id, "tiny386"));
    commands++;
    /* Mirrors completion's shadowing lookup; must not deadlock or touch files. */
    (void)solar_os_app_registry_find(id);
    return true;
}
static bool stop_visit(const char *id, void *user)
{
    (void)id; (void)user;
    commands++;
    return false;
}

int main(void)
{
#if SOLAR_OS_NATIVE_FOREGROUND_ENABLED && !SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    compiled_app = true; mounted = false; root = "";
    assert(solar_os_app_registry_count() == 3 && !scans);
    assert(solar_os_app_registry_find("tiny386") == &external[2]);
    solar_os_app_registry_foreach_module(command_visit, &commands);
    assert(commands == 1 && !scans);
    fail_alloc = true;
    solar_os_app_registry_foreach_module(command_visit, &commands);
    assert(commands == 1);
    fail_alloc = false;
    puts("compiled foreground discovery works without ELF loading or storage");
    return 0;
#elif !SOLAR_OS_NATIVE_FOREGROUND_ENABLED
    assert(solar_os_app_registry_count() == 2);
    assert(solar_os_app_registry_get(0) == &registered_apps[0]);
    assert(solar_os_app_registry_get(1) == &registered_apps[1]);
    assert(!solar_os_app_registry_get(2));
    assert(solar_os_app_registry_find("edit") == &registered_apps[1]);
    assert(!solar_os_app_registry_find("apple2"));
    solar_os_app_registry_foreach_module(command_visit, &commands);
    assert(commands == 0 && scans == 0 && file_checks == 0);
    return 0;
#else
    assert(solar_os_app_registry_count() == 3);
    assert(scans == 1 && file_checks == 3);
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
        for (size_t i = 0; i < solar_os_app_registry_count(); ++i)
            assert(solar_os_app_registry_get(i));
        assert(solar_os_app_registry_get(0) == &external[1]); /* override */
        assert(solar_os_app_registry_get(1) == &registered_apps[1]);
        assert(solar_os_app_registry_get(2) == &external[0]);
        assert(!solar_os_app_registry_get(3));
        assert(solar_os_app_registry_find("apple2") == &external[0]);
        assert(solar_os_app_registry_find("edit") == &registered_apps[1]);
        assert(!solar_os_app_registry_find("hello"));
        assert(!solar_os_app_registry_find("absent"));
        assert(!solar_os_app_registry_find(NULL));
        commands = 0;
        solar_os_app_registry_foreach_module(command_visit, &commands);
        assert(commands == 3); /* legacy command is retained, no duplicate foreground app */
    }
    assert(scans == 1 && file_checks == 3);
    commands = 0;
    assert(solar_os_app_registry_foreach_module(stop_visit, NULL) == ESP_OK);
    assert(commands == 1);
    assert(solar_os_app_registry_foreach_module(NULL, NULL) == ESP_ERR_INVALID_ARG);

    apple2_installed = false; tiny386_installed = true; revision++;
    assert(solar_os_app_registry_count() == 3);
    assert(!solar_os_app_registry_find("apple2"));
    assert(solar_os_app_registry_find("tiny386") == &external[2]);
    assert(scans == 2);

    /* Unmount and a default-root change invalidate even without a notification. */
    mounted = false;
    assert(solar_os_app_registry_count() == 2 && scans == 2);
    assert(!solar_os_app_registry_find("tiny386"));
    mounted = true; root = "/flash/modules/apps";
    assert(solar_os_app_registry_count() == 3 && scans == 3);
    root = "/sdcard/modules/apps";
    assert(solar_os_app_registry_count() == 3 && scans == 4);

    /* Raw VFS changes are picked up by the periodic fallback. */
    apple2_installed = true;
    assert(!solar_os_app_registry_find("apple2"));
    now += 30000000LL;
    assert(solar_os_app_registry_find("apple2") == &external[0] && scans == 5);

    /* A mutation during discovery cannot publish a partial snapshot. */
    revision++; racing_change = true; reentrant = true;
    assert(solar_os_app_registry_count() == 4 && scans == 6);
    assert(solar_os_app_registry_count() == 4 && scans == 7);
    reentrant = false;

    fail_scan = true; revision++;
    assert(solar_os_app_registry_count() == 2 && scans == 8);
    for (unsigned i = 0; i < 100; ++i) assert(solar_os_app_registry_count() == 2);
    assert(scans == 8);
    fail_scan = false; now += 1000000LL;
    assert(solar_os_app_registry_count() == 4 && scans == 9);

    fail_alloc = true; revision++;
    assert(solar_os_app_registry_count() == 2);
    fail_alloc = false;
    assert(solar_os_app_registry_count() == 2 && scans == 9);
    now += 1000000LL;
    assert(solar_os_app_registry_count() == 4 && scans == 10);
    compiled_app = true; mounted = false; root = ""; ++revision;
    assert(solar_os_app_registry_count() == 3 && scans == 10);
    assert(solar_os_app_registry_find("tiny386") == &external[2]);
    commands = 0;
    solar_os_app_registry_foreach_module(command_visit, &commands);
    assert(commands == 1);
    mounted = true; root = "/sdcard/modules/apps"; fail_scan = true; ++revision;
    assert(solar_os_app_registry_count() == 3 && scans == 11);
    puts("app discovery: zero warm-cache file operations, overrides, commands, refresh and recovery passed");
    return 0;
#endif
}
