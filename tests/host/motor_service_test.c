#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_motor.h"
#include "solar_os_memory.h"

static unsigned allocations;
static const char *fail_tag;
size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size) { size_t n = len < size - 1U ? len : size - 1U; memcpy(dst, src, n); dst[n] = '\0'; }
    return len;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert((strcmp(tag, "motor.device") == 0 && cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED) ||
        (strcmp(tag, "motor.lock") == 0 && cls == SOLAR_OS_MEMORY_INTERNAL_CRITICAL));
    if (fail_tag && strcmp(fail_tag, tag) == 0) return NULL;
    void *p = calloc(n, size);
    if (p) allocations++;
    return p;
}
void solar_os_memory_free(void *p) { if (p) { assert(allocations); allocations--; free(p); } }

typedef struct {
    float value;
    unsigned drives, coasts, brakes, clears;
    bool fail_stop, fail_status, hold, entered;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} fake_t;

static esp_err_t status(void *ctx, solar_os_motor_status_t *value)
{
    fake_t *f = ctx;
    if (f->fail_status) return ESP_FAIL;
    *value = (solar_os_motor_status_t){.mode = f->value < 0 ? SOLAR_OS_MOTOR_MODE_REVERSE :
        f->value > 0 ? SOLAR_OS_MOTOR_MODE_FORWARD : SOLAR_OS_MOTOR_MODE_COAST,
        .drive_valid = true, .drive = f->value};
    return ESP_OK;
}
static esp_err_t drive(void *ctx, float value)
{
    fake_t *f = ctx;
    /* Metadata must remain callable while an operation executes. */
    assert(solar_os_motor_count() >= 1U);
    pthread_mutex_lock(&f->mutex);
    f->entered = true;
    pthread_cond_broadcast(&f->condition);
    while (f->hold) pthread_cond_wait(&f->condition, &f->mutex);
    f->value = value;
    f->drives++;
    pthread_mutex_unlock(&f->mutex);
    return ESP_OK;
}
static esp_err_t coast(void *ctx)
{
    fake_t *f = ctx;
    (void)solar_os_motor_count();
    if (f->fail_stop) return ESP_FAIL;
    f->coasts++; f->value = 0;
    return ESP_OK;
}
static esp_err_t brake(void *ctx) { ((fake_t *)ctx)->brakes++; return ESP_OK; }
static esp_err_t clear(void *ctx) { ((fake_t *)ctx)->clears++; return coast(ctx); }
static void *drive_thread(void *ctx)
{
    (void)ctx;
    assert(solar_os_motor_drive("left", -0.75f) == ESP_OK);
    return NULL;
}

