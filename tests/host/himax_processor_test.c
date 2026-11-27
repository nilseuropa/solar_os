/* Reuse the pipeline suite's failure-counted allocator, job and task fixtures. */
#define main pipeline_baseline_main
#include "pipeline_test.c"
#undef main
#include "solar_os_native.h"
#include "solar_os_native_application_lifecycle_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_media.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_native_sscma_abi.h"
#include "solar_os_processor_provider_abi.h"
#include "himax_hx6538.h"
#include <openssl/evp.h>

const solar_os_processor_provider_api_v1_t *solar_os_himax_processor_api(void);
static unsigned provider_leases, hardware_owners, acquired, invoked;
static bool missing_library, wrong_abi, fail_invoke, corrupt_image, block_invoke, corrupt_result, big_result;
static unsigned close_refusals;
static unsigned selected_slot = 1;
static bool provider_owns_camera;
static atomic_bool invoke_entered;
static solar_os_processor_provider_api_v1_t provider_table;
static const uint8_t jpeg[] = {0xff,0xd8,0xff,0xc0,0x00,0x08,0x08,0x00,0x78,0x00,0xa0,0x01,0xff,0xd9};

int mbedtls_base64_decode(unsigned char *dst, size_t capacity, size_t *length,
    const unsigned char *source, size_t size)
{
    if (size % 4 || capacity < size / 4 * 3) return -1;
    unsigned char *copy = malloc(size); assert(copy); memcpy(copy, source, size);
    int decoded = EVP_DecodeBlock(dst, copy, (int)size);
    if (decoded >= 0 && size && copy[size - 1] == '=') decoded--;
    if (decoded >= 0 && size > 1 && copy[size - 2] == '=') decoded--;
    free(copy);
    if (decoded < 0) return -1;
    *length = (size_t)decoded; return 0;
}
static int camera_open(const char *source, const char *owner, uint32_t *token)
{
    assert(owner && *owner); *token = 0;
    if (strcmp(source, "camera0")) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (hardware_owners) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    ++hardware_owners; provider_owns_camera = true; *token = 77; return 0;
}
static int camera_start(uint32_t token) { assert(token == 77 && hardware_owners); return 0; }
static int camera_close(uint32_t token)
{
    assert(token == 77 && hardware_owners == 1);
    if (close_refusals) { --close_refusals; return SOLAR_OS_NATIVE_ERROR_INVALID_STATE; }
    --hardware_owners; provider_owns_camera = false; return 0;
}
static int camera_exchange(uint32_t token, const char *source, const char *command,
    const char *name, bool event, uint32_t timeout, solar_os_native_cancel_fn cancel, void *user, char **reply)
{
    assert(token == 77 && hardware_owners == 1 && !strcmp(source, "camera0") && timeout);
    *reply = NULL;
    char response[512];
    if (event) {
        assert(!strcmp(name, "INVOKE") && !strcmp(command, "AT+INVOKE=1,0,0\r\n"));
        ++invoked; atomic_store(&invoke_entered, true);
        while (block_invoke && !(cancel && cancel(user))) vTaskDelay(1);
        if (cancel && cancel(user)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
        if (fail_invoke) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        if (big_result) {
            const size_t count=35000;
            *reply=solar_os_memory_alloc(count*2+64,0,"test.large");
            if(!*reply)return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
            char *p=*reply;const char *prefix="{\"data\":{\"boxes\":[";
            memcpy(p,prefix,strlen(prefix));p+=strlen(prefix);
            for(size_t i=0;i<count;i++){*p++='0';*p++=',';}
            strcpy(p,"0]}}");return 0;
        }
        unsigned char encoded[64]; assert(EVP_EncodeBlock(encoded, jpeg, sizeof(jpeg)) > 0);
        snprintf(response, sizeof(response), "{\"data\":{\"count\":%u,\"perf\":[2,3,4],"
            "\"boxes\":%s,\"classes\":[],\"points\":[],\"image\":\"%s\"}}",
            invoked, corrupt_result ? "[garbage]" : "[[80,60,20,10,90,1]]",
            corrupt_image ? "bad!" : (char *)encoded);
    } else if (!strcmp(name, "MODELS?"))
        strcpy(response, "{\"data\":[{\"id\":1,\"type\":0,\"size\":0}]}");
    else if (!strcmp(name, "MODEL?")) snprintf(response,sizeof(response),"{\"data\":{\"id\":%u,\"type\":0,\"size\":0}}",selected_slot);
    else if (!strcmp(name, "MODEL")) { assert(sscanf(command,"AT+MODEL=%u",&selected_slot)==1); strcpy(response,"{\"data\":{}}"); }
    else if (!strcmp(name, "ALGOS?")) strcpy(response, "{\"data\":[{\"type\":1,\"input_from\":1}]}");
    else { assert(!strcmp(name, "VER?")); strcpy(response, "{\"data\":{\"software\":\"2024.08.16\"}}"); }
    *reply = solar_os_memory_alloc(strlen(response) + 1, 0, "test.sscma");
    if (!*reply) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(*reply, response); return 0;
}
static int camera_image(char *reply, size_t capacity, const uint8_t **data,
    size_t *length, uint16_t *width, uint16_t *height)
{
    return solar_os_himax_decode_image(reply, capacity, data, length, width, height) ?
        SOLAR_OS_NATIVE_ERROR_FAILED : 0;
}
static const solar_os_native_sscma_api_v1_t camera_api = {1,sizeof(camera_api),
    camera_open,camera_start,camera_exchange,camera_image,camera_close,NULL,NULL,NULL};
static void *memory_alloc(size_t bytes) { return solar_os_memory_alloc(bytes,0,"test.provider"); }
static const solar_os_native_memory_api_v1_t memory_api = {
    .abi_version=1,.struct_size=sizeof(memory_api),.alloc=memory_alloc,.free=solar_os_memory_free};
static const solar_os_native_libc_api_v1_t libc_api = {.abi_version=1,.struct_size=sizeof(libc_api),.format=vsnprintf};
static int64_t sample_time(void) { return 123456; }
static void sleep_ms(uint32_t ms) { vTaskDelay(ms); }
static const solar_os_native_time_api_v1_t time_api = {1,sizeof(time_api),sample_time,sleep_ms};
static const void *get_service(const char *name,uint32_t abi,uint32_t size)
{
    assert(abi==1);
    if (!strcmp(name,"camera.sscma")) { assert(size==SOLAR_OS_NATIVE_SSCMA_BASE_SIZE);return &camera_api; }
    if (!strcmp(name,"memory")) { assert(size==sizeof(memory_api));return &memory_api; }
    if (!strcmp(name,"libc")) { assert(size==sizeof(libc_api));return &libc_api; }
    if (!strcmp(name,"time")) { assert(size==sizeof(time_api));return &time_api; }
    assert(0);return NULL;
}
const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void)
{
    static const solar_os_native_library_host_api_v1_t host={.abi_version=1,.struct_size=sizeof(host),.get_service=get_service};
    return &host;
}
int solar_os_native_library_acquire(const char *id,uint32_t abi,uint32_t size,uint32_t *lease,const void **api)
{
    assert(abi==1 && size==SOLAR_OS_PROCESSOR_PROVIDER_BASE_SIZE);*lease=0;*api=NULL;
    if (strcmp(id,"processor-himax") || missing_library) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    assert(!provider_leases);provider_leases++;acquired++;*lease=1;
    provider_table=*solar_os_himax_processor_api();
    if(wrong_abi) provider_table.abi_version=2;
    *api=&provider_table;return 0;
}
int solar_os_native_library_release(uint32_t lease)
{ assert(lease==1 && provider_leases==1 && !provider_owns_camera);--provider_leases;return 0; }
static void clean(void)
{ assert(!allocations && !workers && !hardware_owners && !provider_leases && !sources && !leases); }

