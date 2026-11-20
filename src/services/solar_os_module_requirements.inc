/* Bounded signed catalog requirements and dependency-first install ordering. */
static esp_err_t module_parse_requirements(const solar_os_json_value_t *value,
                                          uint32_t schema,
                                          solar_os_module_package_t *package)
{
    const solar_os_json_value_t *deps = solar_os_json_path_get(value, "dependencies");
    const solar_os_json_value_t *services = solar_os_json_path_get(value, "required_services");
    if ((deps || services) && schema < 3U) return ESP_ERR_INVALID_RESPONSE;
    if ((deps && !solar_os_json_is_array(deps)) ||
        (services && !solar_os_json_is_array(services))) return ESP_ERR_INVALID_RESPONSE;
    size_t count = deps ? solar_os_json_array_size(deps) : 0;
    if (count > SOLAR_OS_MODULE_DEPENDENCIES_MAX) return ESP_ERR_INVALID_SIZE;
    package->dependency_count = count;
    for (size_t i = 0; i < count; ++i) {
        solar_os_module_dependency_t *dep = &package->dependencies[i];
        const solar_os_json_value_t *entry = solar_os_json_array_get(deps, i);
        if (solar_os_json_get_path_string(entry, "id", dep->id, sizeof(dep->id)) != ESP_OK ||
            solar_os_json_get_path_string(entry, "version", dep->version, sizeof(dep->version)) != ESP_OK ||
            !module_name_valid(dep->id) || !module_name_valid(dep->version))
            return ESP_ERR_INVALID_RESPONSE;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(dep->id, package->dependencies[j].id)) return ESP_ERR_INVALID_RESPONSE;
    }
    count = services ? solar_os_json_array_size(services) : 0;
    if (count > SOLAR_OS_MODULE_SERVICES_MAX) return ESP_ERR_INVALID_SIZE;
    package->service_count = count;
    for (size_t i = 0; i < count; ++i) {
        solar_os_module_service_requirement_t *service = &package->services[i];
        const solar_os_json_value_t *entry = solar_os_json_array_get(services, i);
        if (solar_os_json_get_path_string(entry, "name", service->name, sizeof(service->name)) != ESP_OK ||
            solar_os_json_get_path_uint32(entry, "abi", &service->abi) != ESP_OK ||
            solar_os_json_get_path_uint32(entry, "minimum_struct_size", &service->minimum_struct_size) != ESP_OK ||
            !module_name_valid(service->name) || !service->abi || service->minimum_struct_size < 8U)
            return ESP_ERR_INVALID_RESPONSE;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(service->name, package->services[j].name)) return ESP_ERR_INVALID_RESPONSE;
        bool available = solar_os_native_service_available(service->name, service->abi,
                                                          service->minimum_struct_size);
        /* Storage is a scoped capability supplied only to synchronous apps. */
        if (!strcmp(service->name, "storage") &&
            (package->type != SOLAR_OS_MODULE_TYPE_APP || package->lifecycle_abi != 1U)) available = false;
        if (package->compatible && !available) {
            package->compatible = false;
            snprintf(package->incompatibility, sizeof(package->incompatibility),
                     "needs service %s ABI %u size %u", service->name,
                     (unsigned)service->abi, (unsigned)service->minimum_struct_size);
        }
    }
    return ESP_OK;
}

static int module_catalog_index(const solar_os_module_catalog_t *catalog, const char *id)
{
    for (size_t i = 0; i < catalog->count; ++i)
        if (!strcmp(catalog->packages[i].id, id)) return (int)i;
    return -1;
}

/* Each catalog pins one exact version and content hash for every provider.
 * No network access or writes occur until the entire graph passes this walk. */
static esp_err_t module_plan_visit(const solar_os_module_catalog_t *catalog, size_t index,
                                   uint8_t *state, uint8_t *order, size_t *count)
{
    if (state[index] == 1U) return ESP_ERR_INVALID_RESPONSE;
    if (state[index] == 2U) return ESP_OK;
    state[index] = 1U;
    const solar_os_module_package_t *package = &catalog->packages[index];
    for (size_t i = 0; i < package->dependency_count; ++i) {
        const solar_os_module_dependency_t *dep = &package->dependencies[i];
        int child = module_catalog_index(catalog, dep->id);
        if (child < 0 || catalog->packages[child].type != SOLAR_OS_MODULE_TYPE_LIBRARY ||
            strcmp(dep->version, catalog->packages[child].version)) return ESP_ERR_INVALID_RESPONSE;
        esp_err_t err = module_plan_visit(catalog, child, state, order, count);
        if (err != ESP_OK) return err;
    }
    state[index] = 2U;
    order[(*count)++] = index;
    return ESP_OK;
}

static esp_err_t module_catalog_requirements(solar_os_module_catalog_t *catalog)
{
    uint8_t state[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX] = {0};
    uint8_t order[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
    size_t count = 0;
    for (size_t i = 0; i < catalog->count; ++i) {
        esp_err_t err = module_plan_visit(catalog, i, state, order, &count);
        if (err != ESP_OK) return err;
    }
    for (size_t i = 0; i < count; ++i) {
        solar_os_module_package_t *package = &catalog->packages[order[i]];
        for (size_t d = 0; d < package->dependency_count && package->compatible; ++d) {
            int child = module_catalog_index(catalog, package->dependencies[d].id);
            if (!catalog->packages[child].compatible) {
                package->compatible = false;
                char reason[sizeof(package->incompatibility)];
                snprintf(reason, sizeof(reason),
                         "dependency %s: %.40s", package->dependencies[d].id,
                         catalog->packages[child].incompatibility);
                memcpy(package->incompatibility, reason, sizeof(reason));
            }
        }
    }
    return ESP_OK;
}
