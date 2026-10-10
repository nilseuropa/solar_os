#include "solar_os_model_bundle.h"
#include "solar_os_crypto.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MANIFEST_MAX 65536U
#define BUNDLE_PATH_MAX 256U
typedef enum { RESULT_RAW, RESULT_CLASSIFICATION, RESULT_PICO } result_kind_t;
typedef struct { uint32_t score, bbox, stride, bins; } pico_stage_t;
struct solar_os_model_bundle {
    solar_os_tensor_image_options_t images[SOLAR_OS_INFERENCE_PORTS_MAX];
    result_kind_t result;
    uint32_t output, input, top, label_count, stage_count, limit, candidate_limit;
    bool softmax;
    double score_threshold, iou_threshold;
    char **labels;
    char label[129];
    pico_stage_t stages[8];
};
static void *allocate(size_t size)
{ return solar_os_memory_calloc(1, size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.bundle"); }
static const cJSON *field(const cJSON *object, const char *name)
{ return cJSON_GetObjectItemCaseSensitive(object, name); }
static const char *text(const cJSON *value)
{ return cJSON_IsString(value) ? value->valuestring : NULL; }
static bool equal(const cJSON *value, const char *expected)
{ const char *s = text(value); return s && !strcmp(s, expected); }
static bool fields(const cJSON *object, const char *allowed, const char *required)
{
    if (!cJSON_IsObject(object)) return false;
    for (const cJSON *entry = object->child; entry; entry = entry->next) {
        if (!entry->string) return false;
        char key[132];
        if (snprintf(key, sizeof(key), "|%s|", entry->string) >= (int)sizeof(key) || !strstr(allowed, key)) return false;
        for (const cJSON *previous = object->child; previous != entry; previous = previous->next)
            if (!strcmp(previous->string, entry->string)) return false;
    }
    for (const char *start = required; *start;) {
        if (*start++ != '|') return false;
        const char *end = strchr(start, '|'); if (!end) return false;
        char key[128]; size_t length = end - start;
        if (!length || length >= sizeof(key)) return false;
        memcpy(key, start, length); key[length] = 0;
        if (!field(object, key)) return false;
        start = end + 1;
    }
    return true;
}
static bool number(const cJSON *value, double low, double high, double *out)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < low || value->valuedouble > high) return false;
    *out = value->valuedouble; return true;
}
static bool integer(const cJSON *value, double low, double high, uint32_t *out)
{
    double n;
    if (!number(value, low, high, &n) || floor(n) != n) return false;
    *out = n; return true;
}
static bool identity(const cJSON *value, char *out, size_t size)
{
    const char *s = text(value);
    if (!s || !*s || strlen(s) >= size) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("._-", *p))) return false;
    strcpy(out, s); return true;
}
static bool path_join(const char *manifest, const cJSON *value, char out[BUNDLE_PATH_MAX])
{
    const char *name = text(value);
    if (!name || !*name || strlen(name) > 200) return false;
    const char *part = name;
    for (const unsigned char *p = (const unsigned char *)name;; ++p) {
        if (*p && (*p < 32 || *p == '\\' || *p == ':')) return false;
        if (*p == '/' || !*p) {
            size_t n = (const char *)p - part;
            if (!n || (n == 1 && *part == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
            part = (const char *)p + 1;
        }
        if (!*p) break;
    }
    const char *slash = strrchr(manifest, '/'); size_t folder = slash ? (size_t)(slash - manifest) : 1;
    if (folder + 1 + strlen(name) >= BUNDLE_PATH_MAX) return false;
    if (slash) memcpy(out, manifest, folder); else out[0] = '.';
    out[folder] = '/'; strcpy(out + folder + 1, name); return true;
}
static bool hash_valid(const cJSON *value)
{
    const char *s = text(value);
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; ++i) if (!strchr("0123456789abcdef", s[i])) return false;
    return true;
}
static esp_err_t hash_file(const char *path, const cJSON *expected,
    solar_os_inference_cancel_fn cancel, void *user)
{
    if (!hash_valid(expected)) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb"); if (!file) return ESP_ERR_NOT_FOUND;
    uint8_t *buffer = allocate(8192); esp_err_t error = ESP_ERR_NO_MEM;
    solar_os_crypto_sha256_t sha; solar_os_crypto_sha256_init(&sha);
    if (!buffer) goto done;
    error = solar_os_crypto_sha256_start(&sha);
    while (!error) {
        if (cancel(user)) { error = ESP_ERR_TIMEOUT; break; }
        size_t count = fread(buffer, 1, 8192, file);
        if (count) error = solar_os_crypto_sha256_update(&sha, buffer, count);
        if (count < 8192) { if (ferror(file)) error = ESP_FAIL; break; }
        vTaskDelay(1);
    }
    if (!error) {
        uint8_t digest[32]; error = solar_os_crypto_sha256_finish(&sha, digest);
        if (!error && !solar_os_crypto_sha256_matches_hex(digest, text(expected))) error = ESP_ERR_INVALID_CRC;
    }
done:
    solar_os_crypto_sha256_free(&sha); solar_os_memory_free(buffer); fclose(file); return error;
}
static bool json_depth(const char *source, size_t length)
{
    unsigned depth = 0; bool quoted = false, escape = false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = source[i]; if (!c) return false;
        if (quoted) {
            if (c < 32) return false;
            if (escape) { escape = false; continue; }
            if (c == '\\') { escape = true; continue; }
            if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || c == '{') { if (++depth > 32) return false; }
        else if (c == ']' || c == '}') { if (!depth) return false; --depth; }
    }
    /* cJSON's strings cannot represent embedded NUL. Reject that escape before parsing. */
    return !quoted && !depth && !strstr(source, "\\u0000");
}
static esp_err_t read_manifest(const char *path, solar_os_json_doc_t **out)
{
    *out = NULL;
    FILE *file = fopen(path, "rb"); if (!file) return ESP_ERR_NOT_FOUND;
    char *source = allocate(MANIFEST_MAX + 2); esp_err_t error = ESP_ERR_NO_MEM;
    if (!source) goto done;
    size_t size = fread(source, 1, MANIFEST_MAX + 1, file);
    error = ESP_ERR_INVALID_SIZE;
    if (size > MANIFEST_MAX || ferror(file)) goto done;
    error = ESP_ERR_INVALID_ARG;
    if (!json_depth(source, size)) goto done;
    /* Require complete input, including only whitespace after the document. */
    const char *end = NULL;
    solar_os_json_init();
    cJSON *root = cJSON_ParseWithLengthOpts(source, size + 1, &end, true);
    if (!root) goto done;
    cJSON_Delete(root);
    error = solar_os_json_parse(source, size, out);
done:
    solar_os_memory_free(source); fclose(file); return error;
}
static bool channel_vector(const cJSON *options, const char *key, double values[3], unsigned channels, bool positive, bool bytes)
{
    const cJSON *array = field(options, key); if (!array) return true;
    int count = cJSON_GetArraySize(array);
    if (!cJSON_IsArray(array) || (count != 1 && count != (int)channels)) return false;
    for (unsigned c = 0; c < channels; ++c) {
        const cJSON *item = cJSON_GetArrayItem(array, count == 1 ? 0 : c);
        double v;
        if (!number(item, -HUGE_VAL, HUGE_VAL, &v) || (positive && v <= 0) || (bytes && (v < 0 || v > 255 || floor(v) != v))) return false;
        values[c] = v;
    }
    return true;
}
static bool image_options(const cJSON *options, solar_os_tensor_image_options_t *out)
{
    if (!fields(options, "|layout||color||resize||mean||std||pad||x||y||width||height|", "|layout||color||resize|")) return false;
    solar_os_tensor_image_defaults(out);
    const char *names[] = {"layout", "color", "resize"};
    for (unsigned i = 0; i < 3; ++i)
        if (solar_os_tensor_image_option(out, names[i], text(field(options, names[i]))) != ESP_OK) return false;
    unsigned channels = out->color == SOLAR_OS_TENSOR_GRAY ? 1 : 3;
    double pad[3] = {0};
    if (!channel_vector(options, "mean", out->mean, channels, false, false) ||
        !channel_vector(options, "std", out->std, channels, true, false) ||
        !channel_vector(options, "pad", pad, channels, false, true)) return false;
    for (unsigned i = 0; i < channels; ++i) out->pad[i] = pad[i];
    uint32_t *crops[] = {&out->x, &out->y, &out->width, &out->height};
    const char *keys[] = {"x", "y", "width", "height"};
    for (unsigned i = 0; i < 4; ++i)
        if (field(options, keys[i]) && !integer(field(options, keys[i]), 0, UINT32_MAX, crops[i])) return false;
    return true;
}
static bool port_contract(const cJSON *object, const solar_os_inference_tensor_t *actual, bool input,
    bool *image, solar_os_tensor_image_options_t *options)
{
    if (!fields(object, input ? "|dtype||shape||exponents||adapter|" : "|dtype||shape||exponents|",
        input ? "|dtype||shape||exponents||adapter|" : "|dtype||shape||exponents|")) return false;
    if (!equal(field(object, "dtype"), solar_os_tensor_dtype_name(actual->dtype))) return false;
    const cJSON *shape = field(object, "shape"), *exponents = field(object, "exponents");
    if (!cJSON_IsArray(shape) || cJSON_GetArraySize(shape) != (int)actual->rank ||
        !cJSON_IsArray(exponents) || cJSON_GetArraySize(exponents) != (int)actual->exponent_count) return false;
    uint32_t axis;
    for (unsigned i = 0; i < actual->rank; ++i)
        if (!integer(cJSON_GetArrayItem(shape, i), 1, SOLAR_OS_INFERENCE_TENSOR_MAX, &axis) || axis != actual->shape[i]) return false;
    for (size_t i = 0; i < actual->exponent_count; ++i) {
        double n;
        if (!number(cJSON_GetArrayItem(exponents, i), INT32_MIN, INT32_MAX, &n) || n != actual->exponents[i]) return false;
    }
    if (!input) return true;
    const cJSON *adapter = field(object, "adapter"), *config = field(adapter, "options");
    if (!fields(adapter, "|type||options|", "|type|") || (config && !cJSON_IsObject(config))) return false;
    *image = equal(field(adapter, "type"), "image");
    if (!*image) return equal(field(adapter, "type"), "tensor") && (!config || !config->child);
    if (!image_options(config, options)) return false;
    /* Inspect against a harmless RGB view to check target layout/type/quantization.
     * Source-dependent crop validation is deferred to the actual request. */
    solar_os_tensor_image_options_t check = *options; check.x = check.y = check.width = check.height = 0;
    uint8_t pixel[3] = {0}; solar_os_raster_image_pixels_t view = {pixel, 3, 3, 1, 1};
    solar_os_tensor_image_transform_t transform;
    return solar_os_tensor_image_inspect(&view, actual, &check, &transform) == ESP_OK;
}
static int port_index(const cJSON *name, const solar_os_inference_tensor_t *ports, size_t count)
{
    const char *s = text(name); if (!s) return -1;
    for (size_t i = 0; i < count; ++i) if (!strcmp(s, ports[i].name)) return i;
    return -1;
}
static esp_err_t labels_load(solar_os_model_bundle_t *bundle, const char *path, unsigned count,
    solar_os_inference_cancel_fn cancel, void *user)
{
    FILE *file = fopen(path, "rb"); if (!file) return ESP_ERR_NOT_FOUND;
    bundle->labels = allocate(count * sizeof(char *));
    char *line = allocate(1026); esp_err_t error = ESP_ERR_NO_MEM;
    if (!line || !bundle->labels) goto done;
    error = ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < count; ++i) {
        if (cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; }
        if (!fgets(line, 1026, file)) goto done;
        size_t length = strlen(line);
        if (length > 1024 || (!strchr(line, '\n') && !feof(file))) goto done;
        while (length && isspace((unsigned char)line[length - 1])) line[--length] = 0;
        char *start = line; while (isspace((unsigned char)*start)) ++start;
        if (!*start) goto done;
        bundle->labels[i] = allocate(strlen(start) + 1);
        if (!bundle->labels[i]) { error = ESP_ERR_NO_MEM; goto done; }
        strcpy(bundle->labels[i], start); ++bundle->label_count;
    }
    if (fgetc(file) != EOF || ferror(file)) goto done;
    error = ESP_OK;
