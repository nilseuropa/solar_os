#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_encoder.h"
#include "solar_os_memory.h"
#include "solar_os_stream.h"
#include <math.h>
static int64_t sample_time_us = 1000;
static int32_t *moving_count;
static int32_t movement;
int64_t esp_timer_get_time(void) { return sample_time_us; }
void vTaskDelay(uint32_t ticks) {
    sample_time_us+=(int64_t)ticks*1000;
    if(moving_count) *moving_count=(int32_t)((uint32_t)*moving_count+(uint32_t)movement);
}

static unsigned allocations;
static const char *fail_tag;
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(dst, src, n); dst[n] = '\0'; }
    return len;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert((cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED &&
        (!strcmp(tag, "encoder.device") || !strcmp(tag, "encoder.pcnt"))) ||
        (cls == SOLAR_OS_MEMORY_INTERNAL_CRITICAL && !strcmp(tag, "encoder.lock")));
    if (fail_tag && !strcmp(tag, fail_tag)) return NULL;
    void *p = calloc(n, size);
    if (p) allocations++;
    return p;
}
void solar_os_memory_free(void *p) { if (p) { assert(allocations); allocations--; free(p); } }

typedef struct {
    int32_t value;
    bool fail_read, fail_shutdown, hold, entered;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} fake_t;

static esp_err_t read_count(void *ctx, int32_t *value)
{
    fake_t *f = ctx;
    assert(solar_os_encoder_count() >= 1U);
    pthread_mutex_lock(&f->mutex);
    f->entered = true;
    pthread_cond_broadcast(&f->condition);
    while (f->hold) pthread_cond_wait(&f->condition, &f->mutex);
    *value = f->value;
    pthread_mutex_unlock(&f->mutex);
    return f->fail_read ? ESP_FAIL : ESP_OK;
}
static esp_err_t reset_count(void *ctx) { ((fake_t *)ctx)->value = 0; return ESP_OK; }
static esp_err_t shutdown_count(void *ctx)
{
    (void)solar_os_encoder_count();
    return ((fake_t *)ctx)->fail_shutdown ? ESP_FAIL : ESP_OK;
}
static void *read_thread(void *ctx)
{
    (void)ctx;
    int32_t value;
    assert(solar_os_encoder_read("left", &value) == ESP_OK && value == -42);
    return NULL;
}

