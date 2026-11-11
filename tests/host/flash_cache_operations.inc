#define FLASH_CATALOG_MAX 1024U
#define FLASH_SIGNATURE_MAX 256U
#define FLASH_CATALOG_FILE "catalog.json"
#define FLASH_SIGNATURE_FILE "catalog.sig"
static unsigned requests, activations;
static bool attach_sd, remove_media, fail_http, fail_signature;
static const char *runtime_sd;
static char activated_root[SOLAR_OS_STORAGE_PATH_MAX];
void solar_os_memory_free(void *p) { free(p); }
static void flash_report(solar_os_flash_progress_fn callback, void *user,
    solar_os_flash_progress_stage_t stage, uint32_t done, uint32_t total, bool known)
{ (void)callback; (void)user; (void)stage; (void)done; (void)total; (void)known; }
static esp_err_t flash_catalog_urls(char *catalog, size_t len, char *signature, size_t siglen)
{ strlcpy(catalog, "https://host/catalog.json", len); strlcpy(signature, "https://host/catalog.sig", siglen); return ESP_OK; }
static esp_err_t flash_download_memory(const char *url, size_t limit,
    solar_os_flash_progress_stage_t stage, char **body, size_t *length,
    solar_os_flash_progress_fn callback, void *user)
{
    (void)url; (void)limit; (void)stage; (void)callback; (void)user;
    assert(leases == 1); requests++;
    if (fail_http) return ESP_FAIL;
    *body = strdup("signed"); assert(*body); *length = 6;
    if (attach_sd) { mount_at(runtime_sd, SOLAR_OS_STORAGE_MOUNT_SD); attach_sd = false; }
    if (remove_media) { generation++; remove_media = false; }
    return ESP_OK;
}
static esp_err_t flash_verify_signature(const char *body, size_t length, const char *signature)
{ assert(body && length && signature); return fail_signature ? ESP_ERR_INVALID_CRC : ESP_OK; }
static esp_err_t flash_catalog_parse(const char *root, const char *body, size_t length,
    solar_os_flash_catalog_t **out)
{
    assert(body && length); *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->artifacts = calloc(1, sizeof(*(*out)->artifacts)); assert((*out)->artifacts);
    (*out)->count = 1;
    strlcpy((*out)->cache_root, root, sizeof((*out)->cache_root));
    strlcpy((*out)->artifacts[0].cache_root, root, sizeof((*out)->artifacts[0].cache_root));
    return ESP_OK;
}
static esp_err_t flash_cached_catalog_read(const solar_os_flash_cache_t *cache,
    char **body, size_t *length, char **signature, size_t *signature_length)
{
    assert(solar_os_flash_cache_check(cache) == ESP_OK);
    *body = strdup("signed"); *signature = strdup("signature");
    assert(*body && *signature); *length = 6; *signature_length = 9;
    return ESP_OK;
}
static esp_err_t flash_activate_catalog(const solar_os_flash_cache_t *cache,
    const char *body, size_t length, const char *signature, size_t signature_length)
{
    assert(body && length && signature && signature_length && leases == 1);
    activations++; strlcpy(activated_root, cache->root, sizeof(activated_root)); return ESP_OK;
}
/* Production service functions are inserted here by test_flash_storage.py. */