done:
    solar_os_memory_free(line); fclose(file); return error;
}
static esp_err_t result_contract(solar_os_model_bundle_t *b, const cJSON *root,
    const char *manifest, const solar_os_inference_model_info_t *info,
    solar_os_inference_cancel_fn cancel, void *user)
{
    const cJSON *result = field(root, "result"), *options = field(result, "options");
    if (!fields(result, "|type||options|", "|type|") || (options && !cJSON_IsObject(options))) return ESP_ERR_INVALID_ARG;
    if (equal(field(result, "type"), "raw")) return !options || !options->child ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (equal(field(result, "type"), "classification")) {
        b->result = RESULT_CLASSIFICATION;
        if (!fields(options, "|tensor||labels||activation||top|", "|tensor||labels||activation|")) return ESP_ERR_INVALID_ARG;
        int output = port_index(field(options, "tensor"), info->outputs, info->output_count);
        if (output < 0) return ESP_ERR_INVALID_ARG;
        b->output = output; const solar_os_inference_tensor_t *port = &info->outputs[output];
        if ((port->rank != 1 && (port->rank != 2 || port->shape[0] != 1)) ||
            (port->dtype != SOLAR_OS_TENSOR_INT8 && port->dtype != SOLAR_OS_TENSOR_UINT8 &&
             port->dtype != SOLAR_OS_TENSOR_INT16 && port->dtype != SOLAR_OS_TENSOR_FLOAT32) ||
            (port->dtype != SOLAR_OS_TENSOR_FLOAT32 && (port->exponent_count != 1 || port->exponents[0] < -126 || port->exponents[0] > 126))) return ESP_ERR_NOT_SUPPORTED;
        unsigned count = port->shape[port->rank - 1]; b->top = 5;
        if (field(options, "top") && !integer(field(options, "top"), 1, count < 1000 ? count : 1000, &b->top)) return ESP_ERR_INVALID_ARG;
        if (b->top > count) return ESP_ERR_INVALID_ARG;
        b->softmax = equal(field(options, "activation"), "softmax");
        if (!b->softmax && !equal(field(options, "activation"), "identity")) return ESP_ERR_INVALID_ARG;
        char path[BUNDLE_PATH_MAX]; const char *name = text(field(options, "labels"));
        if (!name || !field(field(root, "assets"), name) || !path_join(manifest, field(options, "labels"), path)) return ESP_ERR_INVALID_ARG;
        return labels_load(b, path, count, cancel, user);
    }
    if (!equal(field(result, "type"), "pico-detection")) return ESP_ERR_NOT_SUPPORTED;
    b->result = RESULT_PICO;
    if (!fields(options, "|input||stages||score_threshold||iou_threshold||limit||candidate_limit||label|", "|input||stages||label|")) return ESP_ERR_INVALID_ARG;
    int input = port_index(field(options, "input"), info->inputs, info->input_count);
    if (input < 0 || !info->image_inputs[input]) return ESP_ERR_INVALID_ARG;
    b->input = input;
    if (b->images[input].layout != SOLAR_OS_TENSOR_NHWC && b->images[input].layout != SOLAR_OS_TENSOR_HWC) return ESP_ERR_NOT_SUPPORTED;
    b->score_threshold = .7; b->iou_threshold = .5; b->limit = 10; b->candidate_limit = 128;
    if ((field(options, "score_threshold") && !number(field(options, "score_threshold"), 0, 1, &b->score_threshold)) ||
        (field(options, "iou_threshold") && !number(field(options, "iou_threshold"), 0, 1, &b->iou_threshold)) ||
        b->score_threshold <= 0 || b->iou_threshold <= 0 ||
        (field(options, "limit") && !integer(field(options, "limit"), 1, 100, &b->limit)) ||
        (field(options, "candidate_limit") && !integer(field(options, "candidate_limit"), b->limit, 1024, &b->candidate_limit)) || b->candidate_limit < b->limit) return ESP_ERR_INVALID_ARG;
    const char *label = text(field(options, "label"));
    if (!label || strlen(label) > 128) return ESP_ERR_INVALID_ARG;
    strcpy(b->label, label);
    const cJSON *stages = field(options, "stages"); int count = cJSON_GetArraySize(stages);
    if (!cJSON_IsArray(stages) || count < 1 || count > 8) return ESP_ERR_INVALID_ARG;
    bool used[SOLAR_OS_INFERENCE_PORTS_MAX] = {0};
    const solar_os_inference_tensor_t *in = &info->inputs[input];
    unsigned height = in->shape[in->rank - 3], width = in->shape[in->rank - 2];
    for (int i = 0; i < count; ++i) {
        const cJSON *stage = cJSON_GetArrayItem(stages, i);
        if (!fields(stage, "|score||bbox||stride||bins|", "|score||bbox||stride||bins|")) return ESP_ERR_INVALID_ARG;
        int score = port_index(field(stage, "score"), info->outputs, info->output_count);
        int bbox = port_index(field(stage, "bbox"), info->outputs, info->output_count);
        pico_stage_t *p = &b->stages[i];
        if (score < 0 || bbox < 0 || score == bbox || used[score] || used[bbox] ||
            !integer(field(stage, "stride"), 1, 1024, &p->stride) || !integer(field(stage, "bins"), 2, 32, &p->bins)) return ESP_ERR_INVALID_ARG;
        p->score = score; p->bbox = bbox; used[score] = used[bbox] = true;
        const solar_os_inference_tensor_t *s = &info->outputs[score], *box = &info->outputs[bbox];
        if (s->dtype != SOLAR_OS_TENSOR_INT8 || box->dtype != SOLAR_OS_TENSOR_INT8 || s->rank != 4 || box->rank != 4 ||
            s->shape[0] != 1 || s->shape[3] != 1 || memcmp(s->shape, box->shape, 3 * sizeof(uint32_t)) || box->shape[3] != 4 * p->bins ||
            s->exponent_count != 1 || box->exponent_count != 1 || s->exponents[0] < -126 || s->exponents[0] > 126 ||
            box->exponents[0] < -126 || box->exponents[0] > 126 || (uint64_t)s->shape[1] * p->stride != height || (uint64_t)s->shape[2] * p->stride != width) return ESP_ERR_INVALID_ARG;
        ++b->stage_count;
    }
    return ESP_OK;
}
void solar_os_model_bundle_free(solar_os_model_bundle_t *b)
{
    if (!b) return;
    for (unsigned i = 0; i < b->label_count; ++i) solar_os_memory_free(b->labels[i]);
    solar_os_memory_free(b->labels); solar_os_memory_free(b);
}
esp_err_t solar_os_model_bundle_load(const char *manifest, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_model_bundle_t **out, solar_os_inference_backend_t **backend,
    solar_os_inference_model_info_t *info)
{
    *out = NULL; solar_os_json_doc_t *doc = NULL; solar_os_model_bundle_t *b = NULL;
    esp_err_t error = read_manifest(manifest, &doc); if (error) return error;
    const cJSON *root = (const cJSON *)solar_os_json_root(doc);
    char model[BUNDLE_PATH_MAX]; uint32_t schema;
    error = ESP_ERR_INVALID_ARG;
    if (!fields(root, "|schema||id||version||runtime||model||inputs||outputs||result||assets||license|", "|schema||id||version||runtime||model||inputs||outputs||result|") ||
        !integer(field(root, "schema"), 1, 1, &schema) || !identity(field(root, "id"), info->bundle_id, sizeof(info->bundle_id)) ||
        !identity(field(root, "version"), info->bundle_version, sizeof(info->bundle_version))) goto done;
    const cJSON *runtime = field(root, "runtime"), *definition = field(root, "model"), *assets = field(root, "assets");
    if (!fields(runtime, "|backend||version||target|", "|backend||version||target|") || !equal(field(runtime, "backend"), "espdl") ||
        !equal(field(runtime, "version"), SOLAR_OS_INFERENCE_BACKEND_VERSION) || !equal(field(runtime, "target"), "esp32s3") ||
        !fields(definition, "|file||sha256|", "|file||sha256|") || !path_join(manifest, field(definition, "file"), model)) goto done;
    if (assets && (!cJSON_IsObject(assets) || cJSON_GetArraySize(assets) > 32)) goto done;
    const cJSON *license = field(root, "license");
    if (license && (!fields(license, "|id||file|", "|id||file|") || !text(field(license, "id")) || !*text(field(license, "id")) ||
        !text(field(license, "file")) || !field(assets, text(field(license, "file"))))) goto done;
    error = hash_file(model, field(definition, "sha256"), cancel, user); if (error) goto done;
    for (const cJSON *asset = assets ? assets->child : NULL; asset; asset = asset->next) {
        char path[BUNDLE_PATH_MAX]; cJSON name = {.type = cJSON_String, .valuestring = asset->string};
        error = ESP_ERR_INVALID_ARG;
        if (!strcmp(asset->string, text(field(definition, "file"))) || !path_join(manifest, &name, path)) goto done;
        for (const cJSON *previous = assets->child; previous != asset; previous = previous->next)
            if (!strcmp(previous->string, asset->string)) goto done;
        error = hash_file(path, asset, cancel, user); if (error) goto done;
    }
    b = allocate(sizeof(*b)); error = ESP_ERR_NO_MEM; if (!b) goto done;
    error = solar_os_inference_backend_load(model, cancel, user, backend, info); if (error) goto done;
    /* Backend fills port metadata without overwriting bundle identity fields. */
    for (unsigned side = 0; side < 2; ++side) {
        const cJSON *ports = field(root, side ? "outputs" : "inputs");
        const solar_os_inference_tensor_t *actual = side ? info->outputs : info->inputs;
        size_t count = side ? info->output_count : info->input_count;
        error = ESP_ERR_INVALID_ARG;
        if (!cJSON_IsObject(ports) || cJSON_GetArraySize(ports) != (int)count) goto done;
        for (size_t i = 0; i < count; ++i) {
            unsigned occurrences = 0;
            for (const cJSON *p = ports->child; p; p = p->next) if (!strcmp(p->string, actual[i].name)) ++occurrences;
            if (occurrences != 1 || !port_contract(field(ports, actual[i].name), &actual[i], !side,
                &info->image_inputs[i], &b->images[i])) goto done;
        }
    }
    error = result_contract(b, root, manifest, info, cancel, user); if (error) goto done;
    if (cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; }
    info->bundle = true; *out = b; b = NULL;
done:
    solar_os_model_bundle_free(b); solar_os_json_free(doc); return error;
}

