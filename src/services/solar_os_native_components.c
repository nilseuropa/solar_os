#include "solar_os_native_components.h"

#include <stdio.h>
#include <string.h>
#include "solar_os_components.h"
#include "solar_os_config.h"
#include "solar_os_native_components_abi.h"
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES || SOLAR_OS_PACKAGE_SERVICE_PLAYGROUND
#include "solar_os_native_support.h"
#endif

_Static_assert(SOLAR_OS_MODULE_TYPE_APP == SOLAR_OS_NATIVE_COMPONENT_APP, "app type");
_Static_assert(SOLAR_OS_MODULE_TYPE_JOB == SOLAR_OS_NATIVE_COMPONENT_JOB, "job type");
_Static_assert(SOLAR_OS_MODULE_TYPE_DRIVER == SOLAR_OS_NATIVE_COMPONENT_DRIVER, "driver type");
_Static_assert(SOLAR_OS_MODULE_TYPE_LIBRARY == SOLAR_OS_NATIVE_COMPONENT_LIBRARY, "library type");

static uint32_t component_count(void)
{
    return solar_os_component_count();
}

static int component_copy(const solar_os_component_t *component, solar_os_native_component_info_v1_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    if (!component) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    snprintf(info->id, sizeof(info->id), "%s", component->id);
    snprintf(info->name, sizeof(info->name), "%s", component->name);
    snprintf(info->version, sizeof(info->version), "%s", component->version);
    info->type = component->type;
    info->lifecycle_abi = component->lifecycle_abi;
    return SOLAR_OS_NATIVE_OK;
}

static int component_info(uint32_t index, solar_os_native_component_info_v1_t *info)
{
    return component_copy(solar_os_component_get(index), info);
}

static int component_find(uint32_t type, const char *id, solar_os_native_component_info_v1_t *info)
{
    if (info) memset(info, 0, sizeof(*info));
    if (!id || !*id || strnlen(id, 32) >= 32 || type > SOLAR_OS_NATIVE_COMPONENT_LIBRARY)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return component_copy(solar_os_component_find((solar_os_module_type_t)type, id), info);
}

static const solar_os_native_components_api_v1_t components_api = {
    .abi_version = SOLAR_OS_NATIVE_COMPONENTS_ABI, .struct_size = sizeof(components_api),
    .count = component_count, .info = component_info, .find = component_find,
};

#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
#include "solar_os_module_packages.h"
#include "solar_os_native_handles.h"

_Static_assert(SOLAR_OS_NATIVE_PACKAGE_PATH_MAX == SOLAR_OS_STORAGE_PATH_MAX, "package path capacity");

static int packages_catalog_destroy(void *state)
{
    solar_os_module_catalog_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int packages_catalog_open(bool refresh, solar_os_native_package_cancel_fn cancel, void *user,
    solar_os_native_handle_t *out, char *detail, size_t capacity)
{
    if (!out || (!detail && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (detail && capacity) detail[0] = 0;
    if (cancel && cancel(user)) return SOLAR_OS_NATIVE_ERROR_CANCELLED;
    solar_os_module_catalog_t *catalog = NULL;
    const esp_err_t error = refresh ? solar_os_module_catalog_fetch_ex(&catalog, cancel, user, detail, capacity) :
        solar_os_module_catalog_load(&catalog, detail, capacity);
    if (error) return solar_os_native_result(error);
    const int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_PACKAGE_CATALOG,
        catalog, packages_catalog_destroy, out);
    if (result) solar_os_module_catalog_free(catalog);
    return result;
}

static int packages_catalog_info(solar_os_native_handle_t id, solar_os_native_package_catalog_info_v1_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_module_catalog_t *catalog = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_PACKAGE_CATALOG);
    if (!catalog) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *info = (solar_os_native_package_catalog_info_v1_t){.native_abi = catalog->native_abi,
        .signature_verified = catalog->signature_verified, .count = catalog->count};
    memcpy(info->host_version, catalog->host_version, sizeof(info->host_version));
    memcpy(info->source_commit, catalog->source_commit, sizeof(info->source_commit));
    memcpy(info->target, catalog->target, sizeof(info->target));
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int packages_catalog_entry(solar_os_native_handle_t id, size_t index, solar_os_native_package_entry_v1_t *entry)
{
    if (!entry) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_module_catalog_t *catalog = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_PACKAGE_CATALOG);
    if (!catalog) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const bool found = index < catalog->count;
    if (found) *entry = catalog->packages[index];
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int packages_catalog_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_PACKAGE_CATALOG); }

