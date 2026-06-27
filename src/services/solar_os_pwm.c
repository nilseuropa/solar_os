#include "solar_os_pwm.h"

#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_PWM
#include "driver/gpio.h"
#include "pwm_port.h"
#endif

#include "solar_os_gpio.h"

esp_err_t solar_os_pwm_init(void)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    return ESP_ERR_NOT_SUPPORTED;
#else
    return pwm_port_init();
#endif
}

size_t solar_os_pwm_pin_count(void)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    return 0;
#else
    size_t count = 0;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t gpio_info;
        if (solar_os_gpio_get_pin_info(i, &gpio_info) && gpio_info.runtime_allowed) {
            count++;
        }
    }
    return count;
#endif
}

bool solar_os_pwm_get_pin_info(size_t index, solar_os_pwm_pin_info_t *info)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    (void)index;
    (void)info;
    return false;
#else
    if (info == NULL) {
        return false;
    }

    size_t current = 0;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t gpio_info;
        if (!solar_os_gpio_get_pin_info(i, &gpio_info) || !gpio_info.runtime_allowed) {
            continue;
        }
        if (current++ != index) {
            continue;
        }

        pwm_port_status_t status;
        (void)pwm_port_get((gpio_num_t)gpio_info.pin, &status);
        *info = (solar_os_pwm_pin_info_t) {
            .pin = gpio_info.pin,
            .runtime_allowed = true,
            .active = status.active,
            .channel = status.active ? (int)status.channel : -1,
            .freq_hz = status.freq_hz,
            .duty_percent = status.duty_percent,
        };
        return true;
    }
    return false;
#endif
}

esp_err_t solar_os_pwm_set(int pin, uint32_t freq_hz, uint8_t duty_percent)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    (void)pin;
    (void)freq_hz;
    (void)duty_percent;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (freq_hz < SOLAR_OS_PWM_FREQ_MIN_HZ ||
        freq_hz > SOLAR_OS_PWM_FREQ_MAX_HZ ||
        duty_percent > SOLAR_OS_PWM_DUTY_MAX_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }
    return pwm_port_set((gpio_num_t)pin, freq_hz, duty_percent);
#endif
}

esp_err_t solar_os_pwm_stop(int pin)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    (void)pin;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return pwm_port_stop((gpio_num_t)pin);
#endif
}
