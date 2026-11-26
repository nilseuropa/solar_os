#include "solar_os_processor.h"
#include "solar_os_config.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "solar_os_processor_provider_abi.h"
#include <stdio.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
#include "solar_os_native.h"
#include "solar_os_native_library_abi.h"
#endif
#if SOLAR_OS_PACKAGE_BACKEND_PROCESSOR_HIMAX
extern const solar_os_processor_provider_api_v1_t *solar_os_himax_processor_api(void);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_VISION
#include "solar_os_vision.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
#include "solar_os_model_bundle.h"
#endif

struct solar_os_processor {
    esp_err_t (*run)(solar_os_processor_t *, solar_os_raster_image_t *, cJSON **);
    solar_os_processor_cancel_fn cancel;
    void *user;
    uint32_t model, timeout_ms;
    uint32_t provider_lease, provider_model;
    const solar_os_processor_provider_api_v1_t *provider;
    void *provider_client;
    bool source_bound;
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
    solar_os_inference_t *client;
    char input[SOLAR_OS_INFERENCE_NAME_MAX];
#endif
};

bool solar_os_processor_kind_valid(const char *kind)
{
    if (!kind || !*kind || strlen(kind) >= 12) return false;
    for (const char *p = kind; *p; ++p)
        if ((*p < 'a' || *p > 'z') && (*p < '0' || *p > '9') && *p != '-') return false;
    return kind[0] >= 'a' && kind[0] <= 'z';
}
static esp_err_t provider_error(int error)
{
    switch (error) {
    case SOLAR_OS_NATIVE_OK: return ESP_OK;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: return ESP_ERR_INVALID_ARG;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE:
    case SOLAR_OS_NATIVE_ERROR_BUSY: return ESP_ERR_INVALID_STATE;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return ESP_ERR_NO_MEM;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT:
    case SOLAR_OS_NATIVE_ERROR_CANCELLED: return ESP_ERR_TIMEOUT;
    case SOLAR_OS_NATIVE_ERROR_INVALID_SIZE: return ESP_ERR_INVALID_SIZE;
    default: return ESP_FAIL;
    }
}
static solar_os_processor_service_fn provider_services(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    return solar_os_native_library_host_v1()->get_service;
#else
    return NULL;
#endif
}
static esp_err_t acquire_provider(const char *kind, solar_os_processor_t *p)
{
#if SOLAR_OS_PACKAGE_BACKEND_PROCESSOR_HIMAX
    if (!strcmp(kind, "himax")) p->provider = solar_os_himax_processor_api();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    if (!p->provider) {
        char library[32]; snprintf(library, sizeof(library), "processor-%s", kind);
        const void *api = NULL;
        esp_err_t error = provider_error(solar_os_native_library_acquire(library,
            SOLAR_OS_PROCESSOR_PROVIDER_ABI, SOLAR_OS_PROCESSOR_PROVIDER_BASE_SIZE, &p->provider_lease, &api));
        if (error) return error;
        p->provider = api;
    }
#else
    (void)kind;
#endif
    const solar_os_processor_provider_api_v1_t *api = p->provider;
    if (!api || api->abi_version != 1 || api->struct_size < SOLAR_OS_PROCESSOR_PROVIDER_BASE_SIZE ||
        !api->kind || strcmp(api->kind, kind) || !(api->capabilities & SOLAR_OS_PROCESSOR_PROVIDER_CAPTURE) ||
        !api->query || !api->open || !api->capture || !api->release_frame || !api->close)
        return ESP_ERR_NOT_SUPPORTED;
    return ESP_OK;
}
static esp_err_t check_json(const char *json)
{
    if (!json || strnlen(json, SOLAR_OS_PROCESSOR_JSON_MAX) == SOLAR_OS_PROCESSOR_JSON_MAX)
        return ESP_ERR_INVALID_SIZE;
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    bool valid = cJSON_IsObject(root); cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
bool solar_os_processor_captures(const solar_os_processor_t *p)
{ return p && p->provider; }
esp_err_t solar_os_processor_bind_source(solar_os_processor_t *p, const char *source)
{
    if (!p || !source || !*source) return ESP_ERR_INVALID_ARG;
    if (!p->provider) return ESP_OK;
    if (p->provider_client) return ESP_ERR_INVALID_STATE;
    esp_err_t error = provider_error(p->provider->open(provider_services(), source, p->provider_model,
        p->timeout_ms, p->cancel, p->user, &p->provider_client));
    if (!error && !p->provider_client) error = ESP_ERR_INVALID_RESPONSE;
    p->source_bound = error == ESP_OK;
    return error;
}
esp_err_t solar_os_processor_capture(solar_os_processor_t *p, const char *source,
    uint64_t sequence, solar_os_media_sample_t **sample, char **json)
{
    if (!sample || !json) return ESP_ERR_INVALID_ARG;
    *sample = NULL; *json = NULL;
    if (!p || !p->source_bound || !source) return ESP_ERR_INVALID_STATE;
    solar_os_processor_frame_v1_t frame = {0};
    esp_err_t error = provider_error(p->provider->capture(p->provider_client, &frame, json));
    if (!error) error = check_json(*json);
    if (!error && (!frame.data || !frame.length || !frame.width || !frame.height)) error = ESP_ERR_INVALID_RESPONSE;
    if (!error) {
        const solar_os_media_sample_metadata_t metadata = {.source = source, .sequence = sequence,
            .timestamp_us = frame.timestamp_us, .width = frame.width, .height = frame.height};
        error = solar_os_media_sample_copy_image(frame.data, frame.length, &metadata, sample);
    }
    p->provider->release_frame(p->provider_client);
    if (error) { solar_os_memory_free(*json); *json = NULL; }
    return error;
}
esp_err_t solar_os_processor_query(const char *kind, const char *source, const char *query, char **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!solar_os_processor_kind_valid(kind) || !source || !query) return ESP_ERR_INVALID_ARG;
    esp_err_t initialized = solar_os_json_init();
    if (initialized) return initialized;
    if (!strcmp(kind, "qr") || !strcmp(kind, "model")) {
        if (strcmp(query, "info")) return ESP_ERR_NOT_SUPPORTED;
        if ((!strcmp(kind, "qr") && !SOLAR_OS_PACKAGE_SERVICE_VISION) ||
            (!strcmp(kind, "model") && !SOLAR_OS_PACKAGE_SERVICE_INFERENCE)) return ESP_ERR_NOT_SUPPORTED;
        char text[128];
        snprintf(text, sizeof(text), "{\"kind\":\"%s\",\"input\":\"image\",\"external_frames\":true}", kind);
        *out = solar_os_memory_alloc(strlen(text) + 1, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "processor.info");
        if (!*out) return ESP_ERR_NO_MEM;
        strcpy(*out, text); return ESP_OK;
    }
    solar_os_processor_t *p = NULL;
    esp_err_t error = solar_os_processor_create(kind, 0, 4000, NULL, NULL, &p);
    if (!error) error = provider_error(p->provider->query(provider_services(), source, query, 4000, out));
    if (!error) error = check_json(*out);
    solar_os_processor_destroy(p);
    if (error) { solar_os_memory_free(*out); *out = NULL; }
    return error;
}
esp_err_t solar_os_processor_select(const char *kind, const char *source, uint32_t slot,
    solar_os_processor_cancel_fn cancel, void *user, char **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!solar_os_processor_kind_valid(kind) || !source || !*source || !slot) return ESP_ERR_INVALID_ARG;
    solar_os_processor_t *p = NULL;
    esp_err_t error = solar_os_processor_create(kind, 0, 10000, cancel, user, &p);
    if (!error && (!p->provider || p->provider->struct_size <
        offsetof(solar_os_processor_provider_api_v1_t, select) + sizeof(p->provider->select) ||
        !(p->provider->capabilities & SOLAR_OS_PROCESSOR_PROVIDER_MODEL_SELECT) || !p->provider->select)) error = ESP_ERR_NOT_SUPPORTED;
    if (!error) error = provider_error(p->provider->select(provider_services(), source, slot, 10000, cancel, user, out));
    if (!error) error = check_json(*out);
    solar_os_processor_destroy(p);
    if (error) { solar_os_memory_free(*out); *out = NULL; }
    return error;
}
esp_err_t solar_os_processor_upload(const char *kind, const char *source, const char *bundle,
    uint32_t slot, uint32_t flags, solar_os_processor_cancel_fn cancel,
    solar_os_processor_progress_fn progress, void *user, char **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!solar_os_processor_kind_valid(kind) || !source || !*source || !bundle || *bundle != '/' || !slot) return ESP_ERR_INVALID_ARG;
    solar_os_processor_t *p = NULL;
    esp_err_t error = solar_os_processor_create(kind, 0, 10000, cancel, user, &p);
    if (!error && (!p->provider || p->provider->struct_size < sizeof(*p->provider) ||
        !(p->provider->capabilities & SOLAR_OS_PROCESSOR_PROVIDER_MODEL_UPLOAD) || !p->provider->upload)) error = ESP_ERR_NOT_SUPPORTED;
    if (!error) error = provider_error(p->provider->upload(provider_services(), source, bundle, slot, flags,
        10000, cancel, progress, user, out));
    if (!error) error = check_json(*out);
    solar_os_processor_destroy(p);
    if (error) { solar_os_memory_free(*out); *out = NULL; }
    return error;
}
#if SOLAR_OS_PACKAGE_SERVICE_VISION || SOLAR_OS_PACKAGE_SERVICE_INFERENCE
static bool add(cJSON *object, const char *key, cJSON *value)
{
    if (value && cJSON_AddItemToObject(object, key, value)) return true;
    cJSON_Delete(value); return false;
}
static bool num(cJSON *object, const char *key, double value)
{ return add(object, key, cJSON_CreateNumber(value)); }
static cJSON *hex(const void *data, size_t length)
{
    if (length > (SOLAR_OS_PROCESSOR_JSON_MAX - 1) / 2) return NULL;
    char *text = solar_os_memory_alloc(length * 2 + 1,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "processor.hex");
    if (!text) return NULL;
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        text[i * 2] = "0123456789abcdef"[bytes[i] >> 4];
        text[i * 2 + 1] = "0123456789abcdef"[bytes[i] & 15];
    }
    text[length * 2] = 0;
    cJSON *value = cJSON_CreateString(text); solar_os_memory_free(text); return value;
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_VISION
static esp_err_t qr_run(solar_os_processor_t *p, solar_os_raster_image_t *image, cJSON **out)
{
    solar_os_vision_qr_results_t *r = NULL;
    uint32_t width = solar_os_raster_image_width(image), height = solar_os_raster_image_height(image);
    solar_os_raster_image_convert_options_t options = {.output_width = width, .output_height = height};
    if (width > SOLAR_OS_VISION_MAX_WIDTH) {
        options.output_width = SOLAR_OS_VISION_MAX_WIDTH;
        options.output_height = (uint64_t)height * SOLAR_OS_VISION_MAX_WIDTH / width;
    }
    if (options.output_height > SOLAR_OS_VISION_MAX_HEIGHT) {
        options.output_height = SOLAR_OS_VISION_MAX_HEIGHT;
        options.output_width = (uint64_t)width * SOLAR_OS_VISION_MAX_HEIGHT / height;
    }
    if (!options.output_width) options.output_width = 1;
    if (!options.output_height) options.output_height = 1;
    esp_err_t error = solar_os_vision_qrcodes(image, &options, p->cancel, p->user, &r);
    if (error) return error;
    cJSON *root = cJSON_CreateObject(), *codes = cJSON_CreateArray();
    error = ESP_ERR_NO_MEM;
    if (!root || !codes) { cJSON_Delete(codes); goto done; }
    if (!add(root, "codes", codes)) goto done;
    if (!add(root, "kind", cJSON_CreateString("qr")) ||
        !num(root, "preprocess_us", r->preprocess_us) || !num(root, "detect_us", r->detect_us) ||
        !num(root, "decode_us", r->decode_us) || !num(root, "elapsed_us", r->elapsed_us) ||
        !num(root, "candidates", r->candidates) || !num(root, "decode_failures", r->decode_failures) ||
        !num(root, "width", r->width) || !num(root, "height", r->height) ||
        !num(root, "processed_width", r->processed_width) || !num(root, "processed_height", r->processed_height) ||
        !add(root, "truncated", cJSON_CreateBool(r->truncated))) goto done;
    for (size_t i = 0; i < r->count; ++i) {
        const solar_os_vision_qr_code_t *code = &r->codes[i];
        cJSON *entry = cJSON_CreateObject(), *corners = cJSON_CreateArray();
        if (!entry || !corners) { cJSON_Delete(entry); cJSON_Delete(corners); goto done; }
        if (!cJSON_AddItemToArray(codes, entry)) { cJSON_Delete(entry); cJSON_Delete(corners); goto done; }
        if (!add(entry, "corners", corners) || !add(entry, "payload_hex", hex(code->payload, code->length)) ||
            !num(entry, "length", code->length) || !num(entry, "eci", code->eci) ||
            !num(entry, "version", code->version) || !num(entry, "ecc_level", code->ecc_level) ||
            !num(entry, "data_type", code->data_type)) goto done;
        for (size_t j = 0; j < 4; ++j) {
            int point[2] = {code->corners[j].x, code->corners[j].y};
            cJSON *xy = cJSON_CreateIntArray(point, 2);
            if (!xy || !cJSON_AddItemToArray(corners, xy)) { cJSON_Delete(xy); goto done; }
        }
    }
    *out = root; root = NULL; error = ESP_OK;
done:
    cJSON_Delete(root); solar_os_vision_qr_results_free(r); return error;
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
static esp_err_t model_run(solar_os_processor_t *p, solar_os_raster_image_t *image, cJSON **out)
{
    solar_os_raster_image_pixels_t pixels;
    esp_err_t error = solar_os_raster_image_pixels(image, &pixels); if (error) return error;
    solar_os_inference_value_t value = {.tensor.name = p->input, .image = &pixels};
    solar_os_inference_result_t *r = NULL;
    error = solar_os_inference_run_bundle(p->client, p->model, &value, 1, p->timeout_ms, &r);
    if (error) return error;
    cJSON *root = cJSON_CreateObject(); error = ESP_ERR_NO_MEM;
    if (!root || !add(root, "kind", cJSON_CreateString("model")) ||
        !num(root, "model", p->model) || !num(root, "preprocess_us", r->preprocess_us) ||
        !num(root, "inference_us", r->inference_us) || !num(root, "input_us", r->input_us) ||
        !num(root, "output_us", r->output_us) || !num(root, "postprocess_us", r->postprocess_us) ||
        !num(root, "elapsed_us", r->elapsed_us) || !add(root, "result", cJSON_Parse(r->result_json)) ||
        !add(root, "transforms", cJSON_Parse(r->transforms_json))) goto done;
    if (!strcmp(r->result_json, "{\"kind\":\"raw\"}")) {
        size_t bytes = 0;
        for (size_t i = 0; i < r->count; ++i) {
            if (r->outputs[i].tensor.bytes > 24000 - bytes) { error = ESP_ERR_INVALID_SIZE; goto done; }
            bytes += r->outputs[i].tensor.bytes;
        }
        cJSON *tensors = cJSON_CreateObject();
        if (!add(root, "outputs", tensors)) goto done;
        for (size_t i = 0; i < r->count; ++i) {
            const solar_os_inference_tensor_t *t = &r->outputs[i].tensor;
            cJSON *entry = cJSON_CreateObject();
            if (!add(tensors, t->name, entry) || !add(entry, "dtype", cJSON_CreateString(solar_os_tensor_dtype_name(t->dtype))) ||
                !num(entry, "bytes", t->bytes) || !add(entry, "data_hex", hex(r->outputs[i].data, t->bytes))) goto done;
            cJSON *shape = cJSON_CreateArray(), *exponents = cJSON_CreateArray();
            if (!shape || !exponents) { cJSON_Delete(shape); cJSON_Delete(exponents); goto done; }
            if (!add(entry, "shape", shape)) { cJSON_Delete(exponents); goto done; }
            if (!add(entry, "exponents", exponents)) goto done;
            for (size_t j = 0; j < t->rank; ++j) {
                cJSON *n = cJSON_CreateNumber(t->shape[j]);
                if (!n || !cJSON_AddItemToArray(shape, n)) { cJSON_Delete(n); goto done; }
            }
            for (size_t j = 0; j < t->exponent_count; ++j) {
                cJSON *n = cJSON_CreateNumber(t->exponents[j]);
                if (!n || !cJSON_AddItemToArray(exponents, n)) { cJSON_Delete(n); goto done; }
            }
        }
    }
    *out = root; root = NULL; error = ESP_OK;
done:
    cJSON_Delete(root); solar_os_inference_result_free(r); return error;
}
#endif
esp_err_t solar_os_processor_create(const char *kind, uint32_t model, uint32_t timeout,
    solar_os_processor_cancel_fn cancel, void *user, solar_os_processor_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!solar_os_processor_kind_valid(kind) || !timeout || timeout > 60000) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_json_init(); if (error) return error;
    solar_os_processor_t *p = solar_os_memory_calloc(1, sizeof(*p),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "processor");
    if (!p) return ESP_ERR_NO_MEM;
    p->cancel = cancel; p->user = user; p->timeout_ms = timeout;
    error = ESP_ERR_NOT_SUPPORTED;
#if SOLAR_OS_PACKAGE_SERVICE_VISION
    if (!strcmp(kind, "qr") && !model) { p->run = qr_run; error = ESP_OK; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
    if (!strcmp(kind, "model") && model) {
        error = solar_os_inference_create(cancel, user, &p->client);
        if (!error) error = solar_os_inference_retain(model);
        if (!error) {
            p->model = model;
            const solar_os_inference_model_info_t *info;
            error = solar_os_inference_info(p->client, model, &info);
            if (!error && (!info->bundle || info->input_count != 1 || !info->image_inputs[0]))
                error = ESP_ERR_NOT_SUPPORTED;
            if (!error) { strcpy(p->input, info->inputs[0].name); p->run = model_run; }
        }
    }
#endif
    if (strcmp(kind, "qr") && strcmp(kind, "model")) {
        p->provider_model = model;
        error = acquire_provider(kind, p);
    }
    if (error) { solar_os_processor_destroy(p); return error; }
    *out = p; return ESP_OK;
}
esp_err_t solar_os_processor_close_checked(solar_os_processor_t *p)
{
    if (!p) return ESP_OK;
    if (p->provider_client) {
        p->source_bound = false;
        int result = p->provider->close(p->provider_client);
        if (result) return provider_error(result);
        p->provider_client = NULL;
    }
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    if (p->provider_lease) {
        int result = solar_os_native_library_release(p->provider_lease);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return provider_error(result);
        p->provider_lease = 0;
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
    if (p->model) {
        /* A competing request can temporarily hold global inference admission. */
        esp_err_t error = solar_os_inference_release(p->model);
        if (error == ESP_ERR_INVALID_STATE) return error;
        p->model = 0;
    }
    solar_os_inference_destroy(p->client);
#endif
    solar_os_memory_free(p);
    return ESP_OK;
}

void solar_os_processor_destroy(solar_os_processor_t *p)
{
    while (solar_os_processor_close_checked(p) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20) + 1);
}
esp_err_t solar_os_processor_run(solar_os_processor_t *p, solar_os_raster_image_t *image, char **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL; if (!p || !image) return ESP_ERR_INVALID_ARG;
    if (!p->run) return ESP_ERR_NOT_SUPPORTED;
    cJSON *root = NULL;
    esp_err_t error = p->run(p, image, &root); if (error) return error;
    char *json = solar_os_memory_alloc(SOLAR_OS_PROCESSOR_JSON_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "processor.result");
    if (!json) error = ESP_ERR_NO_MEM;
    else if (!cJSON_PrintPreallocated(root, json, SOLAR_OS_PROCESSOR_JSON_MAX, false)) error = ESP_ERR_INVALID_SIZE;
    cJSON_Delete(root);
    if (error) solar_os_memory_free(json); else *out = json;
    return error;
}

esp_err_t solar_os_processor_run_sample(solar_os_processor_t *p,
    const solar_os_media_sample_t *sample, char **out, uint64_t *decode_us)
{
    if (!out || !decode_us) return ESP_ERR_INVALID_ARG;
    *out = NULL; *decode_us = 0;
    const solar_os_media_sample_image_t *encoded = solar_os_media_sample_image(sample);
    if (!p || !encoded) return ESP_ERR_INVALID_ARG;
    if (p->provider) return ESP_ERR_NOT_SUPPORTED;
    solar_os_raster_image_t *image = NULL;
    const int64_t begin = esp_timer_get_time();
    esp_err_t error = solar_os_raster_image_decode(encoded->data, encoded->length, &image);
    *decode_us = esp_timer_get_time() - begin;
    if (!error) error = solar_os_processor_run(p, image, out);
    solar_os_raster_image_release(image);
    return error;
}