static int packages_install(const char *id, const solar_os_native_package_install_options_v1_t *options,
    solar_os_native_package_install_result_v1_t *result, char *detail, size_t capacity)
{ return solar_os_native_result(solar_os_module_package_install(id, options, result, detail, capacity)); }

static int packages_remove(const char *id, char *detail, size_t capacity)
{ return solar_os_native_result(solar_os_module_package_remove(id, detail, capacity)); }

static const char *packages_type_name(uint32_t type)
{ return solar_os_module_type_name((solar_os_module_type_t)type); }

static int packages_path(uint32_t type, const char *id, char *path, size_t capacity)
{ return solar_os_native_result(solar_os_module_package_path((solar_os_module_type_t)type, id, path, capacity)); }

static int packages_visit(uint32_t type, solar_os_native_package_installed_visit_fn visit, void *user)
{ return solar_os_native_result(solar_os_module_package_foreach_installed((solar_os_module_type_t)type, visit, user)); }

static int packages_receipt(uint32_t type, const char *id, solar_os_native_package_receipt_v1_t *receipt)
{ return solar_os_native_result(solar_os_module_package_receipt((solar_os_module_type_t)type, id, receipt)); }

static int packages_inspect(solar_os_native_handle_t id, uint32_t type, const char *name,
    const solar_os_native_package_install_options_v1_t *options, solar_os_native_package_installed_info_v1_t *info)
{
    const solar_os_module_catalog_t *catalog = NULL;
    if (id) {
        catalog = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_PACKAGE_CATALOG);
        if (!catalog) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    }
    const esp_err_t error = solar_os_module_package_inspect(catalog, (solar_os_module_type_t)type, name, options, info);
    if (id) solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static bool packages_visit_upgrade(const solar_os_module_upgrade_plan_t *plan,
    const solar_os_native_package_upgrade_options_v1_t *options)
{
    if (!options || !options->visit) return true;
    for (size_t i = 0; i < plan->count; ++i) {
        const solar_os_module_upgrade_entry_t *change = &plan->entries[i];
        const solar_os_module_package_t *package = &plan->catalog->packages[change->package_index];
        const solar_os_native_package_upgrade_entry_v1_t entry = {
            .type = package->type, .id = package->id, .version = package->version,
            .previous_version = change->previous_version, .previous_sha256 = change->previous_sha256,
            .artifact_size = package->artifact_size, .previous_size = change->previous_size,
            .installed = change->installed,
        };
        if (!options->visit(&entry, options->user)) return false;
    }
    return true;
}

static int packages_upgrade(const char *id, uint32_t action, const solar_os_native_package_upgrade_options_v1_t *options,
    solar_os_native_package_upgrade_result_v1_t *out, char *detail, size_t capacity)
{
    if (!out || action > SOLAR_OS_NATIVE_PACKAGE_UPGRADE_APPLY || (!detail && capacity))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_package_upgrade_result_v1_t){0};
    const solar_os_module_install_options_t *transfer = options ? &options->transfer : NULL;
    solar_os_module_upgrade_plan_t *plan = NULL;
    const esp_err_t error = solar_os_module_upgrade_plan(id, transfer, &plan, detail, capacity);
    if (error) return solar_os_native_result(error);
    *out = (solar_os_native_package_upgrade_result_v1_t){.count = plan->count, .held = plan->held_count,
        .verified = plan->verified_count, .unverified = plan->unverified_count,
        .incompatible = plan->incompatible_count, .download_bytes = plan->download_bytes};

    int result = SOLAR_OS_NATIVE_OK;
    if (!packages_visit_upgrade(plan, options)) {
        result = SOLAR_OS_NATIVE_ERROR_CANCELLED;
    } else if (action == SOLAR_OS_NATIVE_PACKAGE_UPGRADE_CHECK) {
        result = solar_os_native_result(solar_os_module_upgrade_check(plan, transfer, detail, capacity));
    } else if (action == SOLAR_OS_NATIVE_PACKAGE_UPGRADE_APPLY) {
        if (options && options->confirm && !options->confirm(out, options->user))
            result = SOLAR_OS_NATIVE_ERROR_CANCELLED;
        else
            result = solar_os_native_result(solar_os_module_upgrade_apply(plan, transfer, &out->completed, detail, capacity));
    }
    solar_os_module_upgrade_plan_free(plan);
    return result;
}

