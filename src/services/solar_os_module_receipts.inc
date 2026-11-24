/* Versioned sidecars are published with their ELF using grouped error rollback.
 * They describe installed requirements, rather than the current online catalog. */
#define MODULE_RECEIPT_MAX_BYTES 1024U

static esp_err_t module_receipt_path(solar_os_module_type_t type, const char *id,
                                     char *path, size_t length)
{
    char elf[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_module_package_path(type, id, elf, sizeof(elf));
    return err == ESP_OK ? solar_os_storage_sibling_path(elf, ".json", path, length) : err;
}

esp_err_t solar_os_module_package_receipt(solar_os_module_type_t type,
                                         const char *id,
                                         solar_os_module_receipt_t *receipt)
{
    if (!receipt) return ESP_ERR_INVALID_ARG;
    memset(receipt, 0, sizeof(*receipt));
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_storage_metadata_t metadata;
    esp_err_t err = module_receipt_path(type, id, path, sizeof(path));
    if (err == ESP_OK) err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK) return err;
    if (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE || !metadata.size_bytes ||
        metadata.size_bytes > MODULE_RECEIPT_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    char *body = solar_os_memory_alloc((size_t)metadata.size_bytes + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.receipt");
    if (!body) return ESP_ERR_NO_MEM;
    size_t length = 0;
    solar_os_json_doc_t *doc = NULL;
    err = solar_os_storage_read_file(path, body, (size_t)metadata.size_bytes, &length);
    if (err == ESP_OK && length != metadata.size_bytes) err = ESP_ERR_INVALID_SIZE;
    body[length] = '\0';
    if (err == ESP_OK) err = solar_os_json_parse(body, length, &doc);
    if (err == ESP_OK) {
        const solar_os_json_value_t *root = solar_os_json_root(doc);
        uint32_t schema = 0, stored_type = 0;
        char stored_id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
        if (solar_os_json_get_path_uint32(root, "schema", &schema) != ESP_OK || schema != 1U ||
            solar_os_json_get_path_uint32(root, "type", &stored_type) != ESP_OK || stored_type != (uint32_t)type ||
            solar_os_json_get_path_string(root, "id", stored_id, sizeof(stored_id)) != ESP_OK || strcmp(id, stored_id) ||
            solar_os_json_get_path_string(root, "version", receipt->version, sizeof(receipt->version)) != ESP_OK ||
            !module_name_valid(receipt->version) ||
            solar_os_json_get_path_string(root, "sha256", receipt->sha256, sizeof(receipt->sha256)) != ESP_OK ||
            !solar_os_crypto_sha256_hex_is_valid(receipt->sha256) ||
            solar_os_json_get_path_uint32(root, "size", &receipt->size) != ESP_OK || !receipt->size) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        const solar_os_json_value_t *deps = solar_os_json_path_get(root, "dependencies");
        size_t count = solar_os_json_array_size(deps);
        if (!solar_os_json_is_array(deps) || count > SOLAR_OS_MODULE_DEPENDENCIES_MAX)
            err = ESP_ERR_INVALID_RESPONSE;
        for (size_t i = 0; err == ESP_OK && i < count; ++i) {
            solar_os_module_dependency_t *dep = &receipt->dependencies[i];
            const solar_os_json_value_t *item = solar_os_json_array_get(deps, i);
            if (solar_os_json_get_path_string(item, "id", dep->id, sizeof(dep->id)) != ESP_OK ||
                solar_os_json_get_path_string(item, "version", dep->version, sizeof(dep->version)) != ESP_OK ||
                !module_name_valid(dep->id) || !module_name_valid(dep->version)) err = ESP_ERR_INVALID_RESPONSE;
            for (size_t j = 0; j < i; ++j)
                if (!strcmp(dep->id, receipt->dependencies[j].id)) err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK) receipt->dependency_count = count;
    }
    solar_os_json_free(doc);
    solar_os_memory_free(body);
    if (err != ESP_OK) memset(receipt, 0, sizeof(*receipt));
    return err;
}

static esp_err_t module_publish_install(const solar_os_module_package_t *package,
    const char *staged, const char *active, const char *backup, bool stage_only)
{
    char paths[3][SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = module_receipt_path(package->type, package->id, paths[0], sizeof(paths[0]));
    if (err == ESP_OK) err = solar_os_storage_sibling_path(paths[0], ".tmp", paths[1], sizeof(paths[1]));
    if (err == ESP_OK) err = solar_os_storage_sibling_path(paths[0], ".bak", paths[2], sizeof(paths[2]));
    if (err != ESP_OK) return err;
    char *body = solar_os_memory_alloc(MODULE_RECEIPT_MAX_BYTES,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.receipt.write");
    if (!body) return ESP_ERR_NO_MEM;
    /* IDs, versions and hashes have already passed the signed catalog parser;
     * their alphabet contains no JSON quoting or control characters. */
    int written = snprintf(body, MODULE_RECEIPT_MAX_BYTES,
        "{\"schema\":1,\"type\":%u,\"id\":\"%s\",\"version\":\"%s\","
        "\"sha256\":\"%s\",\"size\":%u,\"dependencies\":[",
        (unsigned)package->type, package->id, package->version,
        package->artifact_sha256, (unsigned)package->artifact_size);
    size_t length = written > 0 ? (size_t)written : MODULE_RECEIPT_MAX_BYTES;
    for (size_t i = 0; i < package->dependency_count && length < MODULE_RECEIPT_MAX_BYTES; ++i) {
        written = snprintf(body + length, MODULE_RECEIPT_MAX_BYTES - length,
            "%s{\"id\":\"%s\",\"version\":\"%s\"}", i ? "," : "",
            package->dependencies[i].id, package->dependencies[i].version);
        length += written > 0 ? (size_t)written : MODULE_RECEIPT_MAX_BYTES;
    }
    if (length + 2U >= MODULE_RECEIPT_MAX_BYTES) err = ESP_ERR_INVALID_SIZE;
    else {
        memcpy(body + length, "]}", 2U);
        err = solar_os_storage_write_file(paths[1], body, length + 2U, false);
        if (err == ESP_OK && !stage_only) {
            const solar_os_storage_replacement_t items[] = {
                {staged, active, backup}, {paths[1], paths[0], paths[2]},
            };
            err = solar_os_storage_replace_files(items, 2U);
        }
    }
    solar_os_memory_free(body);
    if (err != ESP_OK) (void)solar_os_storage_remove(paths[1]);
    return err;
}
