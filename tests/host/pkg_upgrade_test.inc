#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>
#include "cJSON.h"
#include "solar_os_json.h"
#include "solar_os_module_packages.h"
#include "solar_os_native_components.h"
#include "solar_os_native_handles.h"

#define SOLAR_OS_NATIVE_ELF_MAX_BYTES (2U * 1024U * 1024U)
#define SOLAR_OS_CRYPTO_SHA256_LEN 32U
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 1
#define SOLAR_OS_VERSION "4.18.4"
#define MODULE_HTTP_TIMEOUT_MS 15000U
#define MODULE_HTTP_DEADLINE_MS 60000U
#define SOLAR_OS_HTTP_METHOD_GET 0
#define ESP_LOGW(...) ((void)0)
static atomic_bool module_operation_running;
static solar_os_module_package_t published_packages[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
static solar_os_module_catalog_t published;
static char payloads[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX][64];
static bool active[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
static unsigned allocations, downloads, fetches, rename_calls, validations;
static unsigned fail_download, fail_rename;
static const char *fail_allocation_tag;
static bool fail_receipt_write, fail_validation, fail_activation, fail_deactivation, cancel_requested;
static esp_err_t catalog_error;
static bool compiled_engine, compiled_client, compiled_bridge;
static const solar_os_component_dependency_t client_dependencies[] = {{"engine", "1.0.0"}};
static const solar_os_component_t firmware_components[] = {
    {SOLAR_OS_MODULE_TYPE_LIBRARY, "engine", "Engine", "1.0.0", 1, NULL, 0, NULL, NULL},
    {SOLAR_OS_MODULE_TYPE_APP, "builtin", "Compiled client", "1.0.0", 1, NULL, 1, client_dependencies, NULL},
    {SOLAR_OS_MODULE_TYPE_LIBRARY, "bridge", "Bridge", "1.0.0", 1, NULL, 1, client_dependencies, NULL},
};
size_t solar_os_component_count(void) { return (compiled_engine ? 1U : 0U) + (compiled_client ? 1U : 0U) + (compiled_bridge ? 1U : 0U); }
const solar_os_component_t *solar_os_component_get(size_t index)
{
    if (compiled_engine) { if (!index) return &firmware_components[0]; --index; }
    if (compiled_client) { if (!index) return &firmware_components[1]; --index; }
    return compiled_bridge && !index ? &firmware_components[2] : NULL;
}
const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type, const char *id)
{
    for (size_t i = 0; i < solar_os_component_count(); ++i) {
        const solar_os_component_t *component = solar_os_component_get(i);
        if ((!type || component->type == type) && !strcmp(id, component->id)) return component;
    }
    return NULL;
}

size_t strlcpy(char *d, const char *s, size_t n)
{ size_t len = strlen(s); if (n) snprintf(d, n, "%s", s); return len; }
static void *solar_os_memory_alloc(size_t n, int cls, const char *tag)
{ (void)cls; if (fail_allocation_tag && !strcmp(fail_allocation_tag, tag)) return NULL; void *p = malloc(n); if (p) ++allocations; return p; }
static void *solar_os_memory_calloc(size_t n, size_t size, int cls, const char *tag)
{ void *p = solar_os_memory_alloc(n * size, cls, tag); if (p) memset(p, 0, n * size); return p; }
static void solar_os_memory_free(void *p)
{ if (p) { assert(allocations); --allocations; free(p); } }

typedef struct { EVP_MD_CTX *ctx; } solar_os_crypto_sha256_t;
static void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *s) { s->ctx = NULL; }
static esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *s)
{ s->ctx = EVP_MD_CTX_new(); return s->ctx && EVP_DigestInit_ex(s->ctx, EVP_sha256(), NULL) == 1 ? ESP_OK : ESP_FAIL; }
static esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *s, const void *b, size_t n)
{ return EVP_DigestUpdate(s->ctx, b, n) == 1 ? ESP_OK : ESP_FAIL; }
static esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *s, uint8_t *digest)
{ unsigned n; return EVP_DigestFinal_ex(s->ctx, digest, &n) == 1 && n == 32 ? ESP_OK : ESP_FAIL; }
static void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *s) { EVP_MD_CTX_free(s->ctx); }
static void hash(const char *body, char *out)
{
    solar_os_crypto_sha256_t sha; uint8_t digest[32];
    solar_os_crypto_sha256_init(&sha); assert(solar_os_crypto_sha256_start(&sha) == ESP_OK);
    assert(solar_os_crypto_sha256_update(&sha, body, strlen(body)) == ESP_OK);
    assert(solar_os_crypto_sha256_finish(&sha, digest) == ESP_OK); solar_os_crypto_sha256_free(&sha);
    for (size_t i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", digest[i]);
}
static bool solar_os_crypto_sha256_matches_hex(const uint8_t *digest, const char *hex)
{ char text[65]; for (size_t i = 0; i < 32; ++i) snprintf(text + 2 * i, 3, "%02x", digest[i]); return !strcasecmp(text, hex); }
static bool solar_os_crypto_sha256_hex_is_valid(const char *text)
{ return strlen(text) == 64 && strspn(text, "0123456789abcdefABCDEF") == 64; }
static esp_err_t solar_os_crypto_bytes_to_hex(const uint8_t *bytes, size_t n, char *text, size_t capacity)
{ if (capacity < 2 * n + 1) return ESP_ERR_INVALID_SIZE; for (size_t i = 0; i < n; ++i) snprintf(text + 2 * i, 3, "%02x", bytes[i]); return ESP_OK; }

struct solar_os_json_doc { cJSON *root; };
esp_err_t solar_os_json_parse(const char *text, size_t n, solar_os_json_doc_t **doc)
{
    cJSON *root = cJSON_ParseWithLength(text, n);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    *doc = malloc(sizeof(**doc)); assert(*doc); (*doc)->root = root; return ESP_OK;
}
void solar_os_json_free(solar_os_json_doc_t *doc) { if (doc) { cJSON_Delete(doc->root); free(doc); } }
const solar_os_json_value_t *solar_os_json_root(const solar_os_json_doc_t *doc) { return doc->root; }
const solar_os_json_value_t *solar_os_json_path_get(const solar_os_json_value_t *v, const char *key)
{ return cJSON_GetObjectItemCaseSensitive(v, key); }
bool solar_os_json_is_array(const solar_os_json_value_t *v) { return cJSON_IsArray(v); }
size_t solar_os_json_array_size(const solar_os_json_value_t *v) { return cJSON_GetArraySize(v); }
const solar_os_json_value_t *solar_os_json_array_get(const solar_os_json_value_t *v, size_t i) { return cJSON_GetArrayItem(v, i); }
esp_err_t solar_os_json_get_path_string(const solar_os_json_value_t *v, const char *key, char *out, size_t n)
{ const cJSON *s = cJSON_GetObjectItemCaseSensitive(v, key); if (!cJSON_IsString(s) || strlen(s->valuestring) >= n) return ESP_FAIL; strcpy(out, s->valuestring); return ESP_OK; }
esp_err_t solar_os_json_get_path_uint32(const solar_os_json_value_t *v, const char *key, uint32_t *out)
{ const cJSON *s = cJSON_GetObjectItemCaseSensitive(v, key); if (!cJSON_IsNumber(s) || s->valuedouble < 0 || s->valuedouble > UINT32_MAX || s->valuedouble != (uint32_t)s->valuedouble) return ESP_FAIL; *out = s->valuedouble; return ESP_OK; }
static bool solar_os_native_service_available(const char *name, uint32_t abi, uint32_t size)
{ (void)name; (void)abi; (void)size; return true; }

esp_err_t solar_os_storage_default_path(const char *relative, char *out, size_t n)
{ return snprintf(out, n, "%s", relative) < (int)n ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_stat(const char *path, solar_os_storage_metadata_t *m)
{ struct stat s; if (stat(path, &s)) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL; m->size_bytes = s.st_size; m->type = S_ISREG(s.st_mode) ? SOLAR_OS_STORAGE_ENTRY_FILE : SOLAR_OS_STORAGE_ENTRY_DIRECTORY; return ESP_OK; }
esp_err_t solar_os_storage_exists(const char *path, bool *exists)
{ struct stat s; *exists = stat(path, &s) == 0; return *exists || errno == ENOENT ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_makedirs(const char *path, bool ok)
{ (void)ok; (void)mkdir("modules", 0700); return mkdir(path, 0700) == 0 || access(path, F_OK) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_remove(const char *path)
{ return remove(path) == 0 ? ESP_OK : errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL; }
esp_err_t solar_os_storage_write_file(const char *path, const void *body, size_t n, bool append)
{
    assert(!append);
    if (fail_receipt_write && strstr(path, ".json.tmp")) return ESP_FAIL;
    FILE *f = fopen(path, "wb"); if (!f) return ESP_FAIL;
    bool ok = fwrite(body, 1, n, f) == n; return fclose(f) == 0 && ok ? ESP_OK : ESP_FAIL;
}
esp_err_t solar_os_storage_read_file(const char *path, void *body, size_t n, size_t *read)
{ FILE *f = fopen(path, "rb"); if (!f) return ESP_ERR_NOT_FOUND; *read = fread(body, 1, n, f); bool ok = !ferror(f); return fclose(f) == 0 && ok ? ESP_OK : ESP_FAIL; }
static bool solar_os_disk_image_path_busy(const char *path) { (void)path; return false; }
void solar_os_storage_notify_changed(void) {}
static int injected_rename(const char *from, const char *to)
{ ++rename_calls; if (fail_rename && rename_calls == fail_rename) { errno = EIO; return -1; } return rename(from, to); }
#define rename injected_rename
/* STORAGE */
#undef rename

/* BASICS */
#include "solar_os_module_requirements.inc"
/* RECEIPTS */

esp_err_t solar_os_module_catalog_load(solar_os_module_catalog_t **out, char *detail, size_t n)
{
    *out = NULL; module_set_detail(detail, n, ""); if (catalog_error) return catalog_error;
    *out = solar_os_memory_alloc(sizeof(**out), 1, "catalog"); if (!*out) return ESP_ERR_NO_MEM;
    **out = published;
    (*out)->packages = NULL;
    if (published.count) {
        (*out)->packages = solar_os_memory_alloc(published.count * sizeof(*published.packages), 1, "catalog.entries");
        if (!(*out)->packages) { solar_os_memory_free(*out); *out = NULL; return ESP_ERR_NO_MEM; }
        memcpy((*out)->packages, published.packages, published.count * sizeof(*published.packages));
    }
    return ESP_OK;
}
void solar_os_module_catalog_free(solar_os_module_catalog_t *p) { if (p) solar_os_memory_free(p->packages); solar_os_memory_free(p); }
static esp_err_t module_catalog_fetch(solar_os_module_catalog_t **out,
    solar_os_module_package_cancel_fn cancel, void *user, char *detail, size_t n)
{ (void)cancel; (void)user; ++fetches; return solar_os_module_catalog_load(out, detail, n); }
esp_err_t solar_os_module_catalog_fetch_ex(solar_os_module_catalog_t **out,
    solar_os_module_package_cancel_fn cancel, void *user, char *detail, size_t capacity)
{ return module_catalog_fetch(out, cancel, user, detail, capacity); }
size_t solar_os_module_catalog_cached_count(void) { return published.count; }
bool solar_os_module_catalog_cached_get(size_t index, char *id, size_t capacity)
{ return index < published.count && strlcpy(id, published.packages[index].id, capacity) < capacity; }

static int slot(const char *id) { return module_catalog_index(&published, id); }
static bool solar_os_native_foreground_active(const char *id) { int i = slot(id); return i >= 0 && active[i]; }
static bool solar_os_native_module_active(solar_os_module_type_t type, const char *id)
{ (void)type; return solar_os_native_foreground_active(id); }
static esp_err_t solar_os_native_module_activate(solar_os_module_type_t type, const char *id,
    const char *path, char *detail, size_t n)
{ (void)type; (void)path; (void)detail; (void)n; if (fail_activation) return ESP_FAIL; int i = slot(id); assert(i >= 0 && !active[i]); active[i] = true; return ESP_OK; }
static esp_err_t solar_os_native_module_deactivate(solar_os_module_type_t type, const char *id, char *detail, size_t n)
{ (void)type; (void)detail; (void)n; int i = slot(id); assert(i >= 0 && active[i]); if (fail_deactivation) return ESP_ERR_INVALID_STATE; active[i] = false; return ESP_OK; }
static esp_err_t module_validate_download(const char *path, uint32_t size,
    const solar_os_module_package_t *p, char *detail, size_t n)
{ (void)path; (void)size; (void)p; (void)detail; (void)n; ++validations; return fail_validation ? ESP_ERR_INVALID_RESPONSE : ESP_OK; }
typedef struct {
    const char *url; int method; const char *user_agent;
    bool follow_redirects; uint32_t timeout_ms, read_poll_ms, deadline_ms;
    bool (*should_cancel)(void *); void *cancel_user_data;
    size_t receive_buffer_size, transmit_buffer_size;
} solar_os_http_request_options_t;
typedef struct { struct { int status_code; } response; } solar_os_download_result_t;
static esp_err_t solar_os_download_file(const solar_os_http_request_options_t *r,
    const char *path, size_t maximum, size_t size, const char *sha256,
    void (*progress)(size_t, size_t, void *), void *user, solar_os_download_result_t *out)
{
    ++downloads; out->response.status_code = 200;
    if (fail_download && downloads == fail_download) return ESP_FAIL;
    if (r->should_cancel && r->should_cancel(r->cancel_user_data)) return ESP_ERR_TIMEOUT;
    int i = atoi(r->url); assert(i >= 0 && i < (int)published.count);
    char actual[65]; hash(payloads[i], actual);
    assert(size == maximum && size == strlen(payloads[i]) && !strcmp(actual, sha256));
    esp_err_t err = solar_os_storage_write_file(path, payloads[i], size, false);
    if (progress) progress(size, size, user);
    return err;
}
/* INSTALL */
/* UPGRADE */

static char detail[256];
static void clear_directory(const char *path)
{
    DIR *dir = opendir(path); if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[512]; assert(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < (int)sizeof(child));
        struct stat s; assert(!stat(child, &s));
        if (S_ISDIR(s.st_mode)) clear_directory(child); else assert(!remove(child));
    }
    closedir(dir); assert(!rmdir(path));
}
static void reset(void)
{
    compiled_engine = compiled_client = compiled_bridge = false;
    assert(!allocations && !atomic_load(&module_operation_running));
    clear_directory("modules"); memset(&published, 0, sizeof(published));
    memset(published_packages, 0, sizeof(published_packages)); published.packages = published_packages;
    published.signature_verified = true; strcpy(published.host_version, SOLAR_OS_VERSION);
    memset(active, 0, sizeof(active)); downloads = fetches = rename_calls = validations = 0;
    fail_download = fail_rename = 0; fail_receipt_write = fail_validation = fail_activation = cancel_requested = false;
    fail_deactivation = false;
    catalog_error = ESP_OK;
    fail_allocation_tag = NULL;
}
static solar_os_module_package_t *package(size_t i, const char *id, solar_os_module_type_t type,
                                         const char *version, const char *body)
{
    assert(i < SOLAR_OS_MODULE_PACKAGE_COUNT_MAX); if (published.count <= i) published.count = i + 1;
    solar_os_module_package_t *p = &published.packages[i]; memset(p, 0, sizeof(*p));
    strlcpy(p->id, id, sizeof(p->id)); strlcpy(p->version, version, sizeof(p->version));
    p->type = type; p->compatible = true; p->artifact_size = strlen(body);
    hash(body, p->artifact_sha256); snprintf(p->artifact_url, sizeof(p->artifact_url), "%zu", i);
    strlcpy(payloads[i], body, sizeof(payloads[i])); return p;
}
static void dependency(solar_os_module_package_t *consumer, const char *id, const char *version)
{
    assert(consumer->dependency_count < SOLAR_OS_MODULE_DEPENDENCIES_MAX);
    solar_os_module_dependency_t *d = &consumer->dependencies[consumer->dependency_count++];
    strlcpy(d->id, id, sizeof(d->id)); strlcpy(d->version, version, sizeof(d->version));
}
static void install_old(const solar_os_module_package_t *current, const char *version, const char *body,
                        const char *dependency_version)
{
    solar_os_module_package_t old = *current; strlcpy(old.version, version, sizeof(old.version));
    old.artifact_size = strlen(body); hash(body, old.artifact_sha256);
    if (dependency_version && old.dependency_count) strcpy(old.dependencies[0].version, dependency_version);
    char active_path[256], staged[256], backup[256], dir[64];
    snprintf(dir, sizeof(dir), "modules/%s", module_type_directory(old.type)); assert(solar_os_storage_makedirs(dir, true) == ESP_OK);
    assert(solar_os_module_package_path(old.type, old.id, active_path, sizeof(active_path)) == ESP_OK);
    assert(solar_os_storage_sibling_path(active_path, ".tmp", staged, sizeof(staged)) == ESP_OK);
    assert(solar_os_storage_sibling_path(active_path, ".bak", backup, sizeof(backup)) == ESP_OK);
    assert(solar_os_storage_write_file(staged, body, strlen(body), false) == ESP_OK);
    assert(module_publish_install(&old, staged, active_path, backup, false) == ESP_OK);
}
static void expect_file(const char *path, const char *body)
{ char data[1024]; size_t n; assert(solar_os_storage_read_file(path, data, sizeof(data), &n) == ESP_OK); assert(n == strlen(body) && !memcmp(data, body, n)); }
static solar_os_module_upgrade_plan_t *plan_for(const char *id)
{ unsigned before = downloads; solar_os_module_upgrade_plan_t *plan = NULL; assert(solar_os_module_upgrade_plan(id, NULL, &plan, detail, sizeof(detail)) == ESP_OK); assert(plan && !fetches && downloads == before); return plan; }
static bool cancel(void *user) { (void)user; return cancel_requested; }
static void cancel_after_download(uint32_t bytes, uint32_t total, void *user)
{ (void)bytes; (void)total; (void)user; cancel_requested = true; }

static void test_versions_and_receipts(void)
{
    reset(); int cmp;
    assert(module_version_compare("1.12.0", "1.9.9", &cmp) && cmp > 0);
    assert(module_version_compare("1.0", "1.0.0", &cmp) && !cmp);
    assert(module_version_compare("99999999999999999999999.0", "9.0", &cmp) && cmp > 0);
    assert(!module_version_compare("1..0", "1.0", &cmp));
    assert(!module_version_compare("1.0.", "1.0", &cmp));
    assert(!module_version_compare("release-b", "release-a", &cmp));
    solar_os_module_package_t *p = package(0, "demo", SOLAR_OS_MODULE_TYPE_APP, "1.2.0", "new demo");
    install_old(p, "1.0.0", "old demo", NULL);
    solar_os_module_receipt_t receipt; assert(solar_os_module_package_receipt(p->type, p->id, &receipt) == ESP_OK);
    assert(!strcmp(receipt.version, "1.0.0") && receipt.size == 8 && !receipt.dependency_count);
    solar_os_module_upgrade_plan_t *plan = plan_for(NULL);
    assert(plan->count == 1 && !strcmp(plan->entries[0].previous_version, "1.0.0"));
    size_t done; assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_OK && done == 1);
    solar_os_module_upgrade_plan_free(plan); expect_file("modules/apps/demo.elf", "new demo");
    plan = plan_for(NULL); assert(!plan->count && plan->verified_count == 1); solar_os_module_upgrade_plan_free(plan);
    solar_os_module_installed_info_t info;
    assert(solar_os_module_package_inspect(&published, p->type, p->id, NULL, &info) == ESP_OK);
    assert(info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED && !strcmp(info.version, "1.2.0"));
    published.packages[0].compatible = false;
    assert(solar_os_module_package_inspect(&published, p->type, p->id, NULL, &info) == ESP_OK);
    assert(info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_INCOMPATIBLE);
    assert(solar_os_module_package_inspect(NULL, p->type, p->id, NULL, &info) == ESP_OK);
    assert(info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_UNKNOWN);
    published.packages[0].compatible = true;
    install_old(p, "2.0.0", "locally newer", NULL); plan = plan_for(NULL);
    assert(!plan->count && plan->held_count == 1); solar_os_module_upgrade_plan_free(plan);
    install_old(p, "1.2.0", "rebuilt old", NULL); plan = plan_for(NULL);
    assert(plan->count == 1); solar_os_module_upgrade_plan_free(plan);
    assert(solar_os_module_package_remove("demo", detail, sizeof(detail)) == ESP_OK);
    assert(access("modules/apps/demo.elf.json", F_OK) != 0);
    assert(!allocations);
}

static void setup_pair(void)
{
    reset(); solar_os_module_package_t *app = package(0, "demo", SOLAR_OS_MODULE_TYPE_APP, "2.0.0", "new demo");
    dependency(app, "engine", "2.0.0");
    solar_os_module_package_t *lib = package(1, "engine", SOLAR_OS_MODULE_TYPE_LIBRARY, "2.0.0", "new library");
    install_old(app, "1.0.0", "old demo", "1.0.0"); install_old(lib, "1.0.0", "old library", NULL);
    rename_calls = 0;
}
static void expect_old_pair(void)
{
    expect_file("modules/apps/demo.elf", "old demo"); expect_file("modules/libraries/engine.elf", "old library");
    solar_os_module_receipt_t receipt;
    assert(solar_os_module_package_receipt(SOLAR_OS_MODULE_TYPE_APP, "demo", &receipt) == ESP_OK && !strcmp(receipt.version, "1.0.0"));
    assert(solar_os_module_package_receipt(SOLAR_OS_MODULE_TYPE_LIBRARY, "engine", &receipt) == ESP_OK && !strcmp(receipt.version, "1.0.0"));
    assert(access("modules/apps/demo.elf.tmp", F_OK) && access("modules/libraries/engine.elf.json.tmp", F_OK));
}
static void test_dependency_transactions(void)
{
    setup_pair(); solar_os_module_upgrade_plan_t *plan = plan_for("demo");
    assert(plan->count == 2 && plan->entries[0].package_index == 1 && plan->entries[1].package_index == 0);
    solar_os_module_upgrade_plan_t *other = NULL;
    assert(solar_os_module_upgrade_plan(NULL, NULL, &other, detail, sizeof(detail)) == ESP_ERR_INVALID_STATE && !other);
    active[1] = true; size_t done;
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_ERR_INVALID_STATE && !downloads);
    active[1] = false; assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_OK && done == 2);
    assert(downloads == 2 && validations == 2); solar_os_module_upgrade_plan_free(plan);
    expect_file("modules/apps/demo.elf", "new demo"); expect_file("modules/libraries/engine.elf", "new library");
    for (unsigned failure = 1; failure <= 8; ++failure) {
        setup_pair(); plan = plan_for(NULL); fail_rename = failure;
        assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) != ESP_OK && !done);
        solar_os_module_upgrade_plan_free(plan); expect_old_pair();
    }
    for (unsigned failure = 1; failure <= 2; ++failure) {
        setup_pair(); plan = plan_for(NULL); fail_download = failure;
        assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) != ESP_OK && !done);
        solar_os_module_upgrade_plan_free(plan); expect_old_pair();
    }
    setup_pair(); plan = plan_for(NULL); fail_receipt_write = true;
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) != ESP_OK && !done);
    solar_os_module_upgrade_plan_free(plan); expect_old_pair();
    setup_pair(); plan = plan_for(NULL); fail_validation = true;
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) != ESP_OK && !done);
    solar_os_module_upgrade_plan_free(plan); expect_old_pair();
    setup_pair(); plan = plan_for(NULL); fail_activation = true;
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) != ESP_OK && !done);
    solar_os_module_upgrade_plan_free(plan); expect_old_pair();
    setup_pair(); plan = plan_for(NULL);
    solar_os_module_install_options_t options = {.should_cancel = cancel, .progress = cancel_after_download};
    assert(solar_os_module_upgrade_apply(plan, &options, &done, detail, sizeof(detail)) == ESP_ERR_TIMEOUT && !done);
    solar_os_module_upgrade_plan_free(plan); expect_old_pair();
    assert(!allocations);
}

