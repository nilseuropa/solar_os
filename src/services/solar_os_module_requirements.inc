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

#define MODULE_DEPENDENCY_DEPTH_MAX 32U

static bool module_component_matches(const solar_os_component_t *component,
                                      solar_os_module_type_t type, const char *version)
{
    return component && component->type == type && !strcmp(component->version, version);
}

static void module_component_result(const solar_os_component_t *component,
                                     solar_os_module_install_result_t *result)
{
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->type = component->type;
    strlcpy(result->id, component->id, sizeof(result->id));
    strlcpy(result->version, component->version, sizeof(result->version));
    result->compiled = true;
}

static bool module_component_self_contained(const solar_os_component_t *component, size_t depth)
{
    if (depth >= MODULE_DEPENDENCY_DEPTH_MAX) return false;
    for (size_t i = 0; i < component->dependency_count; ++i) {
        const solar_os_component_dependency_t *dep = &component->dependencies[i];
        const solar_os_component_t *provider = solar_os_component_find(SOLAR_OS_MODULE_TYPE_INVALID, dep->id);
        if (!module_component_matches(provider, SOLAR_OS_MODULE_TYPE_LIBRARY, dep->version) ||
            !module_component_self_contained(provider, depth + 1)) return false;
    }
    return true;
}

typedef struct {
    const solar_os_module_catalog_t *catalog;
    uint8_t *state, *order;
    size_t *count;
    const solar_os_component_t *compiled_path[MODULE_DEPENDENCY_DEPTH_MAX];
} module_dependency_plan_t;

static esp_err_t module_plan_visit_ex(module_dependency_plan_t *, size_t, size_t);
static esp_err_t module_plan_compiled(module_dependency_plan_t *, const solar_os_component_t *, size_t);

static esp_err_t module_plan_dependency(module_dependency_plan_t *plan,
                                       const char *id, const char *version, size_t depth)
{
    const solar_os_component_t *component = solar_os_component_find(SOLAR_OS_MODULE_TYPE_INVALID, id);
    if (component) {
        /* A mismatching compiled provider makes its consumer incompatible;
         * it must never be downloaded over or silently shadowed. */
        return module_component_matches(component, SOLAR_OS_MODULE_TYPE_LIBRARY, version) ?
            module_plan_compiled(plan, component, depth) : ESP_OK;
    }
    int child = module_catalog_index(plan->catalog, id);
    if (child < 0 || plan->catalog->packages[child].type != SOLAR_OS_MODULE_TYPE_LIBRARY ||
        strcmp(version, plan->catalog->packages[child].version)) return ESP_ERR_INVALID_RESPONSE;
    return module_plan_visit_ex(plan, child, depth);
}

static esp_err_t module_plan_compiled(module_dependency_plan_t *plan,
                                     const solar_os_component_t *component, size_t depth)
{
    if (depth >= MODULE_DEPENDENCY_DEPTH_MAX) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < depth; ++i)
        if (plan->compiled_path[i] == component) return ESP_ERR_INVALID_RESPONSE;
    plan->compiled_path[depth] = component;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < component->dependency_count && err == ESP_OK; ++i)
        err = module_plan_dependency(plan, component->dependencies[i].id,
            component->dependencies[i].version, depth + 1);
    plan->compiled_path[depth] = NULL;
    return err;
}

static esp_err_t module_plan_visit_ex(module_dependency_plan_t *plan, size_t index, size_t depth)
{
    if (depth >= MODULE_DEPENDENCY_DEPTH_MAX) return ESP_ERR_INVALID_SIZE;
    if (plan->state[index] == 1U) return ESP_ERR_INVALID_RESPONSE;
    if (plan->state[index] == 2U) return ESP_OK;
    plan->state[index] = 1U;
    const solar_os_module_package_t *package = &plan->catalog->packages[index];
    for (size_t i = 0; i < package->dependency_count; ++i) {
        const solar_os_module_dependency_t *dep = &package->dependencies[i];
        esp_err_t err = module_plan_dependency(plan, dep->id, dep->version, depth + 1);
        if (err != ESP_OK) return err;
    }
    plan->state[index] = 2U;
    plan->order[(*plan->count)++] = index;
    return ESP_OK;
}

/* Resolve compiled and installed library graphs before downloading any file. */
static esp_err_t module_plan_visit(const solar_os_module_catalog_t *catalog, size_t index,
                                   uint8_t *state, uint8_t *order, size_t *count)
{
    module_dependency_plan_t plan = {.catalog = catalog, .state = state, .order = order, .count = count};
    return module_plan_visit_ex(&plan, index, 0);
}

static bool module_dependency_compatible(const solar_os_module_catalog_t *catalog,
    const char *id, const char *version, size_t depth, char *reason, size_t reason_len)
{
    if (depth >= MODULE_DEPENDENCY_DEPTH_MAX) return false;
    const solar_os_component_t *component = solar_os_component_find(SOLAR_OS_MODULE_TYPE_INVALID, id);
    if (component) {
        if (!module_component_matches(component, SOLAR_OS_MODULE_TYPE_LIBRARY, version)) {
            snprintf(reason, reason_len, "needs %.24s %.20s; firmware has %.20s", id, version, component->version);
            return false;
        }
        for (size_t i = 0; i < component->dependency_count; ++i)
            if (!module_dependency_compatible(catalog, component->dependencies[i].id,
                    component->dependencies[i].version, depth + 1, reason, reason_len)) return false;
        return true;
    }
    int child = module_catalog_index(catalog, id);
    if (child < 0) return false;
    if (!catalog->packages[child].compatible) {
        snprintf(reason, reason_len, "dependency %s: %.40s", id, catalog->packages[child].incompatibility);
        return false;
    }
    return true;
}

static esp_err_t module_catalog_requirements(solar_os_module_catalog_t *catalog)
{
    uint8_t state[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX] = {0};
    uint8_t order[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
    size_t count = 0;
    for (size_t i = 0; i < catalog->count; ++i) {
        solar_os_module_package_t *package = &catalog->packages[i];
        const solar_os_component_t *component = solar_os_component_find(SOLAR_OS_MODULE_TYPE_INVALID, package->id);
        if (component) {
            package->compiled = true;
            package->installed = true;
            if (!module_component_matches(component, package->type, package->version)) {
                package->compatible = false;
                snprintf(package->incompatibility, sizeof(package->incompatibility),
                    "firmware provides %.24s %.24s", component->id, component->version);
            }
        }
        esp_err_t err = module_plan_visit(catalog, i, state, order, &count);
        if (err != ESP_OK) return err;
    }
    for (size_t i = 0; i < count; ++i) {
        solar_os_module_package_t *package = &catalog->packages[order[i]];
        for (size_t d = 0; d < package->dependency_count && package->compatible; ++d) {
            package->compatible = module_dependency_compatible(catalog, package->dependencies[d].id,
                package->dependencies[d].version, 0, package->incompatibility, sizeof(package->incompatibility));
        }
    }
    return ESP_OK;
}
