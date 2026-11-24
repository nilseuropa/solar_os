/* A plan retains one verified catalog and the operation lock across preview and
 * confirmation. Recheck local bytes and active users before the first download. */
typedef struct {
    bool installed;
    bool tracked;
    uint32_t size;
    char sha256[SOLAR_OS_MODULE_PACKAGE_SHA256_MAX];
    solar_os_module_receipt_t receipt;
} module_upgrade_local_t;

static esp_err_t module_local_fingerprint(solar_os_module_type_t type, const char *id,
    const solar_os_module_install_options_t *options, module_upgrade_local_t *local)
{
    memset(local, 0, sizeof(*local));
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_module_package_path(type, id, path, sizeof(path));
    solar_os_storage_metadata_t metadata;
    if (err == ESP_OK) err = solar_os_storage_stat(path, &metadata);
    if (err == ESP_ERR_NOT_FOUND && type == SOLAR_OS_MODULE_TYPE_APP) {
        err = module_legacy_app_path(id, path, sizeof(path));
        if (err == ESP_OK) err = solar_os_storage_stat(path, &metadata);
    }
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE || metadata.size_bytes > SOLAR_OS_NATIVE_ELF_MAX_BYTES)
        return ESP_ERR_INVALID_SIZE;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_FAIL;
    uint8_t *buffer = solar_os_memory_alloc(4096U, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.upgrade.hash");
    solar_os_crypto_sha256_t sha;
    solar_os_crypto_sha256_init(&sha);
    err = buffer ? solar_os_crypto_sha256_start(&sha) : ESP_ERR_NO_MEM;
    size_t total = 0;
    while (err == ESP_OK) {
        if (module_download_cancelled((void *)options)) { err = ESP_ERR_TIMEOUT; break; }
        size_t bytes = fread(buffer, 1, 4096U, file);
        if (!bytes) { if (ferror(file)) err = ESP_FAIL; break; }
        total += bytes;
        if (total > metadata.size_bytes) { err = ESP_ERR_INVALID_SIZE; break; }
        err = solar_os_crypto_sha256_update(&sha, buffer, bytes);
    }
    uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    if (err == ESP_OK && total != metadata.size_bytes) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = solar_os_crypto_sha256_finish(&sha, digest);
    solar_os_crypto_sha256_free(&sha);
    solar_os_memory_free(buffer);
    fclose(file);
    if (err != ESP_OK) return err;
    err = solar_os_crypto_bytes_to_hex(digest, sizeof(digest), local->sha256, sizeof(local->sha256));
    if (err != ESP_OK) return err;
    local->installed = true;
    local->size = total;
    err = solar_os_module_package_receipt(type, id, &local->receipt);
    if (err == ESP_ERR_NO_MEM) return err;
    local->tracked = err == ESP_OK && local->receipt.size == local->size &&
        !strcasecmp(local->receipt.sha256, local->sha256);
    return ESP_OK;
}

static bool module_local_matches(const module_upgrade_local_t *local,
                                 const solar_os_module_package_t *package)
{
    return local->installed && local->size == package->artifact_size &&
        !strcasecmp(local->sha256, package->artifact_sha256);
}

esp_err_t solar_os_module_package_inspect(const solar_os_module_catalog_t *catalog,
    solar_os_module_type_t type, const char *id,
    const solar_os_module_install_options_t *options, solar_os_module_installed_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    module_upgrade_local_t local;
    esp_err_t err = module_local_fingerprint(type, id, options, &local);
    if (err != ESP_OK) return err;
    info->installed = local.installed;
    if (local.tracked) strlcpy(info->version, local.receipt.version, sizeof(info->version));
    if (catalog && catalog->signature_verified) {
        int index = module_catalog_index(catalog, id);
        if (index >= 0 && catalog->packages[index].type == type && module_local_matches(&local, &catalog->packages[index])) {
            const solar_os_module_package_t *package = &catalog->packages[index];
            strlcpy(info->version, package->version, sizeof(info->version));
            info->compatibility = package->compatible ? SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED :
                                                      SOLAR_OS_MODULE_COMPATIBILITY_INCOMPATIBLE;
        }
    }
    return ESP_OK;
}