static void test_consumers_and_legacy(void)
{
    setup_pair(); solar_os_module_upgrade_plan_t *plan = plan_for("engine"); size_t done;
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_ERR_INVALID_STATE && !downloads);
    assert(strstr(detail, "demo needs engine 1.0.0")); solar_os_module_upgrade_plan_free(plan);
    setup_pair(); published.count = 1; /* An unrelated custom consumer has unknown requirements. */
    published.packages[0] = published.packages[1]; strcpy(payloads[0], payloads[1]); strcpy(published.packages[0].artifact_url, "0");
    assert(!remove("modules/apps/demo.elf.json")); plan = plan_for(NULL);
    assert(solar_os_module_upgrade_check(plan, NULL, detail, sizeof(detail)) == ESP_ERR_INVALID_STATE && strstr(detail, "untracked module demo"));
    solar_os_module_upgrade_plan_free(plan);
    reset(); solar_os_module_package_t *p = package(0, "legacy", SOLAR_OS_MODULE_TYPE_APP, "1.0.0", "new legacy");
    assert(!mkdir("modules", 0700)); assert(solar_os_storage_write_file("modules/legacy.app.elf", "old legacy", 10, false) == ESP_OK);
    plan = plan_for(NULL); assert(plan->count == 1 && !plan->entries[0].previous_version[0]);
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_OK && done == 1);
    solar_os_module_upgrade_plan_free(plan); assert(access("modules/legacy.app.elf", F_OK));
    solar_os_module_installed_info_t info;
    assert(solar_os_module_package_inspect(&published, p->type, p->id, NULL, &info) == ESP_OK && info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED);
    assert(solar_os_storage_write_file("modules/apps/legacy.elf", "changed", 7, false) == ESP_OK);
    assert(solar_os_module_package_inspect(&published, p->type, p->id, NULL, &info) == ESP_OK && !info.version[0] && info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_UNKNOWN);
    plan = plan_for(NULL); assert(plan->count == 1 && !plan->entries[0].previous_version[0]);
    assert(solar_os_storage_write_file("modules/apps/legacy.elf", "raced", 5, false) == ESP_OK);
    assert(solar_os_module_upgrade_check(plan, NULL, detail, sizeof(detail)) == ESP_ERR_INVALID_STATE);
    solar_os_module_upgrade_plan_free(plan);
    reset(); catalog_error = ESP_ERR_NOT_SUPPORTED; plan = NULL;
    assert(solar_os_module_upgrade_plan(NULL, NULL, &plan, detail, sizeof(detail)) == ESP_ERR_NOT_SUPPORTED && !plan);
    assert(strstr(detail, "pkg update") && !atomic_load(&module_operation_running) && !allocations);
}

