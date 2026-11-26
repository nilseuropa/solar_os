/* Exercise the real installer and ZIP reader against a local HTTP fixture. */
#define main zoo_fixture_main
#include "zoo_test.c"
#undef main
#include "solar_os_board_caps.h"
#include "solar_os_components.h"
#include "solar_os_native_components.h"
#include "solar_os_playground.h"

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
    assert(api->catalog_available() && api->category_count() == 1 && api->app_count() == 1);
    assert(!api->catalog(SOLAR_OS_PLAYGROUND_CATALOG_RELOAD, NULL, NULL));
    solar_os_playground_category_t category;
    assert(api->category(0, &category) && !strcmp(category.id, "tools"));
    solar_os_playground_app_info_t app;
    size_t index = 1;
    assert(api->find("tool", &index, &app) && !index && app.compatible);
    assert(!api->install("tool", SOLAR_OS_PLAYGROUND_TARGET_AUTO, NULL, NULL));
    assert(api->find_installed("tool", &app) && !strcmp(app.entry, "main.py"));
    char id[SOLAR_OS_PLAYGROUND_ID_MAX];
    assert(api->installed_id(0, id, sizeof(id)) && !strcmp(id, "tool"));
    char version[SOLAR_OS_PLAYGROUND_VERSION_MAX];
    assert(api->installed("tool", version, sizeof(version)) && !strcmp(version, "1.0.0"));
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(!api->entry_path("tool", path, sizeof(path)) && !access(path, F_OK));
    assert(!api->set_source(NULL) && !api->catalog_available());
    assert(!api->remove("tool", NULL, NULL) && access(path, F_OK) != 0);
    assert(api->entry_path("tool", path, sizeof(path)) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->catalog(SOLAR_OS_PLAYGROUND_DATA_DELETE, NULL, NULL));
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
    assert(!allocations);
    return 0;
}