/* Numeric dotted versions are ordered without integer overflow. Unordered
 * labels are held rather than guessing that a different string is newer. */
static bool module_version_compare(const char *a, const char *b, int *comparison)
{
    *comparison = 0;
    if (!strcmp(a, b)) return true;
    if (!*a || !*b || strspn(a, "0123456789.") != strlen(a) ||
        strspn(b, "0123456789.") != strlen(b)) return false;
    int result = 0;
    while (*a || *b) {
        const char *ae = a, *be = b;
        while (isdigit((unsigned char)*ae)) ++ae;
        while (isdigit((unsigned char)*be)) ++be;
        if ((*a && ae == a) || (*b && be == b) ||
            (*ae == '.' && !ae[1]) || (*be == '.' && !be[1])) return false;
        while (a < ae && *a == '0') ++a;
        while (b < be && *b == '0') ++b;
        size_t alen = (size_t)(ae - a), blen = (size_t)(be - b);
        if (!result) {
            if (alen != blen) result = alen > blen ? 1 : -1;
            else if (alen) { int cmp = memcmp(a, b, alen); result = (cmp > 0) - (cmp < 0); }
        }
        a = *ae ? ae + 1 : ae;
        b = *be ? be + 1 : be;
    }
    *comparison = result;
    return true;
}

static bool module_upgrade_version_allowed(const module_upgrade_local_t *local,
                                           const solar_os_module_package_t *package)
{
    if (!local->tracked || module_local_matches(local, package)) return true;
    int comparison;
    return module_version_compare(package->version, local->receipt.version, &comparison) && comparison >= 0;
}

static int module_upgrade_entry_index(const solar_os_module_upgrade_plan_t *plan,
                                      solar_os_module_type_t type, const char *id)
{
    for (size_t i = 0; i < plan->count; ++i) {
        const solar_os_module_package_t *p = &plan->catalog->packages[plan->entries[i].package_index];
        if (p->type == type && !strcmp(p->id, id)) return (int)i;
    }
    return -1;
}

typedef struct {
    solar_os_module_upgrade_plan_t *plan;
    solar_os_module_type_t type;
} module_upgrade_count_t;

static bool module_upgrade_count_held(const char *id, void *user)
{
    module_upgrade_count_t *visit = user;
    int index = module_catalog_index(visit->plan->catalog, id);
    if (index < 0 || visit->plan->catalog->packages[index].type != visit->type) {
        ++visit->plan->held_count;
        ++visit->plan->unverified_count;
    }
    return true;
}

void solar_os_module_upgrade_plan_free(solar_os_module_upgrade_plan_t *plan)
{
    if (!plan) return;
    solar_os_module_catalog_free(plan->catalog);
    solar_os_memory_free(plan);
    atomic_store(&module_operation_running, false);
}

