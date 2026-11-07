#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/ledc.h"
#include "solar_os_gpio.h"
#include "solar_os_pwm.h"
#include "solar_os_servo.h"
#define RAD(x) ((x) * (3.14159265358979323846f / 180.0f))
#include "pwm_port.h"

static ledc_timer_config_t timer;
static uint32_t duties[4], pending[4];
static bool enabled[4];
static int pin_channels[8];
static unsigned timer_configs, channel_configs;
static bool fail_timer, fail_channel, fail_update, fail_stop;

esp_err_t ledc_timer_config(const ledc_timer_config_t *c)
{
    assert(c->timer_num == LEDC_TIMER_0);
    if (fail_timer) { fail_timer = false; return ESP_FAIL; }
    timer = *c;
    timer_configs++;
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *c)
{
    if (fail_channel) { fail_channel = false; return ESP_FAIL; }
    assert(c->channel >= 0 && c->channel < 4 && c->timer_sel == 0);
    pending[c->channel] = duties[c->channel] = c->duty;
    pin_channels[c->gpio_num] = c->channel;
    enabled[c->channel] = true;
    channel_configs++;
    return ESP_OK;
}

esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty)
{
    assert(mode == 0 && channel >= 0 && channel < 4);
    if (duty >= (1U << timer.duty_resolution)) return ESP_ERR_INVALID_ARG;
    pending[channel] = duty;
    return ESP_OK;
}

esp_err_t ledc_update_duty(int mode, int channel)
{
    assert(mode == 0 && channel >= 0 && channel < 4);
    if (fail_update) { fail_update = false; return ESP_FAIL; }
    duties[channel] = pending[channel];
    enabled[channel] = true;
    return ESP_OK;
}

esp_err_t ledc_stop(int mode, int channel, uint32_t idle)
{
    assert(mode == 0 && channel >= 0 && channel < 4 && idle == 0);
    if (fail_stop) { fail_stop = false; return ESP_FAIL; }
    enabled[channel] = false;
    return ESP_OK;
}

bool gpio_port_is_valid_output_pin(int pin) { return pin >= 0 && pin < 8; }
esp_err_t gpio_reset_pin(gpio_num_t pin)
{
    assert(pin >= 0 && pin < 8);
    pin_channels[pin] = -1;
    return ESP_OK;
}
bool solar_os_gpio_is_runtime_allowed(int pin) { return pin >= 1 && pin < 8; }
size_t solar_os_gpio_pin_count(void) { return 7U; }
bool solar_os_gpio_get_pin_info(size_t index, solar_os_gpio_pin_info_t *info)
{
    if (index >= 7 || info == NULL) return false;
    *info = (solar_os_gpio_pin_info_t){.pin = (int)index + 1, .runtime_allowed = true};
    return true;
}

static void assert_claim(int pin, const char *owner)
{
    solar_os_resource_claim_t claim;
    assert(solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim));
    assert(strcmp(claim.owner, owner) == 0);
    assert(solar_os_resource_find_claim(SOLAR_OS_RESOURCE_PWM_PIN, pin, -1, &claim));
    assert(strcmp(claim.owner, owner) == 0);
}

static void assert_no_claim(int pin)
{
    assert(!solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, NULL));
    assert(!solar_os_resource_find_claim(SOLAR_OS_RESOURCE_PWM_PIN, pin, -1, NULL));
}