typedef struct { double score; int box[4]; } candidate_t;
static bool add_number(cJSON *object, const char *key, double value)
{ return cJSON_AddNumberToObject(object, key, value) != NULL; }
static bool add_string(cJSON *object, const char *key, const char *value)
{ return cJSON_AddStringToObject(object, key, value) != NULL; }
static cJSON *transform_json(const solar_os_tensor_image_transform_t *t)
{
    cJSON *object = cJSON_CreateObject(); if (!object) return NULL;
#define FIELD(name) if (!add_number(object, #name, t->name)) goto fail
    FIELD(source_width); FIELD(source_height); FIELD(input_width); FIELD(input_height);
    FIELD(crop_x); FIELD(crop_y); FIELD(crop_width); FIELD(crop_height);
    FIELD(resized_width); FIELD(resized_height); FIELD(pad_left); FIELD(pad_top);
#undef FIELD
    return object;
fail:
    cJSON_Delete(object); return NULL;
}
static double tensor_value(const solar_os_inference_tensor_t *p, const void *buffer, unsigned i)
{
    const uint8_t *data = buffer; double value;
    if (p->dtype == SOLAR_OS_TENSOR_INT8) value = (int8_t)data[i];
    else if (p->dtype == SOLAR_OS_TENSOR_UINT8) value = data[i];
    else if (p->dtype == SOLAR_OS_TENSOR_INT16) value = (int16_t)(data[2*i] | (uint16_t)data[2*i+1] << 8);
    else { float number; memcpy(&number, data + 4*i, 4); value = number; }
    return p->dtype == SOLAR_OS_TENSOR_FLOAT32 ? value : ldexp(value, p->exponents[0]);
}
static esp_err_t classify(const solar_os_model_bundle_t *b, const solar_os_inference_result_t *r,
    solar_os_inference_cancel_fn cancel, void *user, cJSON **out)
{
    const solar_os_inference_tensor_t *port = &r->outputs[b->output].tensor;
    unsigned count = b->label_count;
    double *values = allocate(count * sizeof(*values)); unsigned *indices = allocate(b->top * sizeof(*indices));
    cJSON *object = cJSON_CreateObject(), *classes = cJSON_CreateArray(); esp_err_t error = ESP_ERR_NO_MEM;
    if (!values || !indices || !object || !classes) goto done;
    double maximum = -HUGE_VAL, total = 0;
    for (unsigned i = 0; i < count; ++i) {
        values[i] = tensor_value(port, r->outputs[b->output].data, i);
        if (!isfinite(values[i])) { error = ESP_ERR_INVALID_ARG; goto done; }
        if (values[i] > maximum) maximum = values[i];
    }
    unsigned selected = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned at = 0; while (at < selected && values[indices[at]] >= values[i]) ++at;
        if (at < b->top) {
            if (selected < b->top) ++selected;
            for (unsigned j = selected - 1; j > at; --j) indices[j] = indices[j-1];
            indices[at] = i;
        }
        if (b->softmax) total += exp(values[i] - maximum);
        if ((i & 255U) == 255U) { if (cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; } vTaskDelay(1); }
    }
    for (unsigned i = 0; i < selected; ++i) {
        unsigned index = indices[i]; double score = b->softmax ? exp(values[index] - maximum) / total : values[index];
        cJSON *entry = cJSON_CreateObject(); if (!entry) goto done;
        if (!add_number(entry, "id", index) || !add_number(entry, "score", score) || !add_string(entry, "label", b->labels[index]) ||
            !cJSON_AddItemToArray(classes, entry)) { cJSON_Delete(entry); goto done; }
    }
    if (!add_string(object, "kind", "classification") || !cJSON_AddItemToObject(object, "classes", classes)) goto done;
    classes = NULL; *out = object; object = NULL; error = ESP_OK;
