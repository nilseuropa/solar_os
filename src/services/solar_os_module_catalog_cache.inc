/* The signed bytes are cached together with their signature. Readers and
 * publishers serialize access because grouped replacement has error rollback,
 * but does not provide atomic visibility to concurrent readers. */
static atomic_flag module_catalog_file_busy = ATOMIC_FLAG_INIT;

static esp_err_t module_catalog_paths(char paths[6][SOLAR_OS_STORAGE_PATH_MAX])
{
    const char *names[] = {"catalog.json", "catalog.sig", "catalog.json.new",
        "catalog.sig.new", "catalog.json.old", "catalog.sig.old"};
    for (unsigned i = 0; i < 6; ++i) {
        char relative[64];
        snprintf(relative, sizeof(relative), "modules/.catalog/%s", names[i]);
        esp_err_t err = solar_os_storage_default_path(relative, paths[i], sizeof(paths[i]));
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t module_catalog_save(const void *body, size_t body_len,
    const void *signature, size_t signature_len)
{
    if (atomic_flag_test_and_set(&module_catalog_file_busy)) return ESP_ERR_INVALID_STATE;
    char paths[6][SOLAR_OS_STORAGE_PATH_MAX], root[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_default_path("modules/.catalog", root, sizeof(root));
    if (err == ESP_OK) err = solar_os_storage_makedirs(root, true);
    if (err == ESP_OK) err = module_catalog_paths(paths);
    if (err == ESP_OK) err = solar_os_storage_write_file(paths[2], body, body_len, false);
    if (err == ESP_OK) err = solar_os_storage_write_file(paths[3], signature, signature_len, false);
    if (err == ESP_OK) {
        const solar_os_storage_replacement_t items[] = {
            {paths[2], paths[0], paths[4]}, {paths[3], paths[1], paths[5]},
        };
        err = solar_os_storage_replace_files(items, 2);
    }
    atomic_flag_clear(&module_catalog_file_busy);
    return err;
}

static esp_err_t module_catalog_read(const char *path, size_t maximum, char **body, size_t *length)
{
    solar_os_storage_metadata_t metadata;
    esp_err_t err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK) return err;
    if (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE || !metadata.size_bytes || metadata.size_bytes > maximum)
        return ESP_ERR_INVALID_SIZE;
    *body = solar_os_memory_alloc((size_t)metadata.size_bytes + 1,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "modules.catalog.file");
    if (!*body) return ESP_ERR_NO_MEM;
    err = solar_os_storage_read_file(path, *body, (size_t)metadata.size_bytes, length);
    if (err == ESP_OK && *length != metadata.size_bytes) err = ESP_ERR_INVALID_SIZE;
    (*body)[*length] = '\0';
    return err;
}

esp_err_t solar_os_module_catalog_load(solar_os_module_catalog_t **out_catalog,
    char *detail, size_t detail_len)
{
    if (!out_catalog) return ESP_ERR_INVALID_ARG;
    *out_catalog = NULL;
    if (detail && detail_len) *detail = '\0';
    if (atomic_flag_test_and_set(&module_catalog_file_busy)) return ESP_ERR_INVALID_STATE;
    char paths[6][SOLAR_OS_STORAGE_PATH_MAX], release_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    char *body = NULL, *signature = NULL;
    size_t body_len = 0, signature_len = 0;
    esp_err_t err = module_catalog_paths(paths);
    if (err == ESP_OK) err = module_release_url(release_url, sizeof(release_url));
    if (err == ESP_OK) err = module_catalog_read(paths[0], MODULE_CATALOG_MAX_BYTES, &body, &body_len);
    if (err == ESP_OK) err = module_catalog_read(paths[1], MODULE_SIGNATURE_MAX_BYTES, &signature, &signature_len);
    if (err == ESP_OK) err = module_catalog_decode(body, body_len, signature, release_url, out_catalog, detail, detail_len);
    solar_os_memory_free(signature);
    solar_os_memory_free(body);
    atomic_flag_clear(&module_catalog_file_busy);
    return err;
}