const solar_os_application_lifecycle_api_v1_t *solar_os_application_descriptor(void) { return NULL; }

typedef struct {
    const solar_os_native_processor_api_v1_t *api;
    solar_os_native_handle_t processor, image;
    char *json;
    int result;
} capture_request_t;

static void *capture_request(void *opaque)
{
    capture_request_t *request = opaque;
    request->result = request->api->capture(request->processor, "camera0", 9, &request->image, &request->json);
    return NULL;
}

static int check_native_sample(const solar_os_native_media_sample_view_v1_t *sample, void *user)
{
    const uint64_t sequence = user ? *(const uint64_t *)user : 9;
    assert(sample->kind == SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE && sample->metadata.sequence == sequence);
    assert(sample->metadata.width == 160 && sample->metadata.height == 120 && sample->metadata.timestamp_us == 123456);
    assert(!strcmp(sample->metadata.source, "camera0") && sample->length == sizeof(jpeg));
    assert(!memcmp(sample->data, jpeg, sizeof(jpeg)));
    return SOLAR_OS_NATIVE_OK;
}

static void test_native_processor(void)
{
    const solar_os_native_processor_api_v1_t *api = solar_os_native_media_get_service(
        SOLAR_OS_NATIVE_PROCESSOR_SERVICE, SOLAR_OS_NATIVE_PROCESSOR_ABI, sizeof(*api));
    const solar_os_native_media_sample_api_v1_t *samples = solar_os_native_media_get_service(
        SOLAR_OS_NATIVE_MEDIA_SAMPLE_SERVICE, SOLAR_OS_NATIVE_MEDIA_SAMPLE_ABI, sizeof(*samples));
    assert(api && samples);
    assert(api->kind_valid("himax") && !api->kind_valid("../himax"));
    char *json = NULL;
    assert(!api->query("himax", "camera0", "info", &json) && strstr(json, "models"));
    solar_os_memory_free(json);
    clean();

    capture_request_t request = {.api = api};
    assert(!api->create("himax", 0, 1000, &request.processor) && api->captures(request.processor));
    assert(!api->bind_source(request.processor, "camera0"));
    atomic_store(&invoke_entered, false);
    block_invoke = true;
    pthread_t capture_worker;
    assert(!pthread_create(&capture_worker, NULL, capture_request, &request));
    while (!atomic_load(&invoke_entered)) vTaskDelay(1);
    assert(api->close(request.processor) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(api->bind_source(request.processor, "camera0") == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!api->cancel(request.processor));
    assert(!pthread_join(capture_worker, NULL));
    block_invoke = false;
    assert(request.result == SOLAR_OS_NATIVE_ERROR_TIMEOUT && !request.image && !request.json);
    assert(api->cancel(request.processor) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    capture_request(&request);
    assert(!request.result && request.image && strstr(request.json, "boxes"));
    solar_os_memory_free(request.json);

    close_refusals = 1;
    assert(api->close(request.processor) == SOLAR_OS_NATIVE_ERROR_BUSY && hardware_owners == 1);
    assert(api->bind_source(request.processor, "camera0") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->close(request.processor) && !hardware_owners && !provider_leases);
    assert(!samples->visit(request.image, check_native_sample, NULL));
    assert(!samples->close(request.image) && !solar_os_native_handle_count());
    clean();
}

static void test_native_pipeline(void)
{
    const solar_os_native_pipeline_api_v1_t *api = solar_os_native_media_get_service(
        SOLAR_OS_NATIVE_PIPELINE_SERVICE, SOLAR_OS_NATIVE_PIPELINE_ABI, sizeof(*api));
    const solar_os_native_media_sample_api_v1_t *samples = solar_os_native_media_get_service(
        SOLAR_OS_NATIVE_MEDIA_SAMPLE_SERVICE, SOLAR_OS_NATIVE_MEDIA_SAMPLE_ABI, sizeof(*samples));
    assert(api && samples);
    const solar_os_pipeline_config_t config = {.source = "camera0", .processor = "himax", .timeout_ms = 1000, .limit = 1};
    uint32_t id, listed = 0;
    assert(!api->start(&config, &id) && id);
    size_t count = 0;
    assert(api->list(NULL, 0, &count) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && count == 1);
    assert(!api->list(&listed, 1, &count) && count == 1 && listed == id);
    finish(id);
    solar_os_pipeline_status_t status;
    assert(!api->status(id, &status) && status.done && !status.last_error && status.sequence == 1);
    char *json = NULL;
    solar_os_native_handle_t sample = 0;
    assert(!api->result(id, status.sequence, &json, &sample) && !json && !sample);
    failures = 0;
    assert(api->result(id, 0, &json, &sample) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !json && !sample);
    failures = -1;
    assert(!api->result(id, 0, &json, &sample) && sample && strstr(json, "\"sequence\":1"));
    solar_os_memory_free(json);
    assert(!api->stop(id) && !api->destroy(id));
    assert(!samples->visit(sample, check_native_sample, &status.sequence));
    assert(!samples->close(sample));
    assert(api->status(id, &status) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !status.id);
    clean();

    fail_invoke = true;
    assert(!api->start(&config, &id));
    finish(id);
    assert(!api->status(id, &status) && status.last_error == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!api->destroy(id));
    fail_invoke = false;
    clean();
}

int main(void)
{
    pipeline_baseline_main();clean();
    test_native_processor();
    test_native_pipeline();
    char *json=NULL;
    assert(!solar_os_processor_query("himax","camera0","info",&json));
    cJSON *info=cJSON_Parse(json);assert(info);
    assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(info,"models")));
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(info,"external_frames")));
    cJSON_Delete(info);solar_os_memory_free(json);clean();
    assert(solar_os_processor_query("himax","rtsp://other","info",&json)==ESP_ERR_NOT_FOUND);clean();
    assert(solar_os_processor_query("himax","camera0","invalid",&json)==ESP_ERR_INVALID_ARG);clean();
    assert(solar_os_processor_query("../himax","camera0","info",&json)==ESP_ERR_INVALID_ARG);clean();
