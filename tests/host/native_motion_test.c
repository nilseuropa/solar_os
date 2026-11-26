#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_native_client.h"
#include "solar_os_native_motion.h"
#include "solar_os_encoder.h"
#include "solar_os_motor.h"
#include "solar_os_memory.h"

static float drive;
static int32_t count;
static bool fail_coast;
static bool fail_read;
static uint64_t clock_us = 1000;
static unsigned allocations;

uint64_t solar_os_time_uptime_ms(void) { return clock_us / 1000U; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_us; }
void vTaskDelay(uint32_t ticks) { clock_us += 1000U * ticks; count += 4; }

size_t strlcpy(char *dest, const char *src, size_t capacity)
{
    size_t len = strlen(src);
    if (capacity) {
        size_t copied = len < capacity - 1 ? len : capacity - 1;
        memcpy(dest, src, copied);
        dest[copied] = 0;
    }
    return len;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)cls;
    (void)tag;
    void *p = calloc(n, size);
    if (p) ++allocations;
    return p;
}
void solar_os_memory_free(void *p) { if (p) { --allocations; free(p); } }

static esp_err_t motor_status(void *ctx, solar_os_motor_status_t *out)
{
    (void)ctx;
    *out = (solar_os_motor_status_t){.mode = drive ? SOLAR_OS_MOTOR_MODE_REVERSE : SOLAR_OS_MOTOR_MODE_COAST,
        .drive = drive, .drive_valid = true};
    return ESP_OK;
}
static esp_err_t motor_drive(void *ctx, float value) { (void)ctx; drive = value; return ESP_OK; }
static esp_err_t motor_coast(void *ctx) { return fail_coast ? ESP_FAIL : motor_drive(ctx, 0); }

static esp_err_t encoder_read(void *ctx, int32_t *out)
{
    (void)ctx;
    if (fail_read) return ESP_FAIL;
    *out = count;
    return ESP_OK;
}
static esp_err_t encoder_reset(void *ctx) { (void)ctx; count = 0; return ESP_OK; }

int main(void)
{
    const solar_os_native_motion_api_v1_t *motion = solar_os_native_motion_get_service("motion", 1,
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_motion_api_v1_t, servo_close));
    assert(motion);
#if SOLAR_OS_PACKAGE_SERVICE_MOTOR
    assert(motion->capabilities == (SOLAR_OS_NATIVE_MOTION_MOTOR | SOLAR_OS_NATIVE_MOTION_ENCODER));
    assert(solar_os_native_motion_has_motor_command(motion) && solar_os_native_motion_has_encoder_feedback(motion));
    const solar_os_motor_ops_t motor_ops = {.status = motor_status, .drive = motor_drive, .coast = motor_coast};
    const solar_os_motor_registration_t motor = {.name = "wheel", .driver = "test", .ops = &motor_ops};
    assert(!solar_os_motor_register(&motor));
    solar_os_native_motor_info_v1_t motor_info;
    assert(motion->motor_count() == 1 && motion->motor_info(0, &motor_info) && motor_info.can_drive);
    assert(!motion->motor_info(1, &motor_info) && !strcmp(motor_info.name, "wheel"));
    assert(!motion->motor_configure("wheel", "polarity", -1));
    assert(!motion->motor_command("wheel", SOLAR_OS_NATIVE_MOTOR_COMMAND_DRIVE, .5f));
    solar_os_native_motor_status_v1_t status;
    assert(!motion->motor_status("wheel", &status) && status.drive == -.5f && status.config.polarity == -1);
    assert(!strcmp(motion->motor_mode_name(status.mode), "reverse"));
    assert(motion->motor_command("wheel", SOLAR_OS_NATIVE_MOTOR_COMMAND_BRAKE, 0) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(motion->motor_command("wheel", 99, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    uint32_t claim;
    assert(!solar_os_motor_claim("wheel", &claim));
    assert(motion->motor_command("wheel", SOLAR_OS_NATIVE_MOTOR_COMMAND_COAST, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!motion->motor_get_config("wheel", &status.config));
    status.config.max_drive = .25f;
    assert(motion->motor_config_valid(&status.config) && !motion->motor_configure_owned(claim, &status.config));
    assert(motion->motor_drive(claim, .5f) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!motion->motor_drive(claim, .25f) && drive == -.25f);
    fail_coast = true;
    assert(motion->motor_release(claim, false) == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(motion->motor_command("wheel", SOLAR_OS_NATIVE_MOTOR_COMMAND_COAST, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    fail_coast = false;
    assert(!motion->motor_release(claim, false) && drive == 0);
    assert(!solar_os_motor_unregister("wheel"));

    const solar_os_encoder_ops_t encoder_ops = {.read = encoder_read, .reset = encoder_reset};
    const solar_os_encoder_registration_t encoder = {.name = "shaft", .driver = "test",
        .mode = SOLAR_OS_ENCODER_MODE_X4, .ops = &encoder_ops};
    assert(!solar_os_encoder_register(&encoder));
    solar_os_native_encoder_info_v1_t encoder_info;
    assert(motion->encoder_count() == 1 && motion->encoder_info(0, &encoder_info) && encoder_info.can_reset);
    assert(!motion->encoder_configure("shaft", "ppr", 32));
    assert(!motion->encoder_claim("shaft", &claim));
    solar_os_native_encoder_config_v1_t config;
    assert(!motion->encoder_get_config("shaft", &config) && motion->encoder_config_valid(&config));
    config.position_zero = 1;
    assert(!motion->encoder_configure_owned(claim, &config));
    solar_os_native_encoder_feedback_v1_t feedback;
    assert(!motion->encoder_feedback(claim, &feedback) && !feedback.valid && feedback.position == 1);
    count = -12;
    int32_t value;
    assert(!motion->encoder_read("shaft", &value) && value == -12 && clock_us == 1000);
    assert(motion->encoder_reset("shaft") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_native_encoder_sample_v1_t sample;
    assert(!motion->encoder_sample(claim, &sample) && sample.count == -12 && sample.decoding == 4);
    fail_read = true;
    assert(motion->encoder_sample(claim, &sample) == SOLAR_OS_NATIVE_ERROR_FAILED && sample.count == -12);
    fail_read = false;
    assert(!solar_os_encoder_release(claim));
    solar_os_native_encoder_status_v1_t encoder_status;
    assert(!motion->encoder_status("shaft", &encoder_status) && encoder_status.feedback.valid && clock_us == 21000);
    assert(!motion->encoder_reset("shaft") && count == 0);
    assert(!solar_os_encoder_unregister("shaft") && !allocations);
#else
    assert(!motion->capabilities && !solar_os_native_motion_has_motor_command(motion));
    assert(!solar_os_native_motion_has_encoder_feedback(motion));
    (void)motor_status; (void)motor_coast; (void)encoder_read; (void)encoder_reset;
#endif
    return 0;
}
