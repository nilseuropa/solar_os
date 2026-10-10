#include "solar_os_processor.h"
#include "solar_os_config.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
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
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
    solar_os_inference_t *client;
    char input[SOLAR_OS_INFERENCE_NAME_MAX];
#endif
};
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
    if (!kind || !timeout || timeout > 60000) return ESP_ERR_INVALID_ARG;
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
    if (error) { solar_os_processor_destroy(p); return error; }
    *out = p; return ESP_OK;
}
void solar_os_processor_destroy(solar_os_processor_t *p)
{
    if (!p) return;
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
    if (p->model) {
        /* A competing request can temporarily hold global inference admission. */
        while (solar_os_inference_release(p->model) == ESP_ERR_INVALID_STATE) vTaskDelay(1);
    }
    solar_os_inference_destroy(p->client);
#endif
    solar_os_memory_free(p);
}
esp_err_t solar_os_processor_run(solar_os_processor_t *p, solar_os_raster_image_t *image, char **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL; if (!p || !image) return ESP_ERR_INVALID_ARG;
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