static void test_missing_dependencies_and_full_catalog(void)
{
    setup_pair(); assert(!remove("modules/libraries/engine.elf"));
    assert(!remove("modules/libraries/engine.elf.json"));
    solar_os_module_upgrade_plan_t *plan = plan_for("demo"); size_t done;
    assert(plan->count == 2 && !plan->entries[0].installed && plan->entries[1].installed);
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_OK && done == 2);
    solar_os_module_upgrade_plan_free(plan);
    reset(); package(0, "not-installed", SOLAR_OS_MODULE_TYPE_APP, "1.0.0", "available"); plan = plan_for(NULL);
    assert(!plan->count); solar_os_module_upgrade_plan_free(plan); plan = NULL;
    assert(solar_os_module_upgrade_plan("not-installed", NULL, &plan, detail, sizeof(detail)) == ESP_ERR_NOT_FOUND && !plan);
    assert(!allocations && !atomic_load(&module_operation_running));
    reset();
    for (size_t i = 0; i < SOLAR_OS_MODULE_PACKAGE_COUNT_MAX; ++i) {
        char id[32]; snprintf(id, sizeof(id), "app%zu", i);
        solar_os_module_package_t *p = package(i, id, SOLAR_OS_MODULE_TYPE_APP, "1.0.1", "new app");
        install_old(p, "1.0.0", "old app", NULL);
    }
    plan = plan_for(NULL); assert(plan->count == SOLAR_OS_MODULE_PACKAGE_COUNT_MAX);
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_OK && done == SOLAR_OS_MODULE_PACKAGE_COUNT_MAX);
    solar_os_module_upgrade_plan_free(plan);
    for (size_t i = 0; i < published.count; ++i) {
        solar_os_module_installed_info_t info;
        assert(solar_os_module_package_inspect(&published, published.packages[i].type, published.packages[i].id, NULL, &info) == ESP_OK);
        assert(info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED && !strcmp(info.version, "1.0.1"));
    }
    reset(); solar_os_module_package_t *p = package(0, "worker", SOLAR_OS_MODULE_TYPE_JOB, "2.0.0", "new job");
    install_old(p, "1.0.0", "old job", NULL); plan = plan_for(NULL);
    assert(solar_os_module_upgrade_apply(plan, NULL, &done, detail, sizeof(detail)) == ESP_OK && done == 1 && active[0]);
    solar_os_module_upgrade_plan_free(plan);
    reset(); p = package(0, "single", SOLAR_OS_MODULE_TYPE_APP, "2.0.0", "new single");
    install_old(p, "1.0.0", "old single", NULL); fail_receipt_write = true;
    assert(solar_os_module_package_install("single", NULL, NULL, detail, sizeof(detail)) != ESP_OK);
    expect_file("modules/apps/single.elf", "old single");
    fail_receipt_write = false;
    assert(solar_os_module_package_install("single", NULL, NULL, detail, sizeof(detail)) == ESP_OK);
    solar_os_module_receipt_t receipt;
    assert(solar_os_module_package_receipt(p->type, p->id, &receipt) == ESP_OK && !strcmp(receipt.version, "2.0.0"));
    assert(solar_os_storage_write_file("modules/apps/single.elf.json", "{}", 2, false) == ESP_OK);
    assert(solar_os_module_package_receipt(p->type, p->id, &receipt) == ESP_ERR_INVALID_RESPONSE && !receipt.version[0]);
    assert(!allocations);
}