int main(void)
{
    for (size_t i = 0; i < 8; i++) pin_channels[i] = -1;
    solar_os_servo_config_t config = {
        .output = {.provider = "native", .output = 1}, .freq_hz = 50, .pulse_min_us = 1000, .pulse_max_us = 2000,
        .angle_min_rad = RAD(-45), .angle_max_rad = RAD(45),
    };
    solar_os_servo_info_t info;
    pwm_port_status_t pwm;

    assert(solar_os_servo_open("arm", "python", &config) == ESP_OK);
    assert_claim(1, "servo:arm");
    assert(solar_os_servo_status("arm", &info) == ESP_OK && !info.enabled && !info.has_command);
    assert(pwm_port_get(1, &pwm) && pwm.active && pwm.reserved && !pwm.enabled);
    assert(duties[pwm.channel] == 0 && timer.freq_hz == 50 && timer.duty_resolution == 14);

    assert(solar_os_servo_open("arm", "python", &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_open("other", "lua", &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_set("arm", "lua", 0) == ESP_ERR_INVALID_STATE);
    assert(solar_os_servo_set("arm", "python", NAN) == ESP_ERR_INVALID_ARG);
    assert(solar_os_servo_set("arm", "python", RAD(46)) == ESP_ERR_INVALID_ARG);
    assert(solar_os_servo_pulse("arm", "python", 999) == ESP_ERR_INVALID_ARG);
    assert(solar_os_pwm_set(1, 1000, 50) == ESP_ERR_INVALID_STATE);
    assert(pwm_port_set(1, 50, 50) == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_stop(1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_set(2, 1000, 50) == ESP_ERR_INVALID_STATE);
    assert(timer.freq_hz == 50);
    assert_no_claim(2);

    assert(solar_os_servo_set("arm", "python", RAD(-45)) == ESP_OK);
    assert(solar_os_servo_status("arm", &info) == ESP_OK && info.commanded_pulse_us == 1000);
    assert(solar_os_servo_set("arm", "python", RAD(45)) == ESP_OK);
    assert(solar_os_servo_status("arm", &info) == ESP_OK && info.commanded_pulse_us == 2000);
    assert(solar_os_servo_set("arm", "python", 0) == ESP_OK);
    assert(pwm_port_get(1, &pwm) && pwm.pulse_width_us == 1500);
    const uint32_t old_duty = duties[pwm.channel];
    const unsigned configs = channel_configs, timers = timer_configs;
    assert(solar_os_servo_pulse("arm", "python", 1501) == ESP_OK);
    assert(duties[pwm.channel] != old_duty); /* microseconds are not percentage-quantized */
    assert(channel_configs == configs && timer_configs == timers);

    fail_update = true;
    const uint32_t before_failure = duties[pwm.channel];
    assert(solar_os_servo_set("arm", "python", RAD(45)) == ESP_FAIL);
    assert(duties[pwm.channel] == before_failure);
    assert(solar_os_servo_status("arm", &info) == ESP_OK && info.commanded_pulse_us == 1501);
    assert_claim(1, "servo:arm");

    assert(solar_os_servo_disable("arm", "python") == ESP_OK);
    assert(!enabled[pwm.channel] && solar_os_servo_status("arm", &info) == ESP_OK && !info.enabled);
    assert_claim(1, "servo:arm");
    fail_update = true;
    assert(solar_os_servo_set("arm", "python", 0) == ESP_FAIL);
    assert(!enabled[pwm.channel]);
    assert(solar_os_servo_status("arm", &info) == ESP_OK && !info.enabled);
    assert(solar_os_pwm_set(2, 1000, 50) == ESP_ERR_INVALID_STATE); /* disabled reservation pins timer */
    assert(solar_os_servo_set("arm", "python", 0) == ESP_OK && enabled[pwm.channel]);
    fail_stop = true;
    assert(solar_os_servo_close("arm", "python") == ESP_FAIL);
    assert_claim(1, "servo:arm");
    assert(solar_os_servo_count() == 1);
    assert(solar_os_servo_close("arm", "python") == ESP_OK);
    assert_no_claim(1);
    assert(pin_channels[1] == -1); /* freed channels cannot restart this pin */

    config.inverted = true;
    assert(solar_os_servo_open("reverse", "lua", &config) == ESP_OK);
    assert(solar_os_servo_set("reverse", "lua", RAD(-45)) == ESP_OK);
    assert(solar_os_servo_status("reverse", &info) == ESP_OK && info.commanded_pulse_us == 2000);
    assert(solar_os_servo_pulse("reverse", "lua", 1000) == ESP_OK);
    assert(solar_os_servo_status("reverse", &info) == ESP_OK && info.commanded_angle_rad == RAD(45));
    config.output.output = 2;
    assert(solar_os_servo_open("second", "lua", &config) == ESP_OK);
    assert(solar_os_servo_release_owner("python") == ESP_OK && solar_os_servo_count() == 2);
    fail_stop = true;
    assert(solar_os_servo_release_owner("lua") == ESP_FAIL && solar_os_servo_count() == 1);
    assert(solar_os_servo_release_owner("lua") == ESP_OK && solar_os_servo_count() == 0);
    assert_no_claim(1); assert_no_claim(2);
    assert(pin_channels[1] == -1 && pin_channels[2] == -1);

    config.inverted = false;
    config.output.output = 0;
    assert(solar_os_servo_open("reserved", "shell", &config) == ESP_ERR_NOT_ALLOWED);
    config.output.output = 1;
    config.angle_max_rad = INFINITY;
    assert(solar_os_servo_open("bad", "shell", &config) == ESP_ERR_INVALID_ARG);
    config.angle_max_rad = RAD(45);
    config.freq_hz = 1000;
    assert(solar_os_servo_open("bad", "shell", &config) == ESP_ERR_INVALID_ARG);
    config.freq_hz = 50;
    fail_channel = true;
    assert(solar_os_servo_open("bad", "shell", &config) == ESP_FAIL);
    assert_no_claim(1);
    assert(solar_os_servo_count() == 0);

    /* Board-owned PWM participates even without a runtime resource claim. */
    assert(pwm_port_set(0, 5000, 50) == ESP_OK);
    assert(solar_os_servo_open("conflict", "shell", &config) == ESP_ERR_INVALID_STATE);
    assert_no_claim(1);
    assert(timer.freq_hz == 5000);
    assert(pwm_port_stop(0) == ESP_OK);

    /* General PWM and servo outputs consume the same four channels. */
    assert(solar_os_pwm_set(7, 50, 25) == ESP_OK);
    for (int pin = 1; pin <= 3; pin++) {
        char name[8];
        snprintf(name, sizeof(name), "s%d", pin);
        config.output.output = pin;
        assert(solar_os_servo_open(name, "shell", &config) == ESP_OK);
    }
    config.output.output = 4;
    assert(solar_os_servo_open("full", "shell", &config) == ESP_ERR_NOT_FOUND);
    assert_no_claim(4);
    assert(solar_os_servo_count() == 3);
    assert(solar_os_servo_release_owner("shell") == ESP_OK);
    assert(solar_os_pwm_stop(7) == ESP_OK);

    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 1, -1,
        "other", "test") == ESP_OK);
    config.output.output = 1;
    assert(solar_os_servo_open("busy", "shell", &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_resource_release(SOLAR_OS_RESOURCE_GPIO_PIN, 1, -1, "other") == ESP_OK);

    assert(solar_os_pwm_set(1, 1000, 50) == ESP_OK);
    assert(solar_os_pwm_set(2, 1000, 25) == ESP_OK);
    assert(solar_os_pwm_set(1, 5000, 75) == ESP_ERR_INVALID_STATE);
    assert_claim(1, "pwm:1"); assert(timer.freq_hz == 1000);
    assert(solar_os_pwm_stop(2) == ESP_OK);
    fail_timer = true;
    assert(solar_os_pwm_set(1, 5000, 75) == ESP_FAIL);
    assert_claim(1, "pwm:1"); assert(timer.freq_hz == 1000);
    assert(pwm_port_get(1, &pwm));
    const uint32_t old_percent_duty = duties[pwm.channel];
    fail_update = true;
    assert(solar_os_pwm_set(1, 5000, 75) == ESP_FAIL);
    assert(timer.freq_hz == 1000 && timer.duty_resolution == 14);
    assert(duties[pwm.channel] == old_percent_duty);
    assert_claim(1, "pwm:1");
    assert(solar_os_pwm_set(1, 5000, 75) == ESP_OK);
    assert(timer.freq_hz == 5000 && timer.duty_resolution == 10);
    assert(solar_os_pwm_stop(1) == ESP_OK);
    assert(solar_os_pwm_set_pulse(1, 50, 1500) == ESP_OK);
    assert(pwm_port_get(1, &pwm) && !pwm.reserved && pwm.pulse_width_us == 1500);
    assert(solar_os_pwm_set_pulse(1, 50, 20000) == ESP_ERR_INVALID_ARG);
    assert(solar_os_pwm_stop(1) == ESP_OK);

    /* Closing one reservation must not release another pin with the same owner. */
    assert(solar_os_pwm_open(1, 50, "native") == ESP_OK);
    assert(solar_os_pwm_open(2, 50, "native") == ESP_OK);
    assert(solar_os_pwm_close(1, "native") == ESP_OK);
    assert_claim(2, "native");
    assert(solar_os_pwm_close(2, "native") == ESP_OK);
    assert(solar_os_resource_claim_count() == 0);
    puts("PWM and servo tests passed");
}