done:
    solar_os_memory_free(values); solar_os_memory_free(indices); cJSON_Delete(classes); cJSON_Delete(object); return error;
}
static double integral(const int8_t *data, unsigned bins, double scale)
{
    int maximum = -128; for (unsigned i = 0; i < bins; ++i) if (data[i] > maximum) maximum = data[i];
    double sum = 0, weighted = 0;
    for (unsigned i = 0; i < bins; ++i) { double w = exp((data[i] - maximum) * scale); sum += w; weighted += i * w; }
    return weighted / sum;
}
static double iou(const candidate_t *a, const candidate_t *b)
{
    double area_a = fmax(0, (double)a->box[2] - a->box[0] + 1) * fmax(0, (double)a->box[3] - a->box[1] + 1);
    double area_b = fmax(0, (double)b->box[2] - b->box[0] + 1) * fmax(0, (double)b->box[3] - b->box[1] + 1);
    double intersection = fmax(0, fmin(a->box[2], b->box[2]) - fmax(a->box[0], b->box[0]) + 1) *
        fmax(0, fmin(a->box[3], b->box[3]) - fmax(a->box[1], b->box[1]) + 1);
    double sum = area_a + area_b - intersection; return sum ? intersection / sum : 0;
}
static esp_err_t detect(const solar_os_model_bundle_t *b, const solar_os_inference_result_t *r,
    const solar_os_tensor_image_transform_t *t, solar_os_inference_cancel_fn cancel, void *user, cJSON **out)
{
    candidate_t *pool = allocate(b->candidate_limit * sizeof(*pool)), *kept = allocate(b->limit * sizeof(*kept));
    cJSON *object = cJSON_CreateObject(), *detections = cJSON_CreateArray(); esp_err_t error = ESP_ERR_NO_MEM;
    if (!pool || !kept || !object || !detections) goto done;
    unsigned count = 0, qualifying = 0, kept_count = 0; bool limited = false;
    for (unsigned s = 0; s < b->stage_count; ++s) {
        const pico_stage_t *stage = &b->stages[s];
        const solar_os_inference_tensor_t *score = &r->outputs[stage->score].tensor, *bbox = &r->outputs[stage->bbox].tensor;
        const int8_t *scores = r->outputs[stage->score].data, *boxes = r->outputs[stage->bbox].data;
        double scale = ldexp(1, score->exponents[0]), box_scale = ldexp(1, bbox->exponents[0]);
        double cutoff = floor(b->score_threshold * b->score_threshold / scale + .5);
        unsigned width = score->shape[2], cells = width * score->shape[1];
        for (unsigned i = 0; i < cells; ++i) {
            if ((i & 127U) == 127U) { if (cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; } vTaskDelay(1); }
            if (scores[i] <= cutoff) continue;
            ++qualifying; double distance[4];
            for (unsigned side = 0; side < 4; ++side)
                distance[side] = integral(boxes + (i * 4 + side) * stage->bins, stage->bins, box_scale) * stage->stride;
            double cx = (i % width) * stage->stride + stage->stride / 2, cy = (i / width) * stage->stride + stage->stride / 2;
            double coordinates[] = {cx - distance[0], cy - distance[1], cx + distance[2], cy + distance[3]};
            candidate_t candidate = {.score = sqrt(scores[i] * scale)};
            for (unsigned side = 0; side < 4; ++side) {
                bool y = side & 1;
                double v = (coordinates[side] - (y ? t->pad_top : t->pad_left)) *
                    (y ? t->crop_height : t->crop_width) / (y ? t->resized_height : t->resized_width) + (y ? t->crop_y : t->crop_x);
                if (v < INT_MIN || v > INT_MAX) { error = ESP_ERR_INVALID_SIZE; goto done; }
                candidate.box[side] = v;
            }
            if (count < b->candidate_limit) pool[count++] = candidate;
            else {
                unsigned lowest = 0; for (unsigned j = 1; j < count; ++j) if (pool[j].score < pool[lowest].score) lowest = j;
                if (candidate.score > pool[lowest].score) pool[lowest] = candidate;
            }
        }
    }
    /* Stable sort keeps equal-score results consistent with stage/cell ordering. */
    for (unsigned i = 1; i < count; ++i) {
        candidate_t value = pool[i]; unsigned j = i;
        while (j && pool[j-1].score < value.score) { pool[j] = pool[j-1]; --j; }
        pool[j] = value;
    }
    for (unsigned i = 0; i < count; ++i) {
        if ((i & 31U) == 31U) { if (cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; } vTaskDelay(1); }
        bool suppressed = false;
        for (unsigned j = 0; j < kept_count; ++j) if (iou(&pool[i], &kept[j]) > b->iou_threshold) { suppressed = true; break; }
        if (suppressed) continue;
        if (kept_count == b->limit) { limited = true; break; }
        kept[kept_count++] = pool[i];
    }
    for (unsigned i = 0; i < kept_count; ++i) {
        cJSON *entry = cJSON_CreateObject(), *box = cJSON_CreateArray();
        if (!entry || !box) { cJSON_Delete(entry); cJSON_Delete(box); goto done; }
        for (unsigned side = 0; side < 4; ++side) {
            int maximum = (side & 1 ? t->source_height : t->source_width) - 1;
            int coordinate = kept[i].box[side]; if (coordinate < 0) coordinate = 0; if (coordinate > maximum) coordinate = maximum;
            cJSON *n = cJSON_CreateNumber(coordinate);
            if (!n || !cJSON_AddItemToArray(box, n)) { cJSON_Delete(n); cJSON_Delete(box); cJSON_Delete(entry); goto done; }
        }
        if (!cJSON_AddItemToObject(entry, "box", box)) { cJSON_Delete(box); cJSON_Delete(entry); goto done; }
        if (!add_number(entry, "id", 0) || !add_number(entry, "score", kept[i].score) || !add_string(entry, "label", b->label) ||
            !cJSON_AddItemToArray(detections, entry)) { cJSON_Delete(entry); goto done; }
    }
    if (!add_string(object, "kind", "detection") || !add_string(object, "coordinates", "source_pixels") ||
        !add_number(object, "width", t->source_width) || !add_number(object, "height", t->source_height) ||
        !cJSON_AddBoolToObject(object, "truncated", qualifying > b->candidate_limit || limited) ||
        !cJSON_AddItemToObject(object, "detections", detections)) goto done;
    detections = NULL; *out = object; object = NULL; error = ESP_OK;
done:
    solar_os_memory_free(pool); solar_os_memory_free(kept); cJSON_Delete(object); cJSON_Delete(detections); return error;
}
esp_err_t solar_os_model_bundle_run(const solar_os_model_bundle_t *b,
    const solar_os_inference_model_info_t *info, solar_os_inference_backend_t *backend,
    const solar_os_inference_value_t *values, size_t count, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_inference_result_t *r)
{
    if (!b || count != info->input_count) return ESP_ERR_INVALID_ARG;
    solar_os_inference_input_t *inputs = allocate(count * sizeof(*inputs));
    solar_os_tensor_image_transform_t *transforms = allocate(count * sizeof(*transforms));
    void *buffers[SOLAR_OS_INFERENCE_PORTS_MAX] = {0}; bool used[SOLAR_OS_INFERENCE_PORTS_MAX] = {0};
    cJSON *mapping = cJSON_CreateObject(), *payload = NULL; esp_err_t error = ESP_ERR_NO_MEM;
    if (!inputs || !transforms || !mapping) goto done;
    for (size_t i = 0; i < count; ++i) {
        if (cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; }
        const char *name = values[i].tensor.name; error = ESP_ERR_INVALID_ARG;
        if (!name) goto done;
        size_t port = 0; while (port < count && strcmp(name, info->inputs[port].name)) ++port;
        if (port == count || used[port]) goto done;
        used[port] = true; inputs[port] = values[i].tensor;
        const solar_os_inference_tensor_t *expected = &info->inputs[port];
        if (info->image_inputs[port]) {
            if (!values[i].image || values[i].tensor.data || values[i].tensor.tensor) goto done;
            error = solar_os_tensor_image_inspect(values[i].image, expected, &b->images[port], &transforms[port]); if (error) goto done;
            buffers[port] = allocate(expected->bytes); error = ESP_ERR_NO_MEM; if (!buffers[port]) goto done;
            error = solar_os_tensor_image_prepare(values[i].image, expected, &b->images[port], buffers[port], expected->bytes,
                &transforms[port], cancel, user); if (error) goto done;
            r->preprocess_us += transforms[port].preprocess_us; inputs[port].data = buffers[port]; inputs[port].bytes = expected->bytes;
            cJSON *transform = transform_json(&transforms[port]); error = ESP_ERR_NO_MEM;
            if (!transform || !cJSON_AddItemToObject(mapping, name, transform)) { cJSON_Delete(transform); goto done; }
        } else {
            error = ESP_ERR_INVALID_ARG;
            if (values[i].image || !inputs[port].data) goto done;
            if (inputs[port].bytes != expected->bytes) { error = ESP_ERR_INVALID_SIZE; goto done; }
            const solar_os_inference_tensor_t *typed = inputs[port].tensor;
            if (typed && (typed->dtype != expected->dtype || typed->rank != expected->rank || typed->bytes != expected->bytes ||
                memcmp(typed->shape, expected->shape, expected->rank * sizeof(uint32_t)) || typed->exponent_count != expected->exponent_count ||
                (typed->exponent_count && (!typed->exponents || memcmp(typed->exponents, expected->exponents, typed->exponent_count * sizeof(int32_t)))))) goto done;
        }
    }
    error = solar_os_inference_backend_run(backend, inputs, count, cancel, user, r); if (error) goto done;
    /* Backend output iteration order may differ from input descriptors. Resolve by name. */
    if (r->count != info->output_count) { error = ESP_FAIL; goto done; }
    for (size_t i = 0; i < r->count; ++i) {
        size_t j = i; while (j < r->count && strcmp(r->outputs[j].tensor.name, info->outputs[i].name)) ++j;
        if (j == r->count) { error = ESP_FAIL; goto done; }
        if (j != i) {
            solar_os_inference_tensor_t tensor = r->outputs[i].tensor; void *data = r->outputs[i].data;
            r->outputs[i] = r->outputs[j]; r->outputs[j].tensor = tensor; r->outputs[j].data = data;
        }
    }
    int64_t begin = esp_timer_get_time();
    if (b->result == RESULT_CLASSIFICATION) error = classify(b, r, cancel, user, &payload);
    else if (b->result == RESULT_PICO) error = detect(b, r, &transforms[b->input], cancel, user, &payload);
    else {
        payload = cJSON_CreateObject(); error = payload && add_string(payload, "kind", "raw") ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (error) goto done;
    if (cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; }
    r->postprocess_us = esp_timer_get_time() - begin;
    r->result_json = cJSON_PrintUnformatted(payload); r->transforms_json = cJSON_PrintUnformatted(mapping);
    if (!r->result_json || !r->transforms_json) error = ESP_ERR_NO_MEM;
done:
    for (size_t i = 0; i < SOLAR_OS_INFERENCE_PORTS_MAX; ++i) solar_os_memory_free(buffers[i]);
    solar_os_memory_free(inputs); solar_os_memory_free(transforms); cJSON_Delete(mapping); cJSON_Delete(payload); return error;
}
