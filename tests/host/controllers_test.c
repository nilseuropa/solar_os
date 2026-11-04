#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_controllers.h"
#include "solar_os_pwm.h"

static uint64_t now;
static float actuator;
static bool fail_write, pwm_claimed;
static unsigned pwm_writes;
static uint32_t pulse;
static solar_os_stream_scalar_sample_t input;
static esp_err_t input_error;
static bool fail_close;
static solar_os_parameter_registration_t registration;
static bool check_reentry;
static bool retire_output_in_callback;

uint64_t solar_os_time_uptime_ms(void) { return now; }
int64_t esp_timer_get_time(void) { return (int64_t)(now*1000); }
const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "error"; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
esp_err_t solar_os_stream_open(const char *id, const char *owner, solar_os_stream_handle_t *h)
{ (void)owner; (void)h; return strcmp(id, "adc1") == 0 ? ESP_OK : ESP_ERR_NOT_FOUND; }
void solar_os_stream_close(solar_os_stream_handle_t *h) { (void)h; }
esp_err_t solar_os_stream_close_ex(solar_os_stream_handle_t *h)
{ (void)h; return fail_close ? ESP_FAIL : ESP_OK; }
esp_err_t solar_os_stream_read_scalar_sample(solar_os_stream_handle_t *h,
    const solar_os_stream_read_options_t *o, solar_os_stream_scalar_sample_t *sample)
{ (void)h; (void)o; *sample = input; return input_error; }
bool solar_os_pwm_parse_output(const char *text, solar_os_pwm_output_t *out)
{
    if (strcmp(text, "native:2") != 0) return false;
    strcpy(out->provider, "native"); out->output = 2; return true;
}
esp_err_t solar_os_pwm_open_output(const solar_os_pwm_output_t *o, uint32_t hz, const char *owner)
{ (void)o; (void)owner; assert(hz == 50); if (pwm_claimed) return ESP_ERR_INVALID_STATE; pwm_claimed = true; return ESP_OK; }
esp_err_t solar_os_pwm_write_output(const solar_os_pwm_output_t *o, const char *owner, uint32_t value)
{ (void)o; (void)owner; assert(pwm_claimed); if (fail_write) return ESP_FAIL; pulse = value; pwm_writes++; return ESP_OK; }
esp_err_t solar_os_pwm_close_output(const solar_os_pwm_output_t *o, const char *owner)
{ (void)o; (void)owner; assert(pwm_claimed); if (fail_write) return ESP_FAIL; pwm_claimed = false; return ESP_OK; }
static esp_err_t get(void *user, float *value) { (void)user; *value = actuator; return ESP_OK; }
static esp_err_t set(void *user, float value)
{
    (void)user;
    if (check_reentry) {
        assert(solar_os_parameter_release("plant.drive", "ctl.loop") == ESP_ERR_INVALID_STATE);
        assert(solar_os_parameters_update_range(&registration, "drive", 0, 20) == ESP_ERR_INVALID_STATE);
    }
    if (retire_output_in_callback) {
        /* Retirement hides the path, but destruction waits for this callback. */
        assert(solar_os_parameters_unregister(&registration) == ESP_ERR_INVALID_STATE);
        assert(solar_os_parameter_release("plant.drive", "ctl.loop") == ESP_ERR_NOT_FOUND);
    }
    if (fail_write) return ESP_FAIL;
    actuator = value; return ESP_OK;
}
static void register_output(void)
{
    const solar_os_parameter_definition_t def = {.name = "drive", .label = "Drive", .unit = "",
        .minimum = 0, .maximum = 10, .get = get, .set = set};
    assert(solar_os_parameters_register("plant", &def, 1, &registration) == ESP_OK);
}
static solar_os_controller_info_t info(void)
{
    solar_os_controller_info_t result;
    assert(solar_os_controller_find("loop", &result) == ESP_OK);
    return result;
}
static void feed_tick(uint64_t time, float measurement)
{
    now = time;
    assert(solar_os_controller_feed("loop", measurement, now) == ESP_OK);
    solar_os_controllers_tick(now);
}

