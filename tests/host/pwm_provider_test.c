#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "solar_os_pwm.h"
#include "solar_os_servo.h"
#define RAD(x) ((x) * (3.14159265358979323846f / 180.0f))
#include "solar_os_stream.h"
#include <math.h>

typedef struct {
    const char *name;
    solar_os_pwm_output_info_t outputs[16];
    uint32_t frequency;
    bool fail_open, fail_write, fail_close, block, probe_lifetime;
    unsigned writes;
} fake_bank_t;

static pthread_mutex_t barrier = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool entered, proceed;

static size_t bank_count(void *ctx)
{
    fake_bank_t *bank = ctx;
    if (bank->probe_lifetime) assert(solar_os_pwm_unregister(bank->name) == ESP_ERR_INVALID_STATE);
    return 16;
}
static bool bank_get(void *ctx, size_t index, solar_os_pwm_output_info_t *info)
{
    if (index >= 16) return false;
    fake_bank_t *bank = ctx;
    *info = bank->outputs[index];
    info->output = (uint16_t)index;
    info->pin = -1;
    info->runtime_allowed = true;
    info->resolution_bits = 12;
    return true;
}

static esp_err_t bank_config(fake_bank_t *bank, uint16_t output, uint32_t frequency)
{
    if (output >= 16) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 16; i++) {
        if (i != output && bank->outputs[i].active && frequency != bank->frequency) return ESP_ERR_INVALID_STATE;
    }
    bank->frequency = frequency;
    bank->outputs[output].freq_hz = frequency;
    bank->outputs[output].active = true;
    return ESP_OK;
}

static esp_err_t bank_set(void *ctx, uint16_t output, uint32_t frequency, uint32_t value, bool pulse)
{
    fake_bank_t *bank = ctx;
    esp_err_t ret = bank_config(bank, output, frequency);
    if (ret == ESP_OK) {
        bank->outputs[output].enabled = true;
        bank->outputs[output].pulse_width_us = pulse ? value : (uint32_t)((uint64_t)value * 10000U / frequency);
    }
    return ret;
}

static esp_err_t bank_open(void *ctx, uint16_t output, uint32_t frequency, const char *owner)
{
    (void)owner;
    fake_bank_t *bank = ctx;
    if (bank->fail_open) { bank->fail_open = false; return ESP_FAIL; }
    return bank_config(bank, output, frequency);
}

static esp_err_t bank_write(void *ctx, uint16_t output, const char *owner, uint32_t pulse)
{
    (void)owner;
    fake_bank_t *bank = ctx;
    if (bank->fail_write) { bank->fail_write = false; return ESP_FAIL; }
    if (bank->block) {
        /* The callback can inspect registry metadata: no global lock spans I/O. */
        assert(solar_os_pwm_provider_count() == 2);
        assert(solar_os_pwm_unregister(bank->name) == ESP_ERR_INVALID_STATE);
        assert(pthread_mutex_lock(&barrier) == 0);
        entered = true;
        pthread_cond_broadcast(&condition);
        while (!proceed) pthread_cond_wait(&condition, &barrier);
        pthread_mutex_unlock(&barrier);
    }
    bank->outputs[output].pulse_width_us = pulse;
    bank->outputs[output].enabled = true;
    bank->writes++;
    return ESP_OK;
}

static esp_err_t bank_disable(void *ctx, uint16_t output, const char *owner)
{
    (void)owner;
    ((fake_bank_t *)ctx)->outputs[output].enabled = false;
    return ESP_OK;
}

static esp_err_t bank_close(void *ctx, uint16_t output, const char *owner)
{
    (void)owner;
    fake_bank_t *bank = ctx;
    if (bank->fail_close) { bank->fail_close = false; return ESP_FAIL; }
    bank->outputs[output] = (solar_os_pwm_output_info_t){0};
    return ESP_OK;
}

static esp_err_t bank_stop(void *ctx, uint16_t output) { return bank_close(ctx, output, ""); }
static const solar_os_pwm_ops_t ops = {
    .output_count = bank_count, .get_output = bank_get, .set = bank_set,
    .open = bank_open, .write_pulse = bank_write, .disable = bank_disable,
    .close = bank_close, .stop = bank_stop,
};