#if !SOLAR_OS_PACKAGE_BACKEND_PROCESSOR_HIMAX
    wrong_abi=true;
    assert(solar_os_processor_query("himax","camera0","info",&json)==ESP_ERR_NOT_SUPPORTED);clean();wrong_abi=false;
    missing_library=true;
    assert(solar_os_processor_query("himax","camera0","info",&json)==ESP_ERR_NOT_FOUND);clean();missing_library=false;
#endif
    solar_os_pipeline_config_t c={.source="camera0",.processor="himax",.timeout_ms=1000,.limit=2};
    uint32_t id;
    assert(!solar_os_pipeline_start(&c,&id));
    solar_os_pipeline_status_t s=finish(id);assert(s.frames==2 && !s.last_error && !s.decode_us);
    solar_os_media_sample_t *sample=NULL;
    assert(!solar_os_pipeline_result_sample(id,0,&json,&sample) && sample && json);
    const solar_os_media_sample_image_t *image=solar_os_media_sample_image(sample);
    assert(image->length==sizeof(jpeg) && !memcmp(image->data,jpeg,sizeof(jpeg)));
    assert(image->metadata.sequence==s.sequence && image->metadata.timestamp_us==123456);
    assert(image->metadata.width==160 && image->metadata.height==120 && !strcmp(image->metadata.source,"camera0"));
    assert(strstr(json,"boxes") && !strstr(json,"image\""));
    cJSON *record=cJSON_Parse(json);assert(record);
    assert(cJSON_GetObjectItemCaseSensitive(record,"sequence")->valueint==(int)image->metadata.sequence);
    cJSON_Delete(record);
    solar_os_memory_free(json);assert(!solar_os_pipeline_destroy(id));
    assert(!memcmp(image->data,jpeg,sizeof(jpeg)));solar_os_media_sample_release(sample);clean();
    fail_invoke=true;
    assert(!solar_os_pipeline_start(&c,&id));s=finish(id);
    assert(!s.frames && s.last_error==ESP_ERR_NOT_SUPPORTED);
    assert(!solar_os_pipeline_destroy(id));clean();fail_invoke=false;
    corrupt_image=true;
    assert(!solar_os_pipeline_start(&c,&id));s=finish(id);assert(!s.frames && s.last_error);
    assert(!solar_os_pipeline_destroy(id));clean();corrupt_image=false;
    corrupt_result=true;
    assert(!solar_os_pipeline_start(&c,&id));s=finish(id);assert(!s.frames && s.last_error==ESP_ERR_INVALID_RESPONSE);
    assert(!solar_os_pipeline_destroy(id));clean();corrupt_result=false;
    big_result=true;
    assert(!solar_os_pipeline_start(&c,&id));s=finish(id);assert(!s.frames && s.last_error==ESP_ERR_INVALID_SIZE);
    assert(!solar_os_pipeline_destroy(id));clean();big_result=false;
    c.model=2;assert(!solar_os_pipeline_start(&c,&id));s=finish(id);assert(s.frames==2 && !s.last_error);
    assert(!solar_os_pipeline_result(id,0,&json) && strstr(json,"\"model\":2"));solar_os_memory_free(json);
    assert(!solar_os_pipeline_destroy(id));clean();c.model=5;
    assert(solar_os_pipeline_start(&c,&id)==ESP_ERR_INVALID_ARG);clean();c.model=0;selected_slot=1;
    close_refusals=2;
    assert(!solar_os_pipeline_start(&c,&id));finish(id);assert(!solar_os_pipeline_destroy(id));clean();
    deny_worker=true;assert(solar_os_pipeline_start(&c,&id)==ESP_ERR_NO_MEM);clean();deny_worker=false;
    hardware_owners=1;
    assert(solar_os_pipeline_start(&c,&id)==ESP_ERR_INVALID_STATE);
    assert(hardware_owners==1 && !provider_leases);hardware_owners=0;clean();
    atomic_store(&invoke_entered,false);block_invoke=true;
    assert(!solar_os_pipeline_start(&c,&id));
    while(!atomic_load(&invoke_entered))vTaskDelay(1);
    assert(!solar_os_pipeline_stop(id));assert(!solar_os_pipeline_destroy(id));clean();block_invoke=false;
    for(int i=0;i<40;i++) {
        failures=i;esp_err_t error=solar_os_pipeline_start(&c,&id);
        if(!error) { finish(id); assert(!solar_os_pipeline_destroy(id)); }
        failures=-1;clean();
    }
    for(int i=0;i<35;i++) {
        failures=i;(void)solar_os_processor_query("himax","camera0","info",&json);
        solar_os_memory_free(json);failures=-1;clean();
    }
    assert(invoked);
#if !SOLAR_OS_PACKAGE_BACKEND_PROCESSOR_HIMAX
    assert(acquired);
#endif
    puts("Himax provider discovery, matching samples, leases, cancellation and OOM passed");
}