esp_err_t solar_os_module_upgrade_plan(const char *id,
    const solar_os_module_install_options_t *options,
    solar_os_module_upgrade_plan_t **out_plan, char *detail, size_t detail_len)
{
    if (!out_plan) return ESP_ERR_INVALID_ARG;
    *out_plan = NULL;
    module_set_detail(detail, detail_len, "");
    if (id && !module_name_valid(id)) return ESP_ERR_INVALID_ARG;
    if (atomic_exchange(&module_operation_running, true)) {
        module_set_detail(detail, detail_len, "another module operation is running");
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_module_upgrade_plan_t *plan = solar_os_memory_calloc(1, sizeof(*plan),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.upgrade.plan");
    if (!plan) { atomic_store(&module_operation_running, false); return ESP_ERR_NO_MEM; }
    module_upgrade_local_t *local = NULL;
    esp_err_t err = solar_os_module_catalog_load(&plan->catalog, detail, detail_len);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "no usable saved catalog; run pkg update first");
        goto done;
    }
    local = solar_os_memory_calloc(plan->catalog->count ? plan->catalog->count : 1U, sizeof(*local),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.upgrade.local");
    if (!local) { err = ESP_ERR_NO_MEM; goto done; }
    int root = id ? module_catalog_index(plan->catalog, id) : -1;
    if (id && root < 0) { err = ESP_ERR_NOT_FOUND; module_set_detail(detail, detail_len, "module is not in the saved catalog"); goto done; }
    uint8_t state[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX] = {0};
    uint8_t order[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
    size_t count = 0;
    for (size_t i = 0; i < plan->catalog->count; ++i) {
        const solar_os_module_package_t *package = &plan->catalog->packages[i];
        err = module_local_fingerprint(package->type, package->id, options, &local[i]);
        if (err != ESP_OK) goto done;
        plan->catalog->packages[i].installed = local[i].installed;
        if (local[i].installed) {
            if (!module_local_matches(&local[i], package)) ++plan->unverified_count;
            else if (package->compatible) ++plan->verified_count;
            else ++plan->incompatible_count;
        }
    }
    if (id && !local[root].installed) {
        err = ESP_ERR_NOT_FOUND; module_set_detail(detail, detail_len, "module is not installed; use pkg install"); goto done;
    }
    for (size_t i = 0; i < plan->catalog->count; ++i) {
        const solar_os_module_package_t *package = &plan->catalog->packages[i];
        if (!local[i].installed || (id && (int)i != root)) continue;
        if (!package->compatible || !module_upgrade_version_allowed(&local[i], package)) {
            if (id) {
                err = ESP_ERR_NOT_SUPPORTED;
                module_set_detail(detail, detail_len, !package->compatible ? package->incompatibility :
                    "upgrade would downgrade or cannot order versions; use pkg install to replace explicitly");
                goto done;
            }
            ++plan->held_count;
            continue;
        }
        err = module_plan_visit(plan->catalog, i, state, order, &count);
        if (err != ESP_OK) goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t index = order[i];
        const solar_os_module_package_t *package = &plan->catalog->packages[index];
        if (module_local_matches(&local[index], package)) continue;
        if (!module_upgrade_version_allowed(&local[index], package)) {
            if (detail && detail_len) snprintf(detail, detail_len, "dependency %s would downgrade or has unordered versions", package->id);
            err = ESP_ERR_NOT_SUPPORTED; goto done;
        }
        solar_os_module_upgrade_entry_t *entry = &plan->entries[plan->count++];
        entry->package_index = index;
        entry->installed = local[index].installed;
        entry->previous_size = local[index].size;
        strlcpy(entry->previous_sha256, local[index].sha256, sizeof(entry->previous_sha256));
        if (local[index].tracked)
            strlcpy(entry->previous_version, local[index].receipt.version, sizeof(entry->previous_version));
        plan->download_bytes += package->artifact_size;
    }
    if (!id) {
        module_upgrade_count_t visit = {.plan = plan};
        for (visit.type = SOLAR_OS_MODULE_TYPE_APP; visit.type <= SOLAR_OS_MODULE_TYPE_LIBRARY; ++visit.type) {
            err = solar_os_module_package_foreach_installed(visit.type, module_upgrade_count_held, &visit);
            if (err != ESP_OK) goto done;
        }
    }
    *out_plan = plan;
done:
    solar_os_memory_free(local);
    if (err != ESP_OK) solar_os_module_upgrade_plan_free(plan);
    return err;
}

typedef struct {
    const solar_os_module_upgrade_plan_t *plan;
    const solar_os_module_install_options_t *options;
    solar_os_module_type_t type;
    esp_err_t error;
    char *detail;
    size_t detail_len;
} module_upgrade_consumer_t;

static bool module_upgrade_check_consumer(const char *id, void *user)
{
    module_upgrade_consumer_t *visit = user;
    const solar_os_module_upgrade_plan_t *plan = visit->plan;
    if (module_upgrade_entry_index(plan, visit->type, id) >= 0) return true;
    module_upgrade_local_t local;
    visit->error = module_local_fingerprint(visit->type, id, visit->options, &local);
    if (visit->error != ESP_OK) return false;
    const solar_os_module_dependency_t *deps = local.receipt.dependencies;
    size_t count = local.receipt.dependency_count;
    if (!local.tracked) {
        int index = module_catalog_index(plan->catalog, id);
        if (index >= 0 && plan->catalog->packages[index].type == visit->type &&
            module_local_matches(&local, &plan->catalog->packages[index])) {
            deps = plan->catalog->packages[index].dependencies;
            count = plan->catalog->packages[index].dependency_count;
        } else {
            if (visit->detail && visit->detail_len) snprintf(visit->detail, visit->detail_len,
                "untracked module %s may use changed libraries; reinstall it with pkg install", id);
            visit->error = ESP_ERR_INVALID_STATE;
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        int entry = module_upgrade_entry_index(plan, SOLAR_OS_MODULE_TYPE_LIBRARY, deps[i].id);
        if (entry < 0) continue;
        const solar_os_module_package_t *provider = &plan->catalog->packages[plan->entries[entry].package_index];
        if (strcmp(deps[i].version, provider->version)) {
            if (visit->detail && visit->detail_len) snprintf(visit->detail, visit->detail_len,
                "%s needs %s %s; upgrade its consumer together", id, deps[i].id, deps[i].version);
            visit->error = ESP_ERR_INVALID_STATE;
            return false;
        }
    }
    return true;
}

esp_err_t solar_os_module_upgrade_check(const solar_os_module_upgrade_plan_t *plan,
    const solar_os_module_install_options_t *options, char *detail, size_t detail_len)
{
    if (!plan || plan->applied) return ESP_ERR_INVALID_ARG;
    module_set_detail(detail, detail_len, "");
    bool libraries = false;
    for (size_t i = 0; i < plan->count; ++i) {
        const solar_os_module_upgrade_entry_t *entry = &plan->entries[i];
        const solar_os_module_package_t *package = &plan->catalog->packages[entry->package_index];
        module_upgrade_local_t local;
        esp_err_t err = module_local_fingerprint(package->type, package->id, options, &local);
        if (err != ESP_OK) return err;
        if (local.installed != entry->installed || local.size != entry->previous_size ||
            strcmp(local.sha256, entry->previous_sha256)) {
            module_set_detail(detail, detail_len, "installed files changed; run pkg upgrade again");
            return ESP_ERR_INVALID_STATE;
        }
        bool active = package->type == SOLAR_OS_MODULE_TYPE_APP ?
            solar_os_native_foreground_active(package->id) : solar_os_native_module_active(package->type, package->id);
        if (active) {
            if (detail && detail_len) snprintf(detail, detail_len, "close, stop or unload active module %s before upgrading", package->id);
            return ESP_ERR_INVALID_STATE;
        }
        if (package->type == SOLAR_OS_MODULE_TYPE_LIBRARY) libraries = true;
    }
    if (libraries) {
        module_upgrade_consumer_t visit = {.plan = plan, .options = options, .detail = detail, .detail_len = detail_len};
        for (visit.type = SOLAR_OS_MODULE_TYPE_APP; visit.type <= SOLAR_OS_MODULE_TYPE_LIBRARY; ++visit.type) {
            esp_err_t err = solar_os_module_package_foreach_installed(visit.type, module_upgrade_check_consumer, &visit);
            if (err != ESP_OK) return err;
            if (visit.error != ESP_OK) return visit.error;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_module_upgrade_apply(solar_os_module_upgrade_plan_t *plan,
    const solar_os_module_install_options_t *options, size_t *completed,
    char *detail, size_t detail_len)
{
    if (completed) *completed = 0;
    esp_err_t err = solar_os_module_upgrade_check(plan, options, detail, detail_len);
    if (err != ESP_OK) return err;
    if (!plan->count) { plan->applied = true; return ESP_OK; }
    _Static_assert(SOLAR_OS_MODULE_PACKAGE_COUNT_MAX * 2U <= SOLAR_OS_STORAGE_REPLACE_MAX,
                   "upgrade transaction must fit all module and receipt pairs");
    typedef struct { char paths[6][SOLAR_OS_STORAGE_PATH_MAX]; } module_upgrade_paths_t;
    module_upgrade_paths_t *paths = solar_os_memory_calloc(plan->count, sizeof(*paths),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.upgrade.paths");
    solar_os_storage_replacement_t *items = solar_os_memory_calloc(plan->count * 2U, sizeof(*items),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.upgrade.files");
    if (!paths || !items) { err = ESP_ERR_NO_MEM; goto done; }
    for (size_t i = 0; i < plan->count; ++i) {
        const solar_os_module_package_t *package = &plan->catalog->packages[plan->entries[i].package_index];
        char (*p)[SOLAR_OS_STORAGE_PATH_MAX] = paths[i].paths;
        err = solar_os_module_package_path(package->type, package->id, p[0], sizeof(p[0]));
        if (err == ESP_OK) err = solar_os_storage_sibling_path(p[0], ".tmp", p[1], sizeof(p[1]));
        if (err == ESP_OK) err = solar_os_storage_sibling_path(p[0], ".bak", p[2], sizeof(p[2]));
        if (err == ESP_OK) err = module_receipt_path(package->type, package->id, p[3], sizeof(p[3]));
        if (err == ESP_OK) err = solar_os_storage_sibling_path(p[3], ".tmp", p[4], sizeof(p[4]));
        if (err == ESP_OK) err = solar_os_storage_sibling_path(p[3], ".bak", p[5], sizeof(p[5]));
        if (err != ESP_OK) goto done;
        items[2U * i] = (solar_os_storage_replacement_t){p[1], p[0], p[2]};
        items[2U * i + 1U] = (solar_os_storage_replacement_t){p[4], p[3], p[5]};
    }
    for (size_t i = 0; i < plan->count; ++i) {
        if (module_download_cancelled((void *)options)) {
            module_set_detail(detail, detail_len, "upgrade cancelled"); err = ESP_ERR_TIMEOUT; goto done;
        }
        const solar_os_module_package_t *package = &plan->catalog->packages[plan->entries[i].package_index];
        err = module_install_one_ex(package, options, NULL, detail, detail_len, true);
        if (err != ESP_OK) goto done;
    }
    err = solar_os_module_upgrade_check(plan, options, detail, detail_len);
    if (err != ESP_OK) goto done;
    err = solar_os_storage_replace_files(items, plan->count * 2U);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "could not publish upgrade; previous files restored where rollback succeeded");
        goto done;
    }
    if (completed) *completed = plan->count;
    for (size_t i = 0; i < plan->count; ++i) {
        const solar_os_module_package_t *package = &plan->catalog->packages[plan->entries[i].package_index];
        if (package->type == SOLAR_OS_MODULE_TYPE_APP) {
            char legacy[SOLAR_OS_STORAGE_PATH_MAX];
            if (module_legacy_app_path(package->id, legacy, sizeof(legacy)) == ESP_OK)
                (void)solar_os_storage_remove(legacy);
        } else if (package->type != SOLAR_OS_MODULE_TYPE_LIBRARY) {
            err = solar_os_native_module_activate(package->type, package->id, paths[i].paths[0], detail, detail_len);
            if (err != ESP_OK) break;
        }
    }
done:
    plan->applied = true;
    if (paths) for (size_t i = 0; i < plan->count; ++i) {
        if (paths[i].paths[1][0]) (void)solar_os_storage_remove(paths[i].paths[1]);
        if (paths[i].paths[4][0]) (void)solar_os_storage_remove(paths[i].paths[4]);
    }
    solar_os_memory_free(items);
    solar_os_memory_free(paths);
    return err;
}