static const solar_os_native_packages_api_v1_t packages_api = {
    .abi_version = SOLAR_OS_NATIVE_PACKAGES_ABI, .struct_size = sizeof(packages_api),
    .catalog_open = packages_catalog_open, .catalog_info = packages_catalog_info,
    .catalog_entry = packages_catalog_entry, .catalog_close = packages_catalog_close,
    .cached_count = solar_os_module_catalog_cached_count, .cached_id = solar_os_module_catalog_cached_get,
    .install = packages_install, .remove = packages_remove, .type_name = packages_type_name,
    .path = packages_path, .visit_installed = packages_visit, .receipt = packages_receipt, .inspect = packages_inspect,
    .upgrade = packages_upgrade,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PLAYGROUND
#include "solar_os_playground.h"

static int playground_source(char *out, size_t capacity)
{
    if (!out || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    char source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
    solar_os_playground_get_source(source, sizeof(source));
    const size_t size = strlen(source) + 1;
    if (size > capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    memcpy(out, source, size);
    return SOLAR_OS_NATIVE_OK;
}

static int playground_set_source(const char *url)
{
    return solar_os_native_result(url ? solar_os_playground_set_source(url) : solar_os_playground_reset_source());
}

static int playground_set_storage(solar_os_playground_target_t target)
{ return solar_os_native_result(solar_os_playground_set_storage(target)); }

static int playground_catalog(uint32_t action, solar_os_playground_progress_fn progress, void *user)
{
    switch (action) {
    case SOLAR_OS_PLAYGROUND_CATALOG_RELOAD: return solar_os_native_result(solar_os_playground_reload());
    case SOLAR_OS_PLAYGROUND_CATALOG_REFRESH: return solar_os_native_result(solar_os_playground_refresh(NULL, progress, user));
    case SOLAR_OS_PLAYGROUND_DATA_DELETE: return solar_os_native_result(solar_os_playground_delete());
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}

static bool playground_lookup_local(const char *id, solar_os_playground_app_info_t *app)
{
    return solar_os_playground_find_installed_app(id, app) || solar_os_playground_find_app(id, NULL, app);
}

static bool playground_installed(const char *id, char *version, size_t capacity)
{
    solar_os_playground_app_info_t app;
    if (version && capacity) *version = 0;
    return playground_lookup_local(id, &app) && solar_os_playground_is_installed(&app, version, capacity);
}

static int playground_entry_path(const char *id, char *path, size_t capacity)
{
    if (!id || !*id || !path || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *path = 0;
    solar_os_playground_app_info_t app;
    if (!playground_lookup_local(id, &app)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    return solar_os_native_result(solar_os_playground_entry_path(&app, path, capacity));
}

static int playground_install(const char *id, solar_os_playground_target_t target,
    solar_os_playground_progress_fn progress, void *user)
{
    if (!id || !*id) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_playground_app_info_t app;
    if (!solar_os_playground_find_app(id, NULL, &app)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    return solar_os_native_result(solar_os_playground_install(&app, target, NULL, progress, user));
}

static int playground_remove(const char *id, solar_os_playground_progress_fn progress, void *user)
{
    if (!id || !*id) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_playground_app_info_t app;
    if (!playground_lookup_local(id, &app)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    return solar_os_native_result(solar_os_playground_uninstall(&app, progress, user));
}

static const solar_os_native_playground_api_v1_t playground_api = {
    .abi_version = SOLAR_OS_NATIVE_PLAYGROUND_ABI, .struct_size = sizeof(playground_api),
    .source = playground_source, .set_source = playground_set_source,
    .storage = solar_os_playground_get_storage, .set_storage = playground_set_storage,
    .catalog_available = solar_os_playground_catalog_available, .catalog = playground_catalog,
    .cancel = solar_os_playground_cancel,
    .category_count = solar_os_playground_category_count, .category = solar_os_playground_get_category,
    .app_count = solar_os_playground_app_count, .app = solar_os_playground_get_app,
    .find = solar_os_playground_find_app, .find_installed = solar_os_playground_find_installed_app,
    .installed_id = solar_os_playground_get_installed_app_id, .installed = playground_installed,
    .entry_path = playground_entry_path, .install = playground_install, .remove = playground_remove,
    .runtime_name = solar_os_playground_runtime_name, .target_name = solar_os_playground_target_name,
};
#endif

const void *solar_os_native_components_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_components_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