static unsigned compiled_visits;
static bool visit_compiled(const char *id, void *user)
{ assert(user == &compiled_visits && !strcmp(id, "engine")); ++compiled_visits; return true; }
static void test_compiled_components(void)
{
    reset(); compiled_engine = true;
    solar_os_module_package_t *app = package(0, "demo", SOLAR_OS_MODULE_TYPE_APP, "1.0.0", "app");
    app->dependency_count = 1; strcpy(app->dependencies[0].id, "engine"); strcpy(app->dependencies[0].version, "1.0.0");
    assert(module_catalog_requirements(&published) == ESP_OK && app->compatible);
    assert(solar_os_module_package_install("demo", NULL, NULL, detail, sizeof(detail)) == ESP_OK && downloads == 1);
    assert(solar_os_storage_makedirs("modules/libraries", true) == ESP_OK);
    assert(solar_os_storage_write_file("modules/libraries/engine.elf", "shadow", 6, false) == ESP_OK);
    compiled_visits = 0;
    assert(solar_os_module_package_foreach_installed(SOLAR_OS_MODULE_TYPE_LIBRARY, visit_compiled, &compiled_visits) == ESP_OK && compiled_visits == 1);
    solar_os_module_installed_info_t info;
    assert(solar_os_module_package_inspect(&published, SOLAR_OS_MODULE_TYPE_LIBRARY, "engine", NULL, &info) == ESP_OK);
    assert(info.installed && info.compiled && info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED && !strcmp(info.version, "1.0.0"));
    assert(solar_os_module_package_remove("engine", detail, sizeof(detail)) == ESP_ERR_NOT_SUPPORTED);
    expect_file("modules/libraries/engine.elf", "shadow");

    reset(); compiled_engine = true;
    app = package(0, "demo", SOLAR_OS_MODULE_TYPE_APP, "1.0.0", "app");
    app->dependency_count = 1; strcpy(app->dependencies[0].id, "engine"); strcpy(app->dependencies[0].version, "1.0.0");
    solar_os_module_package_t *newer = package(1, "engine", SOLAR_OS_MODULE_TYPE_LIBRARY, "2.0.0", "newer engine");
    assert(module_catalog_requirements(&published) == ESP_OK && app->compatible && !newer->compatible);
    assert(solar_os_module_package_install("demo", NULL, NULL, detail, sizeof(detail)) == ESP_OK && downloads == 1);

    reset(); compiled_engine = true;
    app = package(0, "demo", SOLAR_OS_MODULE_TYPE_APP, "1.0.0", "app");
    app->dependency_count = 1; strcpy(app->dependencies[0].id, "engine"); strcpy(app->dependencies[0].version, "2.0.0");
    solar_os_module_package_t *lib = package(1, "engine", SOLAR_OS_MODULE_TYPE_LIBRARY, "2.0.0", "new engine");
    assert(module_catalog_requirements(&published) == ESP_OK && !app->compatible && !lib->compatible && lib->compiled);
    assert(solar_os_module_package_install("demo", NULL, NULL, detail, sizeof(detail)) == ESP_ERR_NOT_SUPPORTED && !downloads);
    solar_os_module_install_result_t result;
    unsigned previous_fetches = fetches;
    assert(solar_os_module_package_install("engine", NULL, &result, detail, sizeof(detail)) == ESP_OK);
    assert(result.compiled && !strcmp(result.version, "1.0.0") && fetches == previous_fetches && !downloads);
    solar_os_module_upgrade_plan_t *plan;
    assert(solar_os_module_upgrade_plan("engine", NULL, &plan, detail, sizeof(detail)) == ESP_ERR_NOT_SUPPORTED && !plan);
    fetches = 0; plan = plan_for(NULL); assert(!plan->count && plan->held_count == 1); solar_os_module_upgrade_plan_free(plan);

    reset(); compiled_engine = compiled_client = true;
    assert(solar_os_module_package_install("builtin", NULL, &result, detail, sizeof(detail)) == ESP_OK);
    assert(result.compiled && !fetches && !downloads && !strcmp(result.id, "builtin"));
    reset(); compiled_client = true;
    package(0, "engine", SOLAR_OS_MODULE_TYPE_LIBRARY, "1.0.0", "engine");
    assert(solar_os_module_package_install("builtin", NULL, &result, detail, sizeof(detail)) == ESP_OK && downloads == 1);
    assert(result.compiled && !strcmp(result.id, "builtin"));

    reset(); compiled_client = true;
    lib = package(0, "engine", SOLAR_OS_MODULE_TYPE_LIBRARY, "2.0.0", "new engine");
    install_old(lib, "1.0.0", "old engine", NULL);
    plan = plan_for("engine");
    assert(solar_os_module_upgrade_check(plan, NULL, detail, sizeof(detail)) == ESP_ERR_INVALID_STATE && strstr(detail,"builtin needs engine 1.0.0"));
    solar_os_module_upgrade_plan_free(plan); assert(!downloads);

    reset(); compiled_bridge = true;
    lib = package(0, "engine", SOLAR_OS_MODULE_TYPE_LIBRARY, "1.0.0", "engine");
    lib->dependency_count = 1; strcpy(lib->dependencies[0].id,"bridge"); strcpy(lib->dependencies[0].version,"1.0.0");
    assert(module_catalog_requirements(&published) == ESP_ERR_INVALID_RESPONSE);
    reset(); assert(!allocations);
}