static esp_err_t attach(fake_bank_t *bank)
{
    const solar_os_pwm_registration_t registration = {
        .name = bank->name, .driver = "test-pwm-bank", .detachable = true,
        .ops = &ops, .ctx = bank,
    };
    return solar_os_pwm_register(&registration);
}

static void *write_worker(void *unused)
{
    (void)unused;
    assert(solar_os_servo_set("arm", "python", RAD(10)) == ESP_OK);
    return NULL;
}

static void servo_stream_test(fake_bank_t *bank)
{
    solar_os_servo_config_t c={.output={.provider="bank0",.output=0},.freq_hz=50,
        .pulse_min_us=1000,.pulse_max_us=2000,.angle_min_rad=RAD(-90),.angle_max_rad=RAD(90)};
    assert(solar_os_servo_open("stream-servo","shell",&c)==ESP_OK);
    solar_os_stream_info_t metadata;
    assert(solar_os_stream_get_info("servo.stream-servo",&metadata)==ESP_OK && !strcmp(metadata.unit,"rad"));
    solar_os_stream_scalar_sample_t sample;
    assert(solar_os_stream_observe_scalar_sample("servo.stream-servo",NULL,&sample)==ESP_ERR_TIMEOUT);
    solar_os_stream_open_options_t sink={.direction=SOLAR_OS_STREAM_DIRECTION_SINK};
    solar_os_stream_handle_t writer=SOLAR_OS_STREAM_HANDLE_INIT,reader=SOLAR_OS_STREAM_HANDLE_INIT,other=SOLAR_OS_STREAM_HANDLE_INIT;
    unsigned before=bank->writes;
    assert(solar_os_stream_open_ex("servo.stream-servo","publisher",&sink,&writer)==ESP_OK && bank->writes==before);
    assert(solar_os_stream_open_ex("servo.stream-servo","competitor",&sink,&other)==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_open("servo.stream-servo","reader",&reader)==ESP_OK);
    float angle=.785398163f;size_t written;
    assert(solar_os_stream_write(&writer,&angle,sizeof(angle),0,&written)==ESP_OK && written==sizeof(angle));
    assert(bank->outputs[0].pulse_width_us==1750);
    assert(solar_os_stream_observe_scalar_sample("servo.stream-servo",NULL,&sample)==ESP_OK && fabsf(sample.value-angle)<.00001f);
    assert(solar_os_servo_set("stream-servo","shell",0)==ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_pulse("stream-servo","shell",1500)==ESP_ERR_INVALID_STATE);
    before=bank->writes;
    angle=NAN;assert(solar_os_stream_write(&writer,&angle,sizeof(angle),0,&written)==ESP_ERR_INVALID_ARG && !written);
    angle=3;assert(solar_os_stream_write(&writer,&angle,sizeof(angle),0,&written)==ESP_ERR_INVALID_ARG && bank->writes==before);
    angle=1.570796327f;assert(solar_os_stream_write(&writer,&angle,sizeof(angle),0,&written)==ESP_OK && bank->outputs[0].pulse_width_us==2000);
    assert(solar_os_servo_disable("stream-servo","shell")==ESP_OK && !bank->outputs[0].enabled);
    angle=0;assert(solar_os_stream_write(&writer,&angle,sizeof(angle),0,&written)==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_close_ex(&writer)==ESP_OK);
    assert(solar_os_servo_set("stream-servo","shell",0)==ESP_OK);
    assert(solar_os_servo_close("stream-servo","shell")==ESP_ERR_INVALID_STATE && !bank->outputs[0].active);
    assert(solar_os_servo_open("replacement","shell",&c)==ESP_OK);
    assert(solar_os_servo_set("replacement","shell",RAD(10))==ESP_OK);
    assert(solar_os_servo_set("stream-servo","shell",0)==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_read_scalar_sample(&reader,NULL,&sample)==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_close_ex(&reader)==ESP_OK);
    assert(solar_os_servo_close("stream-servo","shell")==ESP_OK);
    assert(bank->outputs[0].enabled); /* Retry must not close a reused PWM output. */
    assert(solar_os_servo_close("replacement","shell")==ESP_OK);
    assert(solar_os_servo_open_private("private-servo","robot",&c)==ESP_OK);
    assert(solar_os_stream_get_info("servo.private-servo",&metadata)==ESP_OK && metadata.direction==SOLAR_OS_STREAM_DIRECTION_SOURCE);
    assert(solar_os_stream_open_ex("servo.private-servo","publisher",&sink,&writer)==ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_servo_set("private-servo","robot",RAD(10))==ESP_OK);
    assert(solar_os_stream_observe_scalar_sample("servo.private-servo",NULL,&sample)==ESP_OK);
    assert(solar_os_servo_close("private-servo","robot")==ESP_OK);
    /* A consumer can finish safe-stop cleanup after the owner closes PWM. */
    assert(solar_os_servo_open("stop-order","shell",&c)==ESP_OK);
    assert(solar_os_stream_open_ex("servo.stop-order","controller",&sink,&writer)==ESP_OK);
    angle=0;assert(solar_os_stream_write(&writer,&angle,sizeof(angle),0,&written)==ESP_OK);
    assert(solar_os_servo_close("stop-order","shell")==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_write(&writer,&angle,sizeof(angle),0,&written)==ESP_ERR_NOT_FOUND);
    assert(solar_os_stream_close_ex(&writer)==ESP_OK);
    assert(solar_os_servo_close("stop-order","shell")==ESP_OK);
    puts("servo streams: radians, calibration bounds, exclusive writer, disable, cleanup retry and private owner passed");
}

