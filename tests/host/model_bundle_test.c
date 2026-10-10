/* Real service, parser, preparation and result adapters; fake numerical backend. */
#define INFERENCE_NATIVE_BUNDLE_TEST 1
#define main ownership_suite
#define solar_os_inference_backend_load base_load
#define solar_os_inference_backend_run base_run
#include "inference_test.c"
#undef main
#undef solar_os_inference_backend_load
#undef solar_os_inference_backend_run
#include "solar_os_crypto.h"
#include "solar_os_json.h"
#include "cJSON.h"
#include <openssl/evp.h>
#include <unistd.h>
#include <math.h>

void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *sha) { memset(sha, 0, sizeof(*sha)); }
void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *sha) { EVP_MD_CTX_free(sha->ctx.pointer); }
esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *sha)
{
    sha->ctx.pointer = EVP_MD_CTX_new();
    return sha->ctx.pointer && EVP_DigestInit_ex(sha->ctx.pointer, EVP_sha256(), NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *sha, const void *data, size_t size)
{ return EVP_DigestUpdate(sha->ctx.pointer, data, size) ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *sha, uint8_t digest[32])
{ return EVP_DigestFinal_ex(sha->ctx.pointer, digest, NULL) ? ESP_OK : ESP_FAIL; }
bool solar_os_crypto_sha256_matches_hex(const uint8_t digest[32], const char *expected)
{
    char actual[65]; for (unsigned i = 0; i < 32; ++i) snprintf(actual + 2*i, 3, "%02x", digest[i]);
    return !strcmp(actual, expected);
}
static void *json_allocate(size_t size)
{ return solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.json"); }
struct solar_os_json_doc { cJSON *root; };
esp_err_t solar_os_json_init(void)
{ cJSON_Hooks hooks = {json_allocate, solar_os_memory_free}; cJSON_InitHooks(&hooks); return ESP_OK; }
esp_err_t solar_os_json_parse(const char *source, size_t size, solar_os_json_doc_t **out)
{
    *out = NULL; cJSON *root = cJSON_ParseWithLength(source, size);
    if (!root) return ESP_ERR_INVALID_ARG;
    *out = json_allocate(sizeof(**out));
    if (!*out) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    (*out)->root = root; return ESP_OK;
}
void solar_os_json_free(solar_os_json_doc_t *doc)
{ if (doc) { cJSON_Delete(doc->root); solar_os_memory_free(doc); } }
const solar_os_json_value_t *solar_os_json_root(const solar_os_json_doc_t *doc) { return doc->root; }

static int profile;
esp_err_t solar_os_inference_backend_load(const char *path, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_inference_backend_t **out, solar_os_inference_model_info_t *info)
{
    if (!profile) return base_load("/model.espdl", cancel, user, out, info);
    (void)path;
    *out = solar_os_memory_calloc(1, sizeof(**out), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.backend");
    if (!*out) return ESP_ERR_NO_MEM;
    info->input_count = 1; info->output_count = profile == 1 ? 1 : 2;
    esp_err_t error = tensor(&info->inputs[0], "pixels"); if (error) return error;
    info->inputs[0].rank = 4; info->inputs[0].shape[0] = 1; info->inputs[0].shape[1] = 2;
    info->inputs[0].shape[2] = profile == 1 ? 3 : 2; info->inputs[0].shape[3] = 3;
    info->inputs[0].bytes = profile == 1 ? 18 : 12;
    error = tensor(&info->outputs[0], profile == 1 ? "scores" : "score"); if (error) return error;
    if (profile == 1) {
        info->outputs[0].shape[0] = 1; info->outputs[0].shape[1] = 3; info->outputs[0].bytes = 3;
        info->outputs[0].exponents[0] = -2;
    } else {
        info->outputs[0].rank = 4; for (unsigned i = 0; i < 4; ++i) info->outputs[0].shape[i] = 1;
        info->outputs[0].bytes = 1; info->outputs[0].exponents[0] = -7;
        error = tensor(&info->outputs[1], "bbox"); if (error) return error;
        info->outputs[1].rank = 4; info->outputs[1].shape[0] = info->outputs[1].shape[1] = info->outputs[1].shape[2] = 1;
        info->outputs[1].shape[3] = 8;
    }
    return ESP_OK;
}
esp_err_t solar_os_inference_backend_run(solar_os_inference_backend_t *backend,
    const solar_os_inference_input_t *inputs, size_t count, solar_os_inference_cancel_fn cancel,
    void *user, solar_os_inference_result_t *result)
{
    if (!profile) return base_run(backend, inputs, count, cancel, user, result);
    assert(count == 1 && !strcmp(inputs[0].name, "pixels"));
    if (cancel(user)) return ESP_ERR_TIMEOUT;
    assert(inputs[0].bytes == (profile == 1 ? 18U : 12U));
    assert(((const int8_t *)inputs[0].data)[0] == 127);
    assert(((const int8_t *)inputs[0].data)[1] == 0);
    /* Fill independent output descriptors, as the actual backend does. */
    for (unsigned i = 0; i < (profile == 1 ? 1U : 2U); ++i) {
        esp_err_t error = tensor(&result->outputs[i].tensor, profile == 1 ? "scores" : i ? "bbox" : "score");
        if (error) return error;
        solar_os_inference_tensor_t *port = &result->outputs[i].tensor;
        if (profile == 1) { port->shape[0] = 1; port->shape[1] = 3; port->bytes = 3; port->exponents[0] = -2; }
        else {
            port->rank = 4; port->shape[0] = port->shape[1] = port->shape[2] = 1;
            port->shape[3] = i ? 8 : 1; port->bytes = i ? 8 : 1; port->exponents[0] = i ? 0 : -7;
        }
        result->outputs[i].data = solar_os_memory_calloc(1, port->bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.output");
        if (!result->outputs[i].data) return ESP_ERR_NO_MEM;
        if (profile == 1) { const int8_t values[] = {8, 4, -4}; memcpy(result->outputs[i].data, values, 3); }
        else if (!i) *(int8_t *)result->outputs[i].data = 100;
        ++result->count;
    }
    return ESP_OK;
}
static void write_file(const char *path, const char *data)
{ FILE *file = fopen(path, "wb"); assert(file); assert(fwrite(data, 1, strlen(data), file) == strlen(data)); assert(!fclose(file)); }
static void digest(const char *data, char out[65])
{
    uint8_t sha[32]; unsigned length;
    assert(EVP_Digest(data, strlen(data), sha, &length, EVP_sha256(), NULL));
    for (unsigned i = 0; i < 32; ++i) snprintf(out + 2*i, 3, "%02x", sha[i]);
}
int main(void)
{
    /* The base suite tests /missing; retain its actual path check during that suite. */
    solar_os_json_init();
    char directory[] = "/tmp/solaros-native-bundle-XXXXXX"; assert(mkdtemp(directory));
    char model[256], manifest[256], labels[256], model_sha[65], labels_sha[65];
    snprintf(model, sizeof(model), "%s/model.espdl", directory);
    snprintf(manifest, sizeof(manifest), "%s/bundle.json", directory);
    snprintf(labels, sizeof(labels), "%s/labels.txt", directory);
    write_file(model, "test-model"); digest("test-model", model_sha);
    write_file(labels, "red\nblue\ngreen\n"); digest("red\nblue\ngreen\n", labels_sha);
    char source[4096];
    const char *format = "{\"schema\":1,\"id\":\"test\",\"version\":\"1\","
        "\"runtime\":{\"backend\":\"espdl\",\"version\":\"3.3.13\",\"target\":\"esp32s3\"},"
        "\"model\":{\"file\":\"model.espdl\",\"sha256\":\"%s\"},"
        "\"assets\":{\"labels.txt\":\"%s\"},\"inputs\":%s,\"outputs\":%s,\"result\":%s}";
    const char *raw_inputs = "{\"a\":{\"dtype\":\"int8\",\"shape\":[2,4],\"exponents\":[0],\"adapter\":{\"type\":\"tensor\"}},"
        "\"b\":{\"dtype\":\"int8\",\"shape\":[2,4],\"exponents\":[0],\"adapter\":{\"type\":\"tensor\"}}}";
    const char *raw_outputs = "{\"sum\":{\"dtype\":\"int8\",\"shape\":[2,4],\"exponents\":[0]},"
        "\"difference\":{\"dtype\":\"int8\",\"shape\":[2,4],\"exponents\":[0]}}";
    snprintf(source, sizeof(source), format, model_sha, labels_sha, raw_inputs, raw_outputs, "{\"type\":\"raw\"}");
    write_file(manifest, source);
    solar_os_inference_t *client; assert(solar_os_inference_create(NULL, NULL, &client) == ESP_OK);
    int baseline = allocations; uint32_t handle;
    for (int failure = 0; failure < 140; ++failure) {
        failures = failure;
        esp_err_t error = solar_os_inference_load_bundle(client, manifest, 1000, &handle); failures = -1;
        if (!error) assert(solar_os_inference_close(client, handle) == ESP_OK);
        assert(allocations == baseline && !workers);
    }
    assert(solar_os_inference_load_bundle(client, manifest, 1000, &handle) == ESP_OK);
    solar_os_inference_value_t raw[] = {{{"a", a, 8, NULL}, NULL}, {{"b", b, 8, NULL}, NULL}};
    solar_os_inference_result_t *r;
    assert(solar_os_inference_run_bundle(client, handle, raw, 2, 1000, &r) == ESP_OK);
    assert(((int8_t *)r->outputs[0].data)[0] == 3 && !strcmp(r->result_json, "{\"kind\":\"raw\"}"));
    solar_os_inference_result_free(r);
    assert(solar_os_inference_close(client, handle) == ESP_OK);
    cJSON *bad = cJSON_Parse(source); assert(bad);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(bad, "model"), "sha256", cJSON_CreateString("0000000000000000000000000000000000000000000000000000000000000000"));
    char *changed = cJSON_PrintUnformatted(bad); write_file(manifest, changed); cJSON_free(changed); cJSON_Delete(bad);
    assert(solar_os_inference_load_bundle(client, manifest, 1000, &handle) == ESP_ERR_INVALID_CRC);
    assert(allocations == baseline);
    write_file(manifest, "{\"schema\":1,\"schema\":1}");
    assert(solar_os_inference_load_bundle(client, manifest, 1000, &handle) != ESP_OK);
    /* Mutations span pre-construction and post-construction validation failures. */
    for (unsigned failure = 0; failure < 10; ++failure) {
        bad = cJSON_Parse(source); assert(bad);
        cJSON *object = bad; const char *key = "schema";
        cJSON *replacement = cJSON_CreateBool(true);
        switch (failure) {
        case 0: break;
        case 1: object = cJSON_GetObjectItem(bad, "runtime"); key = "version"; cJSON_Delete(replacement); replacement = cJSON_CreateString("0"); break;
        case 2: object = cJSON_GetObjectItem(bad, "model"); key = "file"; cJSON_Delete(replacement); replacement = cJSON_CreateString("../model.espdl"); break;
        case 3: object = cJSON_GetObjectItem(cJSON_GetObjectItem(bad, "inputs"), "a"); key = "shape"; cJSON_Delete(replacement); replacement = cJSON_CreateIntArray((int[]){1,8},2); break;
        case 4: object = cJSON_GetObjectItem(cJSON_GetObjectItem(bad, "inputs"), "a"); key = "dtype"; cJSON_Delete(replacement); replacement = cJSON_CreateString("uint8"); break;
        case 5: object = cJSON_GetObjectItem(cJSON_GetObjectItem(bad, "outputs"), "sum"); key = "exponents"; cJSON_Delete(replacement); replacement = cJSON_CreateIntArray((int[]){1},1); break;
        case 6: object = cJSON_GetObjectItem(bad, "result"); key = "type"; cJSON_Delete(replacement); replacement = cJSON_CreateString("custom"); break;
        case 7: object = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(bad, "inputs"), "a"), "adapter"); key = "type"; cJSON_Delete(replacement); replacement = cJSON_CreateString("custom"); break;
        case 8: key = "id"; cJSON_Delete(replacement); replacement = cJSON_CreateString(""); break;
        case 9: key = "unknown"; break;
        }
        assert(replacement);
        if (failure == 9) assert(cJSON_AddItemToObject(object,key,replacement));
        else assert(cJSON_ReplaceItemInObject(object,key,replacement));
        changed = cJSON_PrintUnformatted(bad); assert(changed);
        write_file(manifest,changed); cJSON_free(changed); cJSON_Delete(bad);
        assert(solar_os_inference_load_bundle(client,manifest,1000,&handle) != ESP_OK);
        assert(allocations == baseline && !workers);
    }
    const char *invalid_json[] = {"{} trailing", "{\"id\":\"a\nb\"}", "{\"id\":\"a\\u0000b\"}",
        "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]"};
    for (unsigned i = 0; i < sizeof(invalid_json)/sizeof(*invalid_json); ++i) {
        write_file(manifest,invalid_json[i]);
        assert(solar_os_inference_load_bundle(client,manifest,1000,&handle) != ESP_OK);
        assert(allocations == baseline && !workers);
    }
    const uint8_t pixels[] = {255,0,0,0,255,0,0,0,255,10,32,13,124,116,104,255,255,255};
    solar_os_raster_image_pixels_t image = {pixels, sizeof(pixels), 9, 3, 2};
    solar_os_inference_value_t input = {.tensor.name = "pixels", .image = &image};
    const char *image_inputs = "{\"pixels\":{\"dtype\":\"int8\",\"shape\":[1,2,3,3],\"exponents\":[0],"
        "\"adapter\":{\"type\":\"image\",\"options\":{\"layout\":\"NHWC\",\"color\":\"RGB\",\"resize\":\"stretch\"}}}}";
    const char *classes = "{\"scores\":{\"dtype\":\"int8\",\"shape\":[1,3],\"exponents\":[-2]}}";
    const char *classification = "{\"type\":\"classification\",\"options\":{\"tensor\":\"scores\",\"labels\":\"labels.txt\",\"activation\":\"softmax\",\"top\":3}}";
    profile = 1;
    snprintf(source, sizeof(source), format, model_sha, labels_sha, image_inputs, classes, classification); write_file(manifest, source);
    for (int failure = 0; failure < 300; ++failure) {
        failures = failure;
        esp_err_t error = solar_os_inference_load_bundle(client,manifest,1000,&handle); failures = -1;
        if (!error) assert(solar_os_inference_close(client,handle) == ESP_OK);
        assert(allocations == baseline && !workers);
    }
    assert(solar_os_inference_load_bundle(client, manifest, 1000, &handle) == ESP_OK);
    baseline = allocations;
    for (int failure = 0; failure < 60; ++failure) {
        failures = failure;
        esp_err_t error = solar_os_inference_run_bundle(client, handle, &input, 1, 1000, &r); failures = -1;
        if (!error) solar_os_inference_result_free(r);
        assert(allocations == baseline && !workers);
    }
    assert(solar_os_inference_run_bundle(client, handle, &input, 1, 1000, &r) == ESP_OK);
    cJSON *decoded = cJSON_Parse(r->result_json); assert(decoded);
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(decoded, "classes"), 0);
    assert(cJSON_GetObjectItem(first, "id")->valueint == 0 && !strcmp(cJSON_GetObjectItem(first, "label")->valuestring, "red"));
    assert(fabs(cJSON_GetObjectItem(first, "score")->valuedouble - exp(2) / (exp(2)+exp(1)+exp(-1))) < 1e-10);
    cJSON_Delete(decoded); solar_os_inference_result_free(r);
    input.image = NULL;
    assert(solar_os_inference_run_bundle(client, handle, &input, 1, 1000, &r) == ESP_ERR_INVALID_ARG); input.image = &image;
    assert(solar_os_inference_close(client, handle) == ESP_OK);
    profile = 2;
    const char *detector_inputs = "{\"pixels\":{\"dtype\":\"int8\",\"shape\":[1,2,2,3],\"exponents\":[0],"
        "\"adapter\":{\"type\":\"image\",\"options\":{\"layout\":\"NHWC\",\"color\":\"RGB\",\"resize\":\"stretch\"}}}}";
    const char *detector_outputs = "{\"score\":{\"dtype\":\"int8\",\"shape\":[1,1,1,1],\"exponents\":[-7]},"
        "\"bbox\":{\"dtype\":\"int8\",\"shape\":[1,1,1,8],\"exponents\":[0]}}";
    const char *detector = "{\"type\":\"pico-detection\",\"options\":{\"input\":\"pixels\",\"label\":\"person\","
        "\"stages\":[{\"score\":\"score\",\"bbox\":\"bbox\",\"stride\":2,\"bins\":2}]}}";
    snprintf(source, sizeof(source), format, model_sha, labels_sha, detector_inputs, detector_outputs, detector); write_file(manifest, source);
    assert(solar_os_inference_load_bundle(client, manifest, 1000, &handle) == ESP_OK);
    assert(solar_os_inference_run_bundle(client, handle, &input, 1, 1000, &r) == ESP_OK);
    decoded = cJSON_Parse(r->result_json); assert(decoded);
    first = cJSON_GetArrayItem(cJSON_GetObjectItem(decoded, "detections"), 0); assert(first);
    assert(fabs(cJSON_GetObjectItem(first, "score")->valuedouble - sqrt(100.0/128)) < 1e-10);
    const cJSON *box = cJSON_GetObjectItem(first, "box");
    assert(cJSON_GetArrayItem(box, 0)->valueint == 0 && cJSON_GetArrayItem(box, 2)->valueint == 2);
    cJSON_Delete(decoded); solar_os_inference_result_free(r);
    baseline = allocations;
    for (int failure = 0; failure < 80; ++failure) {
        failures = failure;
        esp_err_t error = solar_os_inference_run_bundle(client,handle,&input,1,1000,&r); failures = -1;
        if (!error) solar_os_inference_result_free(r);
        assert(allocations == baseline && !workers);
    }
    assert(solar_os_inference_close_all(client) == ESP_OK); solar_os_inference_destroy(client);
    assert(!allocations && !workers);
    unlink(model); unlink(manifest); unlink(labels); assert(!rmdir(directory));
    puts("native bundle hashes, contracts, OOM rollback, image preparation, raw/classification/PICO results passed");
    return 0;
}