static void test_plan_allocation_failures(void)
{
    setup_pair();
    const char *tags[] = {"modules.upgrade.plan", "catalog", "catalog.entries",
        "modules.upgrade.entries", "modules.upgrade.local"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
        fail_allocation_tag = tags[i];
        solar_os_module_upgrade_plan_t *plan = (void *)1;
        assert(solar_os_module_upgrade_plan(NULL, NULL, &plan, detail, sizeof(detail)) == ESP_ERR_NO_MEM);
        assert(!plan && !allocations && !atomic_load(&module_operation_running));
    }
    fail_allocation_tag = NULL;
    solar_os_module_upgrade_plan_t *plan = plan_for(NULL);
    assert(plan && plan->entries && plan->catalog->packages);
    solar_os_module_upgrade_plan_free(plan);
    reset();
}

static bool package_cancel(void *user)
{ assert(user == &cancel_requested); return cancel_requested; }

static void test_package_system_abi(void)
{
    reset();
    const solar_os_native_packages_api_v1_t *api = solar_os_native_components_get_service(
        SOLAR_OS_NATIVE_PACKAGES_SERVICE, SOLAR_OS_NATIVE_PACKAGES_ABI, sizeof(*api));
    assert(api && !solar_os_native_components_get_service(SOLAR_OS_NATIVE_PACKAGES_SERVICE, 2, 0));
    solar_os_module_package_t *p = package(0, "demo", SOLAR_OS_MODULE_TYPE_APP, "1.2.0", "new demo");
    strcpy(published.host_version, "4.18.5");
    strcpy(published.target, "esp32s3");
    strcpy(published.source_commit, "fixture");
    published.native_abi = 1;
    solar_os_native_handle_t snapshot;
    cancel_requested = true;
    assert(api->catalog_open(true, package_cancel, &cancel_requested, &snapshot, detail, sizeof(detail)) ==
        SOLAR_OS_NATIVE_ERROR_CANCELLED && !snapshot && !fetches);
    cancel_requested = false;
    assert(!api->catalog_open(false, NULL, NULL, &snapshot, detail, sizeof(detail)));
    assert(snapshot && !fetches);
    solar_os_native_package_catalog_info_v1_t meta;
    assert(!api->catalog_info(snapshot, &meta) && meta.count == 1 && meta.signature_verified);
    assert(!strcmp(meta.host_version, "4.18.5") && !strcmp(meta.target, "esp32s3") && meta.native_abi == 1);
    solar_os_native_package_entry_v1_t record;
    assert(!api->catalog_entry(snapshot, 0, &record) && !strcmp(record.id, "demo"));
    assert(api->catalog_entry(snapshot, 1, &record) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    char id[32];
    assert(api->cached_count() == 1 && api->cached_id(0, id, sizeof(id)) && !strcmp(id, "demo"));
    solar_os_native_package_install_result_v1_t installed;
    assert(!api->install(p->id, NULL, &installed, detail, sizeof(detail)));
    assert(installed.type == SOLAR_OS_NATIVE_COMPONENT_APP && !strcmp(installed.version, "1.2.0"));
    solar_os_native_package_receipt_v1_t receipt;
    assert(!api->receipt(installed.type, installed.id, &receipt) && receipt.size == 8);
    solar_os_native_package_installed_info_v1_t info;
    assert(!api->inspect(snapshot, installed.type, installed.id, NULL, &info));
    assert(info.installed && info.compatibility == SOLAR_OS_MODULE_COMPATIBILITY_VERIFIED);
    assert(!api->catalog_close(snapshot));
    assert(api->catalog_info(snapshot, &meta) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->remove(p->id, detail, sizeof(detail)));
    assert(!allocations && !solar_os_native_handle_count());
}

typedef struct { unsigned seen, confirmations; bool allow; } upgrade_review_t;
static bool review_upgrade_entry(const solar_os_native_package_upgrade_entry_v1_t *entry, void *user)
{
    upgrade_review_t *review = user;
    assert(atomic_load(&module_operation_running));
    assert(entry->type == SOLAR_OS_NATIVE_COMPONENT_APP && !strcmp(entry->id, "demo"));
    assert(entry->installed && !strcmp(entry->previous_version, "1.0.0") && !strcmp(entry->version, "1.2.0"));
    ++review->seen;
    return true;
}
static bool confirm_upgrade_plan(const solar_os_native_package_upgrade_result_v1_t *plan, void *user)
{
    upgrade_review_t *review = user;
    assert(atomic_load(&module_operation_running));
    assert(plan->count == 1 && !plan->completed && plan->download_bytes == 8);
    ++review->confirmations;
    return review->allow;
}
static void test_upgrade_system_abi(void)
{
    reset();
    const solar_os_native_packages_api_v1_t *api = solar_os_native_components_get_service(
        SOLAR_OS_NATIVE_PACKAGES_SERVICE, SOLAR_OS_NATIVE_PACKAGES_ABI, sizeof(*api));
    assert(api);
    solar_os_module_package_t *p = package(0, "demo", SOLAR_OS_MODULE_TYPE_APP, "1.2.0", "new demo");
    install_old(p, "1.0.0", "old demo", NULL);
    upgrade_review_t review = {0};
    const solar_os_native_package_upgrade_options_v1_t options = {
        .visit = review_upgrade_entry, .confirm = confirm_upgrade_plan, .user = &review,
    };
    solar_os_native_package_upgrade_result_v1_t result;
    assert(!api->upgrade("demo", SOLAR_OS_NATIVE_PACKAGE_UPGRADE_PREVIEW, &options, &result, detail, sizeof(detail)));
    assert(review.seen == 1 && !review.confirmations && !downloads && result.count == 1 && !result.completed);
    assert(!api->upgrade("demo", SOLAR_OS_NATIVE_PACKAGE_UPGRADE_CHECK, &options, &result, detail, sizeof(detail)));
    assert(review.seen == 2 && !review.confirmations && !downloads);
    assert(api->upgrade("demo", SOLAR_OS_NATIVE_PACKAGE_UPGRADE_APPLY, &options, &result, detail, sizeof(detail)) ==
        SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(review.seen == 3 && review.confirmations == 1 && !downloads && !atomic_load(&module_operation_running));
    expect_file("modules/apps/demo.elf", "old demo");
    review.allow = true;
    assert(!api->upgrade("demo", SOLAR_OS_NATIVE_PACKAGE_UPGRADE_APPLY, &options, &result, detail, sizeof(detail)));
    assert(review.seen == 4 && review.confirmations == 2 && downloads == 1 && result.completed == 1);
    assert(!allocations && !atomic_load(&module_operation_running));
    expect_file("modules/apps/demo.elf", "new demo");
}

static void test_driver_install_and_remove(void)
{
    reset();
    const char id[] = "sht40-driver";
    const char path[] = "modules/drivers/sht40-driver.elf";
    package(0, id, SOLAR_OS_MODULE_TYPE_DRIVER, "1.0.0", "verified driver");
    solar_os_module_install_result_t result;
    fail_activation = true;
    assert(solar_os_module_package_install(id, NULL, &result, detail, sizeof(detail)) == ESP_FAIL);
    assert(!active[0] && access(path, F_OK) != 0);
    fail_activation = false;
    assert(solar_os_module_package_install(id, NULL, &result, detail, sizeof(detail)) == ESP_OK);
    assert(active[0] && result.type == SOLAR_OS_MODULE_TYPE_DRIVER);
    expect_file(path, "verified driver");
    fail_deactivation = true;
    assert(solar_os_module_package_remove(id, detail, sizeof(detail)) == ESP_ERR_INVALID_STATE);
    assert(active[0]);
    expect_file(path, "verified driver");
    fail_deactivation = false;
    assert(solar_os_module_package_remove(id, detail, sizeof(detail)) == ESP_OK);
    assert(!active[0] && access(path, F_OK) != 0 && !allocations);
}

int main(void)
{
    test_versions_and_receipts(); test_dependency_transactions(); test_consumers_and_legacy();
    test_compiled_components();
    test_plan_allocation_failures();
    test_missing_dependencies_and_full_catalog(); test_package_system_abi(); test_upgrade_system_abi();
    test_driver_install_and_remove(); reset();
    puts("pkg upgrade: receipts, version ordering, compatibility, dependency planning, active guards, grouped rollback, cancellation and legacy migration passed");
}
