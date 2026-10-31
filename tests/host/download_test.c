#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "solar_os_download.h"
#include "solar_os_crypto.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_work.h"

static size_t payload_size, allocations, maximum_allocation, checkpoints, progress_last;
static bool fail_sync, corrupt_file, cancel_during;
static volatile bool cancelled;
struct solar_os_http_request { solar_os_http_request_options_t options; };

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)cls; (void)tag;
    ++allocations;
    if (size > maximum_allocation) maximum_allocation = size;
    return malloc(size);
}
void solar_os_memory_free(void *data) { if (data) { assert(allocations); --allocations; free(data); } }
void solar_os_work_begin(solar_os_work_budget_t *work) { memset(work, 0, sizeof(*work)); }
void solar_os_work_checkpoint(solar_os_work_budget_t *work) { (void)work; ++checkpoints; }
void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *ctx) { ctx->value = 0; }
void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *ctx) { ctx->value = 0; }
esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *ctx) { ctx->value = 2166136261U; return ESP_OK; }
esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *ctx, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) ctx->value = (ctx->value ^ bytes[i]) * 16777619U;
    return ESP_OK;
}
esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *ctx, uint8_t out[32])
{
    for (size_t i = 0; i < 32; ++i) out[i] = (uint8_t)(ctx->value >> ((i % 4) * 8));
    return ESP_OK;
}
bool solar_os_crypto_sha256_hex_is_valid(const char *hex)
{
    if (strlen(hex) != 64) return false;
    return strspn(hex, "0123456789abcdefABCDEF") == 64;
}
bool solar_os_crypto_sha256_matches_hex(const uint8_t digest[32], const char *hex)
{
    char actual[65];
    for (size_t i = 0; i < 32; ++i) snprintf(actual + i * 2, 3, "%02x", digest[i]);
    return !strcmp(actual, hex);
}
esp_err_t solar_os_storage_sync_file(FILE *file)
{
    if (fail_sync || fflush(file) || fsync(fileno(file))) return ESP_FAIL;
    if (corrupt_file) {
        assert(!fseek(file, 0, SEEK_SET));
        assert(fputc(255, file) != EOF && !fflush(file));
    }
    return ESP_OK;
}
esp_err_t solar_os_http_request_create(const solar_os_http_request_options_t *options,
    solar_os_http_request_t **out)
{
    *out = malloc(sizeof(**out)); assert(*out); (*out)->options = *options; return ESP_OK;
}
esp_err_t solar_os_http_request_destroy(solar_os_http_request_t *request) { free(request); return ESP_OK; }
esp_err_t solar_os_http_request_perform(solar_os_http_request_t *request, solar_os_http_response_t *response)
{
    *response = (solar_os_http_response_t){.status_code = 200, .content_length = -1};
    solar_os_http_event_t event = {.type = SOLAR_OS_HTTP_EVENT_RESPONSE, .status_code = 200, .content_length = -1};
    esp_err_t err = request->options.event_handler(&event, request->options.user_data);
    uint8_t bytes[5003];
    for (size_t offset = 0; err == ESP_OK && offset < payload_size;) {
        if (cancelled) return ESP_ERR_INVALID_STATE;
        size_t size = payload_size - offset;
        if (size > sizeof(bytes)) size = sizeof(bytes);
        for (size_t i = 0; i < size; ++i) bytes[i] = (uint8_t)((offset + i) % 251);
        event = (solar_os_http_event_t){.type = SOLAR_OS_HTTP_EVENT_DATA,
            .status_code = 200, .data = bytes, .data_len = size};
        err = request->options.event_handler(&event, request->options.user_data);
        offset += size;
    }
    return err;
}
static void progress(size_t bytes, size_t total, void *user)
{
    (void)total; (void)user;
    assert(bytes > progress_last);
    progress_last = bytes;
    if (cancel_during && bytes > 4096) cancelled = true;
}
int main(void)
{
    char directory[] = "/tmp/solaros-download-XXXXXX";
    assert(mkdtemp(directory));
    char path[160]; snprintf(path, sizeof(path), "%s/staged", directory);
    solar_os_http_request_options_t options = {.url = "http://fixture/archive", .cancel_flag = &cancelled};
    const size_t size = 2U * 1024U * 1024U;
    const char bad_hash[] = "0000000000000000000000000000000000000000000000000000000000000000";
    for (unsigned pass = 0; pass < 6; ++pass) {
        fail_sync = pass == 1; corrupt_file = pass == 2; cancel_during = pass == 3;
        cancelled = false; progress_last = 0;
        payload_size = pass == 4 ? size - 1 : pass == 5 ? size + 1 : size;
        solar_os_download_result_t result;
        esp_err_t err = solar_os_download_file(&options, path, size, size,
            pass == 0 ? NULL : bad_hash, progress, NULL, &result);
        if (pass == 0) {
            assert(err == ESP_OK && result.bytes == size && access(path, F_OK) == 0);
            assert(checkpoints > size / 4096);
            assert(!unlink(path));
        } else {
            assert(err != ESP_OK && access(path, F_OK) != 0);
        }
        assert(allocations == 0 && maximum_allocation == SOLAR_OS_DOWNLOAD_BUFFER_BYTES);
    }
    /* Read-back corruption and failed synchronization must fail independently
     * of the optional signed-catalog digest check. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        fail_sync = pass == 0; corrupt_file = pass != 0; cancel_during = cancelled = false;
        payload_size = size; progress_last = 0;
        solar_os_download_result_t result;
        assert(solar_os_download_file(&options, path, size, size, NULL,
            progress, NULL, &result) == (pass ? ESP_ERR_INVALID_CRC : ESP_FAIL));
        assert(access(path, F_OK) != 0 && allocations == 0);
    }
    assert(!rmdir(directory));
}