int main(void)
{
    fake_t a = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    fake_t b = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const solar_os_motor_ops_t full = {.status = status, .drive = drive, .coast = coast, .brake = brake, .clear = clear};
    const solar_os_motor_ops_t basic = {.status = status, .drive = drive, .coast = coast};
    const solar_os_motor_registration_t left = {.name = "left", .driver = "voltage-backend", .ops = &full, .ctx = &a};
    const solar_os_motor_registration_t right = {.name = "right", .driver = "pwm-backend", .ops = &basic, .ctx = &b};
    assert(solar_os_motor_register(NULL) == ESP_ERR_INVALID_ARG);
    solar_os_motor_registration_t invalid = left;
    invalid.name = ""; assert(solar_os_motor_register(&invalid) == ESP_ERR_INVALID_ARG);
    invalid = left; invalid.driver = ""; assert(solar_os_motor_register(&invalid) == ESP_ERR_INVALID_ARG);
    for (unsigned i = 0; i < 2; i++) {
        fail_tag = i == 0 ? "motor.device" : "motor.lock";
        assert(solar_os_motor_register(&left) == ESP_ERR_NO_MEM && !allocations);
    }
    fail_tag = NULL;
    assert(solar_os_motor_register(&left) == ESP_OK);
    assert(solar_os_motor_register(&left) == ESP_ERR_INVALID_STATE);
    assert(solar_os_motor_register(&right) == ESP_OK && solar_os_motor_count() == 2);
    solar_os_motor_info_t info;
    assert(solar_os_motor_get(0, &info) && !strcmp(info.name, "left") && info.can_brake && info.can_clear);
    assert(solar_os_motor_get(1, &info) && !strcmp(info.driver, "pwm-backend") && !info.can_brake && !info.can_clear);
    assert(!solar_os_motor_get(2, &info));
    assert(solar_os_motor_drive("left", 0.25f) == ESP_OK && a.value == 0.25f);
    assert(solar_os_motor_drive("right", -1) == ESP_OK && b.value == -1);
    assert(solar_os_motor_drive("left", 1500) == ESP_ERR_INVALID_ARG);
    assert(solar_os_motor_drive("left", -1.01f) == ESP_ERR_INVALID_ARG);
    assert(solar_os_motor_drive("left", NAN) == ESP_ERR_INVALID_ARG);
    assert(solar_os_motor_drive("left", INFINITY) == ESP_ERR_INVALID_ARG && a.drives == 1);
    assert(solar_os_motor_drive("missing", 0.5f) == ESP_ERR_NOT_FOUND);
    assert(solar_os_motor_brake("right") == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_motor_clear("right") == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_motor_brake("left") == ESP_OK && a.brakes == 1);
    assert(solar_os_motor_clear("left") == ESP_OK && a.clears == 1 && a.value == 0);
    assert(solar_os_motor_drive("right", 0) == ESP_OK && b.value == 0);
    solar_os_motor_status_t value;
    assert(solar_os_motor_status("right", &value) == ESP_OK && !strcmp(value.info.driver, "pwm-backend"));
    assert(value.drive_valid && value.drive == 0 && !value.voltage_valid && !value.raw_valid);
    value.drive = 123;
    b.fail_status = true;
    assert(solar_os_motor_status("right", &value) == ESP_FAIL && value.drive == 123);
    b.fail_status = false;

    /* In-flight operations retain their provider; another provider can run
     * independently while the first is blocked. */
    a.hold = true; a.entered = false;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, drive_thread, NULL) == 0);
    pthread_mutex_lock(&a.mutex);
    while (!a.entered) pthread_cond_wait(&a.condition, &a.mutex);
    pthread_mutex_unlock(&a.mutex);
    assert(solar_os_motor_unregister("left") == ESP_ERR_INVALID_STATE);
    assert(solar_os_motor_drive("right", 0.5f) == ESP_OK && b.value == 0.5f);
    pthread_mutex_lock(&a.mutex); a.hold = false; pthread_cond_broadcast(&a.condition); pthread_mutex_unlock(&a.mutex);
    assert(pthread_join(thread, NULL) == 0 && a.value == -0.75f);
    a.fail_stop = true;
    assert(solar_os_motor_unregister("left") == ESP_FAIL && solar_os_motor_count() == 2);
    assert(solar_os_motor_status("left", &value) == ESP_OK);
    a.fail_stop = false;
    assert(solar_os_motor_unregister("left") == ESP_OK);
    assert(solar_os_motor_unregister("right") == ESP_OK && !allocations);
    assert(solar_os_motor_count() == 0 && !solar_os_motor_get(0, &info));
    assert(solar_os_motor_unregister("left") == ESP_ERR_NOT_FOUND);
    const solar_os_motor_ops_t coast_only = {.status = status, .coast = coast};
    const solar_os_motor_registration_t passive = {.name = "passive", .driver = "coast-backend", .ops = &coast_only, .ctx = &a};
    assert(solar_os_motor_register(&passive) == ESP_OK);
    assert(solar_os_motor_get(0, &info) && !info.can_drive && !info.can_brake && !info.can_clear);
    assert(solar_os_motor_drive("passive", 0.5f) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_motor_drive("passive", 0.0f) == ESP_OK);
    assert(solar_os_motor_unregister("passive") == ESP_OK && !allocations);
    assert(!strcmp(solar_os_motor_mode_name(SOLAR_OS_MOTOR_MODE_REVERSE), "reverse"));
    puts("motor service: multiple backends, normalized range, capabilities, lifetime and failure recovery PASS");
    return 0;
}