static void owned_controllers_test(void)
{
    now=1;
    solar_os_native_controller_config_v1_t c={.kp=.5f,.ki=1,.derivative_tau=.05f,
        .output_min=-.2f,.output_max=.2f,.max_dt=.2f,.setpoint_min=-3,.setpoint_max=3,
        .period_ms=20,.timeout_ms=100};
    uint32_t id=0;
    assert(solar_os_controller_open_owned("wheel","robot",&c,&id)==ESP_OK && id);
    solar_os_controller_info_t s;
    assert(solar_os_controller_find("wheel",&s)==ESP_OK && s.enabled && s.waiting && !strcmp(s.owner,"robot"));
    assert(solar_os_controller_create("wheel",SOLAR_OS_CONTROLLER_PID)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_configure("wheel","kp",2)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_configure("wheel","setpoint",1)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_bind("wheel","manual","parameter:plant.drive")==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_feed("wheel",0,0)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_manual("wheel",0)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_auto("wheel")==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_reset("wheel")==ESP_ERR_INVALID_STATE);
    float value;
    assert(solar_os_controller_step("wheel",0,.02f,&value)==ESP_ERR_INVALID_STATE);
    solar_os_controllers_tick(now+20); /* The generic worker must not double-step. */
    assert(solar_os_controller_find("wheel",&s)==ESP_OK && !s.steps && s.enabled);
    solar_os_native_controller_sample_v1_t sample={.setpoint=1,.measurement=0,.dt=.02f,.feedforward=.15f,.sample_us=1000,.run=1};
    solar_os_native_controller_output_v1_t out;
    assert(solar_os_controller_evaluate_owned(id,&sample,1000,&out)==ESP_OK);
    assert(out.output==.2f && out.saturated && out.i==0); /* Feedforward participates in anti-windup. */
    assert(solar_os_controller_find("wheel",&s)==ESP_OK && s.steps==1 && !s.waiting && s.state.p==out.p);
    assert(solar_os_controller_pause_owned(id)==ESP_OK);
    sample.sample_us+=20000; sample.run=0;
    assert(solar_os_controller_evaluate_owned(id,&sample,sample.sample_us,&out)==ESP_OK && !out.output && !out.i);
    assert(solar_os_controller_evaluate_owned(id,&sample,sample.sample_us,&out)==ESP_ERR_TIMEOUT);
    assert(solar_os_controller_find("wheel",&s)==ESP_OK && s.faulted && !s.enabled);
    assert(solar_os_controller_pause_owned(id)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_delete("wheel")==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_close_owned(id)==ESP_OK);
    uint32_t replacement=0;
    assert(solar_os_controller_open_owned("wheel","robot",&c,&replacement)==ESP_OK && id!=replacement);
    assert(solar_os_controller_close_owned(id)==ESP_ERR_NOT_FOUND); /* Stale owner cannot remove a replacement. */
    assert(solar_os_controller_stop("wheel")==ESP_OK);
    assert(solar_os_controller_evaluate_owned(replacement,&sample,sample.sample_us,&out)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_start("wheel")==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_close_owned(replacement)==ESP_OK);
    const char *long_name="Wheel.01234567890123456";
    assert(strlen(long_name)==23);
    assert(solar_os_controller_open_owned(long_name,"robot",&c,&id)==ESP_OK);
    assert(solar_os_controller_find(long_name,&s)==ESP_OK && !strcmp(s.config.name,long_name));
    assert(solar_os_controller_close_owned(id)==ESP_OK);
    uint32_t handles[SOLAR_OS_CONTROLLER_MAX];
    for(unsigned i=0;i<SOLAR_OS_CONTROLLER_MAX;++i) {
        char name[24];snprintf(name,sizeof(name),"owned_%u",i);
        assert(solar_os_controller_open_owned(name,"robot",&c,&handles[i])==ESP_OK);
    }
    assert(solar_os_controller_create("overflow",SOLAR_OS_CONTROLLER_PID)==ESP_ERR_NO_MEM);
    assert(solar_os_controller_open_owned("overflow","robot",&c,&id)==ESP_ERR_NO_MEM && !id);
    assert(solar_os_controllers_stop_all()==ESP_OK);
    for(unsigned i=0;i<SOLAR_OS_CONTROLLER_MAX;++i) assert(solar_os_controller_close_owned(handles[i])==ESP_OK);
    assert(solar_os_controller_open_owned("delayed","robot",&c,&id)==ESP_OK);
    now=201; sample.sample_us=1000;
    /* Simulate a sample that aged while its caller was waiting for admission. */
    assert(solar_os_controller_evaluate_owned(id,&sample,1000,&out)==ESP_ERR_TIMEOUT);
    assert(solar_os_controller_close_owned(id)==ESP_OK);
    assert(!solar_os_controller_count() && !solar_os_parameter_count());
}

