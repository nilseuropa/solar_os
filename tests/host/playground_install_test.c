/* Exercise the real installer and ZIP reader against a local HTTP fixture. */
#define main zoo_fixture_main
#include "zoo_test.c"
#undef main
#include <dirent.h>
#include "solar_os_board_caps.h"
#include "solar_os_app_registry.h"
#include "solar_os_components.h"
#include "solar_os_native_components.h"
#include "solar_os_native_zip.h"
#include "solar_os_native_zip_abi.h"
#include "solar_os_zip.h"
#include "solar_os_playground.h"

static bool lua_installed;

const solar_os_app_registry_entry_t *solar_os_app_registry_find(const char *name)
{
    static const solar_os_app_t runtime = {0};
    static const solar_os_app_registry_entry_t python = {.name = "python", .app = &runtime};
    static const solar_os_app_registry_entry_t lua = {.name = "lua", .app = &runtime};
    if (strcmp(name, "python") == 0) {
        return &python;
    }
    return lua_installed && strcmp(name, "lua") == 0 ? &lua : NULL;
}

size_t solar_os_component_count(void) { return 0; }
const solar_os_component_t *solar_os_component_get(size_t index) { (void)index; return NULL; }
const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type, const char *id)
{ (void)type; (void)id; return NULL; }

bool solar_os_board_has(solar_os_board_capability_t capability)
{
    (void)capability;
    return false;
}

const char *solar_os_board_capability_name(solar_os_board_capability_t capability)
{
    (void)capability;
    return "unknown";
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle; (void)key; (void)value;
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle; (void)key; (void)value;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    assert(!strcmp(key, "source"));
    custom_source[0] = 0;
    return ESP_OK;
}

esp_err_t solar_os_storage_replace_files(const solar_os_storage_replacement_t *items, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const solar_os_storage_replacement_t *item = &items[i];
        const esp_err_t error = solar_os_storage_replace_file(item->staged_path, item->active_path, item->backup_path);
        if (error) return error;
    }
    return ESP_OK;
}

size_t solar_os_storage_mount_count(void) { return 1; }

bool solar_os_storage_get_mount(size_t index, solar_os_storage_mount_info_t *mount)
{
    if (index) return false;
    *mount = (solar_os_storage_mount_info_t){.type = SOLAR_OS_STORAGE_MOUNT_SD};
    assert(strlcpy(mount->mount_point, mount_root, sizeof(mount->mount_point)) < sizeof(mount->mount_point));
    return true;
}

esp_err_t solar_os_http_request_cancel(solar_os_http_request_t *request)
{
    assert(request);
    return ESP_OK;
}