int main(void)
{
    fake_t a = {.value = -42, .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    fake_t b = {.value = INT32_MAX, .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const solar_os_encoder_ops_t full = {.read = read_count, .reset = reset_count, .shutdown = shutdown_count};
    const solar_os_encoder_ops_t basic = {.read = read_count};
    const solar_os_encoder_registration_t one = {.name = "left", .driver = "pcnt", .mode = SOLAR_OS_ENCODER_MODE_X4, .ops = &full, .ctx = &a};
    const solar_os_encoder_registration_t two = {.name = "right", .driver = "external", .mode = SOLAR_OS_ENCODER_MODE_X2, .ops = &basic, .ctx = &b};
    assert(solar_os_encoder_register(NULL) == ESP_ERR_INVALID_ARG);
    solar_os_encoder_registration_t invalid = one;
    invalid.mode = 3; assert(solar_os_encoder_register(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = one; invalid.name = ""; assert(solar_os_encoder_register(&invalid) == ESP_ERR_INVALID_ARG);
    for (unsigned i = 0; i < 2; i++) {
        fail_tag = i == 0 ? "encoder.device" : "encoder.lock";
        assert(solar_os_encoder_register(&one) == ESP_ERR_NO_MEM && !allocations);
    }
    fail_tag = NULL;
    assert(solar_os_encoder_register(&one) == ESP_OK);
    assert(solar_os_encoder_register(&one) == ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_register(&two) == ESP_OK && solar_os_encoder_count() == 2);
    solar_os_encoder_info_t info;
    assert(solar_os_encoder_get(0, &info) && !strcmp(info.name, "left") && info.mode == 4 && info.can_reset);
    assert(solar_os_encoder_get(1, &info) && !strcmp(info.driver, "external") && !info.can_reset);
    assert(!solar_os_encoder_get(2, &info) && !solar_os_encoder_get(0, NULL));
    solar_os_encoder_status_t s;
    assert(solar_os_encoder_status("left", &s) == ESP_OK && s.count == -42 && s.info.mode == 4);
    int32_t value = 123;
    assert(solar_os_encoder_read("right", &value) == ESP_OK && value == INT32_MAX);
    b.value = INT32_MIN;
    assert(solar_os_encoder_read("right", &value) == ESP_OK && value == INT32_MIN);
    b.fail_read = true; value = 123; s.count = 456;
    assert(solar_os_encoder_read("right", &value) == ESP_FAIL && value == 123);
    assert(solar_os_encoder_status("right", &s) == ESP_FAIL && s.count == 456);
    b.fail_read = false;
    assert(solar_os_encoder_reset("right") == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_encoder_read("missing", &value) == ESP_ERR_NOT_FOUND);
    assert(solar_os_encoder_read(NULL, &value) == ESP_ERR_INVALID_ARG);
    assert(solar_os_encoder_read("left", NULL) == ESP_ERR_INVALID_ARG);
    a.hold = true; a.entered = false;
    pthread_t thread; assert(pthread_create(&thread, NULL, read_thread, NULL) == 0);
    pthread_mutex_lock(&a.mutex);
    while (!a.entered) pthread_cond_wait(&a.condition, &a.mutex);
    pthread_mutex_unlock(&a.mutex);
    assert(solar_os_encoder_unregister("left") == ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_read("right", &value) == ESP_OK);
    pthread_mutex_lock(&a.mutex); a.hold = false; pthread_cond_broadcast(&a.condition); pthread_mutex_unlock(&a.mutex);
    assert(pthread_join(thread, NULL) == 0);
    assert(solar_os_encoder_reset("left") == ESP_OK && a.value == 0);
    uint32_t claim, other;
    assert(solar_os_encoder_claim("left", &claim) == ESP_OK && claim);
    assert(solar_os_encoder_claim("left", &other) == ESP_ERR_INVALID_STATE && other == 0);
    assert(solar_os_encoder_reset("left") == ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_unregister("left") == ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_read("left", &value) == ESP_OK);
    solar_os_encoder_sample_t sample;
    assert(solar_os_encoder_sample_owned(claim, &sample) == ESP_OK);
    assert(sample.count == 0 && sample.decoding == 4 && sample.timestamp_us == (uint64_t)sample_time_us);
    assert(solar_os_encoder_release(claim) == ESP_OK);
    assert(solar_os_encoder_sample_owned(claim, &sample) == ESP_ERR_NOT_FOUND);
    assert(solar_os_encoder_claim("left", &other) == ESP_OK && other != claim);
    assert(solar_os_encoder_release(claim) == ESP_ERR_NOT_FOUND);
    assert(solar_os_encoder_release(other) == ESP_OK);
    /* A single idle status query measures current motion, without a stream
     * or a previous query. Raw reads remain immediate. */
    assert(solar_os_encoder_configure("left","ppr",70)==ESP_OK);
    assert(solar_os_encoder_configure("left","gear_ratio",30)==ESP_OK);
    moving_count=&a.value;movement=28; /* x4: 8400 counts/revolution. */
    int64_t before_time=sample_time_us;
    assert(solar_os_encoder_read("left",&value)==ESP_OK && sample_time_us==before_time);
    assert(solar_os_encoder_status("left",&s)==ESP_OK && !s.claimed && s.feedback.valid);
    assert(s.count==28 && s.feedback.count==28 && s.feedback.timestamp_us==(uint64_t)sample_time_us);
    assert(fabsf(s.feedback.velocity-1.04719755f)<.00001f && s.feedback.position>0);
    movement=0;
    assert(solar_os_encoder_status("left",&s)==ESP_OK && s.feedback.valid && s.feedback.velocity==0);
    /* Observation does not acquire another handle or mutate the owner's
     * filter, wrap accumulator, baseline or acquisition time. */
    solar_os_stream_handle_t handle=SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open("enc.left","controller",&handle)==ESP_OK);
    solar_os_stream_scalar_sample_t observed;
    const solar_os_stream_read_options_t window={.window_ms=10};
    assert(solar_os_stream_read_scalar_sample(&handle,&window,&observed)==ESP_ERR_TIMEOUT);
    sample_time_us+=20000;a.value+=28;
    assert(solar_os_stream_read_scalar_sample(&handle,NULL,&observed)==ESP_OK);
    solar_os_native_encoder_feedback_v1_t previous,after;
    assert(solar_os_encoder_feedback((uint32_t)handle.private_data[0],&previous)==ESP_OK);
    sample_time_us+=10000;a.value+=14;
    assert(solar_os_encoder_status("left",&s)==ESP_OK && s.feedback.valid && s.count==70);
    assert(solar_os_stream_observe_scalar_sample("enc.left",NULL,&observed)==ESP_OK);
    assert(observed.value==previous.velocity && observed.uptime_ms==previous.timestamp_us/1000);
    assert(solar_os_encoder_feedback((uint32_t)handle.private_data[0],&after)==ESP_OK);
    assert(!memcmp(&previous,&after,sizeof(previous)));
    solar_os_stream_info_t stream_info;
    assert(solar_os_stream_get_info("enc.left",&stream_info)==ESP_OK && stream_info.active_handles==1 && !strcmp(stream_info.owner,"controller"));
    assert(solar_os_encoder_reset("left")==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_close_ex(&handle)==ESP_OK);
    assert(solar_os_stream_observe_scalar_sample("enc.left",NULL,&observed)==ESP_ERR_NOT_SUPPORTED);
    moving_count=NULL;
    a.fail_shutdown = true;
    assert(solar_os_encoder_unregister("left") == ESP_FAIL && solar_os_encoder_count() == 2);
    assert(solar_os_encoder_read("left", &value) == ESP_OK && value == 70);
    a.fail_shutdown = false;
    assert(solar_os_encoder_unregister("left") == ESP_OK);
    assert(solar_os_encoder_unregister("right") == ESP_OK && !allocations && !solar_os_encoder_count());
    assert(solar_os_encoder_unregister("left") == ESP_ERR_NOT_FOUND);
    puts("encoder service: providers, signed counts, concurrency, errors and lifecycle PASS");
    return 0;
}