int main(void)
{
    alarm(15); /* A lock regression fails instead of hanging the host suite. */
    assert(solar_os_pwm_provider_count() == 0); /* No GPIO or LEDC linked. */
    fake_bank_t a = {.name = "bank0"}, b = {.name = "bank1"};
    assert(attach(&a) == ESP_OK && attach(&b) == ESP_OK);
    assert(attach(&a) == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_provider_count() == 2 && solar_os_pwm_output_count() == 32);
    solar_os_pwm_provider_info_t provider;
    assert(solar_os_pwm_get_provider(0, &provider) && provider.output_count == 16 && provider.detachable);
    solar_os_pwm_output_t output;
    assert(solar_os_pwm_parse_output("bank0:15", &output) && output.output == 15);
    assert(!solar_os_pwm_parse_output("bank0:65536", &output));
    assert(!solar_os_pwm_parse_output("bank0:-1", &output));
    assert(!solar_os_pwm_parse_output("bank0:", &output));
    assert(!solar_os_pwm_parse_output("bank0:1:2", &output));
    assert(solar_os_pwm_parse_output("1", &output) && strcmp(output.provider, "native") == 0);
    assert(solar_os_pwm_open(1, 50, "native-test") == ESP_ERR_NOT_FOUND);

    solar_os_servo_config_t config = {
        .output = {.provider = "bank0", .output = 0}, .freq_hz = 50,
        .pulse_min_us = 1000, .pulse_max_us = 2000, .angle_min_rad = RAD(-45), .angle_max_rad = RAD(45),
    };
    a.fail_open = true;
    assert(solar_os_servo_open("failed", "python", &config) == ESP_FAIL);
    assert(solar_os_servo_count() == 0);
    assert(solar_os_pwm_unregister("bank0") == ESP_OK); /* failed open rolled back */
    assert(attach(&a) == ESP_OK);
    assert(solar_os_servo_open("arm", "python", &config) == ESP_OK);
    assert(!a.outputs[0].enabled && a.outputs[0].pulse_width_us == 0);
    assert(solar_os_pwm_unregister("bank0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_set_output(&config.output, 50, 25, false) == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_stop_output(&config.output) == ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_set("arm", "lua", 0) == ESP_ERR_INVALID_STATE);
    assert(a.writes == 0);
    config.output.output = 1;
    config.freq_hz = 100;
    assert(solar_os_servo_open("conflict", "python", &config) == ESP_ERR_INVALID_STATE);
    memcpy(config.output.provider, "bank1", 6);
    assert(solar_os_servo_open("other", "lua", &config) == ESP_OK);
    assert(a.frequency == 50 && b.frequency == 100); /* independent timing domains */
    assert(solar_os_servo_set("arm", "python", 0) == ESP_OK && a.outputs[0].pulse_width_us == 1500);
    a.fail_write = true;
    assert(solar_os_servo_set("arm", "python", RAD(45)) == ESP_FAIL);
    solar_os_servo_info_t servo;
    assert(solar_os_servo_status("arm", &servo) == ESP_OK && servo.commanded_pulse_us == 1500);
    assert(solar_os_servo_disable("arm", "python") == ESP_OK);
    assert(solar_os_pwm_unregister("bank0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_set("arm", "python", 0) == ESP_OK);

    a.block = true;
    pthread_t worker;
    assert(pthread_create(&worker, NULL, write_worker, NULL) == 0);
    pthread_mutex_lock(&barrier);
    while (!entered) pthread_cond_wait(&condition, &barrier);
    pthread_mutex_unlock(&barrier);
    assert(solar_os_pwm_unregister("bank0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_set("other", "lua", RAD(20)) == ESP_OK); /* no global servo lock spans I/O */
    pthread_mutex_lock(&barrier);
    proceed = true;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&barrier);
    assert(pthread_join(worker, NULL) == 0);
    a.block = false;
    a.fail_close = true;
    assert(solar_os_servo_release_owner("python") == ESP_FAIL);
    assert(solar_os_pwm_unregister("bank0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_release_owner("python") == ESP_OK);
    assert(solar_os_servo_release_owner("lua") == ESP_OK);
    a.probe_lifetime = true;
    assert(solar_os_pwm_get_provider(0, &provider)); /* refs protect enumeration without bindings */
    a.probe_lifetime = false;

    /* A full controller is usable: the servo limit is independent of native's four channels. */
    config.freq_hz = 50;
    memcpy(config.output.provider, "bank0", 6);
    for (unsigned i = 0; i < 16; i++) {
        char name[16];
        snprintf(name, sizeof(name), "servo%u", i);
        config.output.output = (uint16_t)i;
        assert(solar_os_servo_open(name, "python", &config) == ESP_OK);
    }
    assert(solar_os_servo_count() == 16);
    assert(solar_os_servo_open("over-limit", "python", &config) == ESP_ERR_NO_MEM);
    assert(solar_os_servo_release_owner("python") == ESP_OK);

    fake_bank_t c = {.name = "bank2"}, d = {.name = "bank3"}, e = {.name = "bank4"};
    assert(attach(&c) == ESP_OK && attach(&d) == ESP_OK);
    assert(attach(&e) == ESP_ERR_NO_MEM);
    /* Exhaust the independent output pool and verify failed allocation rollback. */
    for (unsigned i = 0; i < 16; i++) {
        output = (solar_os_pwm_output_t){.provider = "bank0", .output = (uint16_t)i};
        assert(solar_os_pwm_set_output(&output, 50, 1500, true) == ESP_OK);
        memcpy(output.provider, "bank1", 6);
        assert(solar_os_pwm_set_output(&output, 50, 1500, true) == ESP_OK);
    }
    output = (solar_os_pwm_output_t){.provider = "bank2", .output = 0};
    assert(solar_os_pwm_open_output(&output, 50, "extra") == ESP_ERR_NO_MEM);
    assert(!c.outputs[0].active && solar_os_pwm_unregister("bank2") == ESP_OK);
    assert(solar_os_pwm_unregister("bank3") == ESP_OK);
    for (unsigned i = 0; i < 16; i++) {
        output = (solar_os_pwm_output_t){.provider = "bank0", .output = (uint16_t)i};
        assert(solar_os_pwm_stop_output(&output) == ESP_OK);
        memcpy(output.provider, "bank1", 6);
        assert(solar_os_pwm_stop_output(&output) == ESP_OK);
    }

    output = (solar_os_pwm_output_t){.provider = "bank0", .output = 15};
    assert(solar_os_pwm_set_output(&output, 50, 1500, true) == ESP_OK);
    assert(solar_os_pwm_unregister("bank0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_stop_output(&output) == ESP_OK);
    servo_stream_test(&a);
    assert(solar_os_pwm_unregister("bank0") == ESP_OK);
    assert(solar_os_pwm_unregister("bank1") == ESP_OK);
    assert(solar_os_pwm_provider_count() == 0);
    puts("PWM provider lifecycle, timing domains and concurrent servo tests passed");
    return 0;
}
