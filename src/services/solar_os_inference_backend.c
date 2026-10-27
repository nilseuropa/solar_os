#include "solar_os_inference_backend.h"
#include "solar_os_inference_backend_abi.h"
#include "solar_os_config.h"
#include "solar_os_native.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

struct solar_os_inference_backend {
    const solar_os_inference_backend_api_v1_t *api;
    void *engine;
    uint32_t lease;
};
#if SOLAR_OS_PACKAGE_BACKEND_INFERENCE_ESPDL
extern const solar_os_inference_backend_api_v1_t *solar_os_inference_espdl_api(void);
#endif
static esp_err_t provider_error(int result)
{
    switch (result) {
    case SOLAR_OS_NATIVE_OK: return ESP_OK;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: return ESP_ERR_INVALID_ARG;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: return ESP_ERR_INVALID_STATE;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return ESP_ERR_NO_MEM;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: return ESP_ERR_TIMEOUT;
    case SOLAR_OS_NATIVE_ERROR_INVALID_SIZE: return ESP_ERR_INVALID_SIZE;
    default: return ESP_FAIL;
    }
}
static void release_provider(solar_os_inference_backend_t *b)
{
    if (b->lease) solar_os_native_library_release(b->lease);
    solar_os_memory_free(b);
}
esp_err_t solar_os_inference_backend_load(const char *path,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_backend_t **out, solar_os_inference_model_info_t *info)
{
    if (!out || !path || !info || !cancel) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (cancel(user)) return ESP_ERR_TIMEOUT;
    solar_os_storage_metadata_t metadata;
    esp_err_t error = solar_os_storage_stat(path, &metadata);
    if (error) return error;
    if (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE || metadata.size_bytes < 16 ||
        metadata.size_bytes > SOLAR_OS_INFERENCE_FILE_MAX) return ESP_ERR_INVALID_SIZE;
    solar_os_inference_backend_t *b = solar_os_memory_calloc(1, sizeof(*b),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.provider");
    if (!b) return ESP_ERR_NO_MEM;
#if SOLAR_OS_PACKAGE_BACKEND_INFERENCE_ESPDL
    b->api = solar_os_inference_espdl_api();
#else
    const void *api = NULL;
    error = provider_error(solar_os_native_library_acquire(SOLAR_OS_INFERENCE_BACKEND_LIBRARY,
        SOLAR_OS_INFERENCE_BACKEND_ABI, SOLAR_OS_INFERENCE_BACKEND_API_V1_SIZE, &b->lease, &api));
    if (error) { release_provider(b); return error; }
    b->api = api;
#endif
    if (!b->api || b->api->abi_version != SOLAR_OS_INFERENCE_BACKEND_ABI ||
        b->api->struct_size < SOLAR_OS_INFERENCE_BACKEND_API_V1_SIZE || !b->api->load_buffer || !b->api->run || !b->api->reset ||
        !b->api->set_mode || !b->api->close || !b->api->version ||
        strcmp(b->api->version, SOLAR_OS_INFERENCE_BACKEND_VERSION)) {
        release_provider(b); return ESP_ERR_NOT_SUPPORTED;
    }
    void *allocation = solar_os_memory_alloc(metadata.size_bytes + 31,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.source");
    if (!allocation) { release_provider(b); return ESP_ERR_NO_MEM; }
    uint8_t *data = (uint8_t *)(((uintptr_t)allocation + 15) & ~(uintptr_t)15);
    FILE *file = fopen(path, "rb");
    error = file ? ESP_OK : ESP_ERR_NOT_FOUND;
    for (size_t offset = 0; !error && offset < metadata.size_bytes;) {
        if (cancel(user)) { error = ESP_ERR_TIMEOUT; break; }
        size_t chunk = metadata.size_bytes - offset;
        if (chunk > 4096) chunk = 4096;
        if (fread(data + offset, 1, chunk, file) != chunk) { error = ESP_ERR_INVALID_SIZE; break; }
        offset += chunk; vTaskDelay(1);
    }
    if (!error && (fgetc(file) != EOF || ferror(file))) error = ESP_ERR_INVALID_SIZE;
    if (file && fclose(file) && !error) error = ESP_FAIL;
    if (!error && cancel(user)) error = ESP_ERR_TIMEOUT;
    if (!error) {
        if (b->api->struct_size >= sizeof(*b->api) && b->api->load_owned_buffer) {
            error = provider_error(b->api->load_owned_buffer(allocation, data, metadata.size_bytes,
                cancel, user, &b->engine, info));
            allocation = NULL;
        } else error = provider_error(b->api->load_buffer(data, metadata.size_bytes,
            cancel, user, &b->engine, info));
    }
    solar_os_memory_free(allocation);
    if (error) {
        if (b->engine) b->api->close(b->engine);
        release_provider(b); return error;
    }
    if (!b->engine) { release_provider(b); return ESP_FAIL; }
    *out = b; return ESP_OK;
}
esp_err_t solar_os_inference_backend_run(solar_os_inference_backend_t *b,
    const solar_os_inference_input_t *inputs, size_t count,
    solar_os_inference_cancel_fn cancel, void *user, solar_os_inference_result_t *result)
{
    return b ? provider_error(b->api->run(b->engine, inputs, count, cancel, user, result)) : ESP_ERR_INVALID_ARG;
}
esp_err_t solar_os_inference_backend_reset(solar_os_inference_backend_t *b)
{ return b ? provider_error(b->api->reset(b->engine)) : ESP_ERR_INVALID_ARG; }
esp_err_t solar_os_inference_backend_set_mode(solar_os_inference_backend_t *b, solar_os_inference_mode_t mode)
{ return b ? provider_error(b->api->set_mode(b->engine, mode)) : ESP_ERR_INVALID_ARG; }
void solar_os_inference_backend_close(solar_os_inference_backend_t *b)
{
    if (!b) return;
    b->api->close(b->engine); /* Joins all engine workers before releasing code. */
    release_provider(b);
}