int main(void)
{
    register_output();
    assert(solar_os_controller_create("bad.name", SOLAR_OS_CONTROLLER_PID) == ESP_ERR_INVALID_ARG);
    assert(solar_os_controller_create("loop", SOLAR_OS_CONTROLLER_PID) == ESP_OK);
    assert(solar_os_controller_create("loop", SOLAR_OS_CONTROLLER_PID) == ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_configure("loop", "setpoint", 1) == ESP_OK);
    float sp;
    assert(solar_os_parameter_get("controller.loop.setpoint", &sp) == ESP_OK && sp == 1);
    assert(solar_os_parameter_set("controller.loop.setpoint", 2) == ESP_OK);
    assert(info().config.setpoint == 2);
    assert(solar_os_controller_configure("loop", "setpoint_min", 0) == ESP_OK);
    assert(solar_os_controller_configure("loop", "setpoint_max", 4) == ESP_OK);
    assert(solar_os_parameter_set_normalized("controller.loop.setpoint", UINT16_MAX) == ESP_OK);
    assert(info().config.setpoint == 4);
    assert(solar_os_controller_configure("loop", "setpoint", 5) == ESP_ERR_INVALID_ARG);
    assert(solar_os_controller_configure("loop", "setpoint", 2) == ESP_OK);
    assert(solar_os_controller_configure("loop", "setpoint_max", 1) == ESP_ERR_INVALID_ARG);
    assert(solar_os_controller_configure("loop", "period_ms", 1.5f) == ESP_ERR_INVALID_ARG);
    assert(solar_os_controller_bind("loop", "manual", "parameter:controller.loop.setpoint") == ESP_ERR_INVALID_ARG);
    assert(solar_os_controller_bind("loop", "manual", "parameter:plant.drive") == ESP_OK);
    assert(solar_os_controller_start("loop") == ESP_OK);
    assert(solar_os_parameter_set("plant.drive", 0.4f) == ESP_ERR_INVALID_STATE);
    assert(solar_os_parameter_set_normalized("plant.drive", 10) == ESP_ERR_INVALID_STATE);
    assert(solar_os_parameter_claim("plant.drive", "other") == ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_delete("loop") == ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_configure("loop", "kp", 2) == ESP_ERR_INVALID_STATE);
    check_reentry = true;
    feed_tick(100, 0); assert(actuator == 1); assert(info().steps == 1);
    check_reentry = false;
    /* Cached samples age out even though their value remains valid. */
    now = 301; solar_os_controllers_tick(now);
    assert(info().faulted && !info().enabled && info().claimed && actuator == 0);
    assert(info().last_error == ESP_ERR_TIMEOUT);
    assert(solar_os_controller_start("loop") == ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_stop("loop") == ESP_OK);
    assert(!info().claimed);

    assert(solar_os_controller_start("loop") == ESP_OK);
    feed_tick(400, 0);
    /* Output failure latches, failed cleanup retains ownership for retry. */
    fail_write = true; feed_tick(420, 0);
    assert(info().faulted && info().last_error == ESP_FAIL && info().claimed);
    assert(solar_os_controller_stop("loop") == ESP_FAIL);
    assert(solar_os_parameter_set("plant.drive", 0) == ESP_ERR_INVALID_STATE);
    fail_write = false; assert(solar_os_controller_stop("loop") == ESP_OK);
    assert(solar_os_controller_start("loop") == ESP_OK);
    retire_output_in_callback = true;
    feed_tick(500, 0);
    retire_output_in_callback = false;
    /* Provider replacement cannot inherit a controller's old writer claim. */
    assert(solar_os_parameter_set("plant.drive", 0) == ESP_ERR_NOT_FOUND);
    assert(solar_os_parameters_unregister(&registration) == ESP_OK);
    register_output(); actuator = 7;
    assert(solar_os_parameter_claim("plant.drive", "replacement") == ESP_OK);
    feed_tick(520, 0);
    assert(info().faulted && actuator == 7);
    assert(solar_os_controller_stop("loop") == ESP_OK);
    assert(!info().claimed && actuator == 7);
    assert(solar_os_parameter_set("plant.drive", 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_parameter_release("plant.drive", "replacement") == ESP_OK);

    assert(solar_os_controller_bind("loop", "adc1", "parameter:plant.drive") == ESP_OK);
    assert(solar_os_controller_start("loop") == ESP_OK);
    now = 600; input = (solar_os_stream_scalar_sample_t){.value = 1, .uptime_ms = now};
    solar_os_controllers_tick(now); assert(info().enabled);
    now = 620; input.uptime_ms = 700; solar_os_controllers_tick(now);
    assert(info().faulted); assert(solar_os_controller_stop("loop") == ESP_OK);
    assert(solar_os_controller_start("loop") == ESP_OK);
    input_error = ESP_ERR_NOT_SUPPORTED;
    now = 800; solar_os_controllers_tick(now); assert(info().faulted);
    assert(solar_os_controllers_stop_all() == ESP_OK); input_error = ESP_OK;
    assert(solar_os_controller_start("loop") == ESP_OK);
    now = 820; input.uptime_ms = now; fail_close = true;
    solar_os_controllers_tick(now); assert(info().faulted && info().claimed);
    assert(solar_os_controller_stop("loop") == ESP_FAIL && info().claimed);
    fail_close = false;
    assert(solar_os_controller_stop("loop") == ESP_OK && !info().claimed);

    assert(solar_os_controller_bind("loop", "manual", "parameter:plant.drive") == ESP_OK);
    assert(solar_os_controller_start("loop") == ESP_OK);
    feed_tick(1000, 0);
    feed_tick(2100, 0); assert(info().faulted); /* Missed deadline, despite fresh input. */
    assert(solar_os_controller_stop("loop") == ESP_OK);
    assert(solar_os_controller_reset("loop") == ESP_OK);
    assert(!info().faulted);
    float output;
    assert(solar_os_controller_manual("loop", 0.4f) == ESP_OK);
    assert(solar_os_controller_step("loop", 0, 0.02f, &output) == ESP_OK);
    assert(fabsf(output - 0.4f) < 0.0001f);
    assert(solar_os_controller_auto("loop") == ESP_OK);
    assert(solar_os_controller_step("loop", 0.5f, 0.02f, &output) == ESP_OK);
    assert(fabsf(output - 0.4f) < 0.0001f);

    assert(solar_os_controller_bind("loop", "manual", "pwm:native:2") == ESP_OK);
    assert(solar_os_controller_configure("loop", "output_max", 1000) == ESP_OK);
    assert(solar_os_controller_configure("loop", "safe_output", 100) == ESP_OK);
    assert(solar_os_controller_start("loop") == ESP_OK && pwm_claimed);
    assert(pwm_writes == 0); feed_tick(2200, 0); assert(pwm_writes == 1);
    assert(solar_os_controllers_stop_all() == ESP_OK && !pwm_claimed && pulse == 100);
    assert(solar_os_controller_delete("loop") == ESP_OK);
    assert(solar_os_parameter_get("controller.loop.setpoint", &sp) == ESP_ERR_NOT_FOUND);
    assert(solar_os_controller_count() == 0);
    assert(solar_os_parameters_unregister(&registration) == ESP_OK);
    /* Exhaustion is bounded, and deletion returns both slots and parameters. */
    for (unsigned i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) {
        char name[20]; snprintf(name, sizeof(name), "loop%u", i);
        assert(solar_os_controller_create(name, SOLAR_OS_CONTROLLER_HYSTERESIS) == ESP_OK);
    }
    assert(solar_os_controller_create("overflow", SOLAR_OS_CONTROLLER_PID) == ESP_ERR_NO_MEM);
    for (unsigned i = 0; i < SOLAR_OS_CONTROLLER_MAX; i++) {
        char name[20]; snprintf(name, sizeof(name), "loop%u", i);
        assert(solar_os_controller_delete(name) == ESP_OK);
    }
    assert(solar_os_controller_count() == 0 && solar_os_parameter_count() == 0);
    owned_controllers_test();
    puts("controllers service tests passed");
    return 0;
}