static void cancel_before_extract(const solar_os_playground_progress_t *progress, void *user)
{
    if (progress->stage != SOLAR_OS_PLAYGROUND_PROGRESS_INSTALL) return;
    const solar_os_playground_app_info_t *app = user;
    assert(solar_os_playground_install(app, SOLAR_OS_PLAYGROUND_TARGET_SD, NULL, NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(solar_os_playground_uninstall(app, NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(solar_os_playground_delete() == ESP_ERR_INVALID_STATE);
    assert(solar_os_playground_reload() == ESP_ERR_INVALID_STATE);
    solar_os_playground_cancel();
}

static void cancel_catalog(const solar_os_playground_progress_t *progress, void *user)
{
    const solar_os_native_playground_api_v1_t *api = user;
    if (progress->stage == SOLAR_OS_PLAYGROUND_PROGRESS_CATALOG && progress->bytes_read) api->cancel();
}

static void test_portable_catalog(void)
{
    const solar_os_native_playground_api_v1_t *api = solar_os_native_components_get_service(
        SOLAR_OS_NATIVE_PLAYGROUND_SERVICE, SOLAR_OS_NATIVE_PLAYGROUND_ABI, sizeof(*api));
    assert(api);
    char source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    assert(!api->set_source("https://fixture.test/catalog.json"));
    assert(api->source(source, 4) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !source[0]);
    assert(!api->source(source, sizeof(source)) && !strcmp(source, "https://fixture.test/catalog.json"));
    assert(!api->set_storage(SOLAR_OS_PLAYGROUND_TARGET_SD) && api->storage() == SOLAR_OS_PLAYGROUND_TARGET_SD);
    assert(api->install("tool", SOLAR_OS_PLAYGROUND_TARGET_AUTO, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->catalog(SOLAR_OS_PLAYGROUND_CATALOG_REFRESH, cancel_catalog, (void *)api) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!api->catalog_available());
    assert(!api->catalog(SOLAR_OS_PLAYGROUND_CATALOG_REFRESH, NULL, NULL));
    assert(api->catalog_available() && api->category_count() == 1 && api->app_count() == 2);
    assert(!api->catalog(SOLAR_OS_PLAYGROUND_CATALOG_RELOAD, NULL, NULL));
    solar_os_playground_category_t category;
    assert(api->category(0, &category) && !strcmp(category.id, "tools"));
    solar_os_playground_app_info_t app;
    size_t index;
    assert(api->find("lua-tool", &index, &app) && !app.compatible);
    assert(strstr(app.incompatibility, "pkg install lua") != NULL);
    assert(api->install("lua-tool", SOLAR_OS_PLAYGROUND_TARGET_AUTO, NULL, NULL) ==
           SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    lua_installed = true;
    assert(api->app(index, &app) && app.compatible && !app.incompatibility[0]);
    lua_installed = false;
    assert(api->find("lua-tool", NULL, &app) && !app.compatible);
    assert(api->find("tool", &index, &app) && app.compatible);
    assert(!api->install("tool", SOLAR_OS_PLAYGROUND_TARGET_AUTO, NULL, NULL));
    assert(api->find_installed("tool", &app) && !strcmp(app.entry, "main.py"));
    char id[SOLAR_OS_PLAYGROUND_ID_MAX];
    assert(api->installed_id(index, id, sizeof(id)) && !strcmp(id, "tool"));
    char version[SOLAR_OS_PLAYGROUND_VERSION_MAX];
    assert(api->installed("tool", version, sizeof(version)) && !strcmp(version, "1.0.0"));
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!api->entry_path("tool", path, sizeof(path)) && !access(path, F_OK));
    assert(!api->set_source(NULL) && !api->catalog_available());
    assert(!api->remove("tool", NULL, NULL) && access(path, F_OK) != 0);
    assert(api->entry_path("tool", path, sizeof(path)) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->catalog(SOLAR_OS_PLAYGROUND_DATA_DELETE, NULL, NULL));
}

static void visit_zip_entry(const solar_os_zip_event_info_t *entry, void *user)
{
    unsigned *count = user;
    assert(entry->event == SOLAR_OS_ZIP_EVENT_LIST && entry->method == 0);
    assert(strcmp(entry->archive_name, *count ? "empty" : "example") == 0);
    ++*count;
}

static void test_zip_reader(void)
{
    const solar_os_native_zip_api_v1_t *api = solar_os_native_zip_get_service(
        SOLAR_OS_NATIVE_ZIP_SERVICE, SOLAR_OS_NATIVE_ZIP_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    assert(!solar_os_native_zip_get_service(SOLAR_OS_NATIVE_ZIP_SERVICE, 2, sizeof(*api)));
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!solar_os_storage_join_path(server, "entries.zip", path, sizeof(path)));
    unsigned count = 0;
    assert(!api->list(path, visit_zip_entry, &count) && count == 2);

    const char expected[] = "stored ZIP entry";
    char data[sizeof(expected)];
    memset(data, 0x7f, sizeof(data));
    size_t length = 0;
    assert(api->read(path, "example", data, 1, &length) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(length == sizeof(expected) - 1 && data[0] == 0x7f);
    assert(!api->read(path, "example", data, sizeof(data), &length));
    assert(length == sizeof(expected) - 1 && !memcmp(data, expected, length) && data[length] == 0x7f);
    assert(!api->read(path, "empty", NULL, 0, &length) && !length);
    assert(api->read(path, "../example", data, sizeof(data), &length) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->read(path, "missing", data, sizeof(data), &length) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);

    uint8_t *owned = NULL;
    assert(!solar_os_zip_read_file(path, "example", sizeof(data), &owned, &length));
    assert(length == sizeof(expected) - 1 && !strcmp((const char *)owned, expected));
    solar_os_zip_free(owned);
    assert(!solar_os_storage_join_path(server, "bad-crc.zip", path, sizeof(path)));
    assert(api->read(path, "example", data, sizeof(data), &length) == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!length);
}

static void verify_no_zip_workspace(const char *directory)
{
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) assert(strncmp(entry->d_name, ".solarzip-", 10));
    closedir(dir);
}

static void count_zip_files(const solar_os_zip_event_info_t *entry, void *user)
{
    size_t *count = user;
    assert(entry->event == SOLAR_OS_ZIP_EVENT_LIST && !strstr(entry->archive_name, ".solarzip-"));
    assert(!strstr(entry->archive_name, "bundle.zip"));
    (*count)++;
}

static bool cancel_zip_before_second_chunk(void *user)
{
    unsigned *checks = user;
    return ++*checks >= 4;
}

static void test_zip_writer(void)
{
    const solar_os_native_zip_api_v1_t *api = solar_os_native_zip_get_service(
        SOLAR_OS_NATIVE_ZIP_SERVICE, SOLAR_OS_NATIVE_ZIP_ABI, sizeof(*api));
    assert(api);
    char root[SOLAR_OS_STORAGE_PATH_MAX], path[SOLAR_OS_STORAGE_PATH_MAX];
    char archive[SOLAR_OS_STORAGE_PATH_MAX], missing[SOLAR_OS_STORAGE_PATH_MAX];
    char target[SOLAR_OS_STORAGE_PATH_MAX], output[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!solar_os_storage_join_path(mount_root, "zip-source", root, sizeof(root)));
    assert(!solar_os_storage_makedirs(root, true));
    assert(!solar_os_storage_join_path(root, "payload", path, sizeof(path)));
    assert(!solar_os_storage_join_path(root, "bundle.zip", archive, sizeof(archive)));
    assert(!solar_os_storage_join_path(root, "missing", missing, sizeof(missing)));
    assert(!solar_os_storage_join_path(mount_root, "zip-result", target, sizeof(target)));
    assert(!solar_os_storage_join_path(target, "zip-source/payload", output, sizeof(output)));
    char data[6000];
    memset(data, 'x', sizeof(data));
    assert(!solar_os_storage_write_file(path, data, sizeof(data), false));

    const char *const source[] = {root};
    assert(!api->create(archive, source, 1, true, NULL, NULL));
    size_t entries = 0;
    assert(!api->list(archive, count_zip_files, &entries) && entries == 2);
    const char *const broken[] = {root, missing};
    assert(api->create(archive, broken, 2, true, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_FAILED);
    entries = 0;
    assert(!api->list(archive, count_zip_files, &entries) && entries == 2);

    assert(!solar_os_storage_write_file(path, "new", 3, false));
    mode = "zip-publish-failure";
    assert(api->create(archive, source, 1, true, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_FAILED);
    mode = "normal";
    size_t bytes = 0;
    assert(!api->read(archive, "zip-source/payload", data, sizeof(data), &bytes));
    assert(bytes == sizeof(data) && data[0] == 'x');
    verify_no_zip_workspace(root);

    char parent[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!solar_os_storage_join_path(target, "zip-source", parent, sizeof(parent)));
    assert(!solar_os_storage_makedirs(parent, true));
    assert(!solar_os_storage_write_file(output, "keep", 4, false));
    unsigned checks = 0;
    assert(api->extract(archive, target, NULL, cancel_zip_before_second_chunk, &checks) ==
           SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(checks == 4);
    FILE *file = fopen(output, "rb");
    assert(file && fread(data, 1, sizeof(data), file) == 4 && !memcmp(data, "keep", 4));
    fclose(file);
    verify_no_zip_workspace(parent);
    assert(!api->extract(archive, target, NULL, NULL, NULL));
    file = fopen(output, "rb");
    assert(file && fread(data, 1, sizeof(data), file) == sizeof(data) && data[0] == 'x');
    fclose(file);

    char corrupt[SOLAR_OS_STORAGE_PATH_MAX], old[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!solar_os_storage_join_path(server, "bad-crc.zip", corrupt, sizeof(corrupt)));
    assert(!solar_os_storage_join_path(target, "example", old, sizeof(old)));
    file = fopen(corrupt, "rb");
    assert(file);
    bytes = fread(data, 1, sizeof(data), file);
    fclose(file);
    assert(bytes > 0 && bytes < sizeof(data));
    assert(!solar_os_storage_join_path(root, "corrupt.zip", corrupt, sizeof(corrupt)));
    assert(!solar_os_storage_write_file(corrupt, data, bytes, false));
    assert(!solar_os_storage_write_file(old, "old", 3, false));
    assert(api->extract(corrupt, target, NULL, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_FAILED);
    file = fopen(old, "rb");
    assert(file && fread(data, 1, sizeof(data), file) == 3 && !memcmp(data, "old", 3));
    fclose(file);
    verify_no_zip_workspace(target);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    mount_root = argv[1];
    server = argv[2];
    mode = "normal";

    solar_os_playground_app_info_t app = {.id = "tool", .entry = "main.py", .version = "1.0.0",
        .runtime = SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON, .archive = "tool.sopkg", .compatible = true};
    assert(strlcpy(app.sha256, argv[3], sizeof(app.sha256)) == 64);
    app.size = strtoul(argv[4], NULL, 10);
    const solar_os_playground_target_t target = SOLAR_OS_PLAYGROUND_TARGET_SD;

    solar_os_playground_app_info_t invalid = app;
    strcpy(invalid.archive, "../tool.sopkg");
    assert(solar_os_playground_install(&invalid, target, NULL, NULL, NULL) == ESP_ERR_INVALID_ARG);
    assert(solar_os_playground_install(&app, target, NULL, NULL, NULL) == ESP_OK);
    char entry[SOLAR_OS_STORAGE_PATH_MAX];
    char old_entry[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!solar_os_playground_entry_path(&app, entry, sizeof(entry)));
    assert(!solar_os_storage_sibling_path(entry, ".saved", old_entry, sizeof(old_entry)));
    assert(!rename(entry, old_entry));
    assert(!solar_os_storage_write_file(entry, "existing", 8, false));

    assert(solar_os_playground_install(&app, target, NULL, cancel_before_extract, &app) == ESP_ERR_TIMEOUT);
    mode = "swap-failure";
    assert(solar_os_playground_install(&app, target, NULL, NULL, NULL) == ESP_FAIL);
    mode = "normal";
    assert(!access(old_entry, F_OK));
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!solar_os_storage_join_path(mount_root, "playground/python/.stage-tool", path, sizeof(path)));
    assert(access(path, F_OK) != 0);
    assert(!solar_os_playground_install(&app, target, NULL, NULL, NULL));
    assert(access(old_entry, F_OK) != 0);
    assert(!solar_os_playground_uninstall(&app, NULL, NULL));
    assert(access(entry, F_OK) != 0);
    test_portable_catalog();
    test_zip_reader();
    test_zip_writer();
    assert(!allocations);
    return 0;
}
