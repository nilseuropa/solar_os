#include "solar_os_board_caps.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    solar_os_board_capability_t capability;
    const char *name;
} board_capability_name_t;

static const board_capability_name_t capability_names[] = {
    {SOLAR_OS_BOARD_CAP_PSRAM, "psram"},
    {SOLAR_OS_BOARD_CAP_DISPLAY, "display"},
    {SOLAR_OS_BOARD_CAP_GFX, "gfx"},
    {SOLAR_OS_BOARD_CAP_CDC, "cdc"},
    {SOLAR_OS_BOARD_CAP_UART, "uart"},
    {SOLAR_OS_BOARD_CAP_SD, "sd"},
    {SOLAR_OS_BOARD_CAP_I2C, "i2c"},
    {SOLAR_OS_BOARD_CAP_SPI, "spi"},
    {SOLAR_OS_BOARD_CAP_RTC, "rtc"},
    {SOLAR_OS_BOARD_CAP_BATTERY, "battery"},
    {SOLAR_OS_BOARD_CAP_AUDIO, "audio"},
    {SOLAR_OS_BOARD_CAP_AUDIO_INPUT, "audio_input"},
    {SOLAR_OS_BOARD_CAP_WIFI, "wifi"},
    {SOLAR_OS_BOARD_CAP_BLE, "ble"},
    {SOLAR_OS_BOARD_CAP_GPIO, "gpio"},
    {SOLAR_OS_BOARD_CAP_ADC, "adc"},
    {SOLAR_OS_BOARD_CAP_PWM, "pwm"},
    {SOLAR_OS_BOARD_CAP_EXPANSION_GPIO, "expansion_gpio"},
    {SOLAR_OS_BOARD_CAP_EXPANSION_I2C, "expansion_i2c"},
    {SOLAR_OS_BOARD_CAP_EXPANSION_SPI, "expansion_spi"},
    {SOLAR_OS_BOARD_CAP_EXPANSION_UART, "expansion_uart"},
    {SOLAR_OS_BOARD_CAP_EXPANSION_ADC, "expansion_adc"},
    {SOLAR_OS_BOARD_CAP_EXPANSION_PWM, "expansion_pwm"},
    {SOLAR_OS_BOARD_CAP_KEY, "key"},
    {SOLAR_OS_BOARD_CAP_BUTTONS, "buttons"},
    {SOLAR_OS_BOARD_CAP_JOYSTICK, "joystick"},
    {SOLAR_OS_BOARD_CAP_ADC_DPAD, "adc_dpad"},
    {SOLAR_OS_BOARD_CAP_STATUS_LED, "status_led"},
    {SOLAR_OS_BOARD_CAP_DISPLAY_BRIGHTNESS, "display_brightness"},
    {SOLAR_OS_BOARD_CAP_TEMPERATURE, "temperature"},
    {SOLAR_OS_BOARD_CAP_HUMIDITY, "humidity"},
};

solar_os_board_capabilities_t solar_os_board_capabilities(void)
{
    return
        (SOLAR_OS_BOARD_HAS_DISPLAY ? SOLAR_OS_BOARD_CAP_DISPLAY : 0U) |
        (SOLAR_OS_BOARD_HAS_GFX ? SOLAR_OS_BOARD_CAP_GFX : 0U) |
        (SOLAR_OS_BOARD_HAS_CDC ? SOLAR_OS_BOARD_CAP_CDC : 0U) |
        (SOLAR_OS_BOARD_HAS_UART ? SOLAR_OS_BOARD_CAP_UART : 0U) |
        (SOLAR_OS_BOARD_HAS_SD ? SOLAR_OS_BOARD_CAP_SD : 0U) |
        (SOLAR_OS_BOARD_HAS_I2C ? SOLAR_OS_BOARD_CAP_I2C : 0U) |
        (SOLAR_OS_BOARD_HAS_SPI ? SOLAR_OS_BOARD_CAP_SPI : 0U) |
        (SOLAR_OS_BOARD_HAS_RTC ? SOLAR_OS_BOARD_CAP_RTC : 0U) |
        (SOLAR_OS_BOARD_HAS_BATTERY ? SOLAR_OS_BOARD_CAP_BATTERY : 0U) |
        (SOLAR_OS_BOARD_HAS_AUDIO ? SOLAR_OS_BOARD_CAP_AUDIO : 0U) |
        (SOLAR_OS_BOARD_HAS_AUDIO_INPUT ? SOLAR_OS_BOARD_CAP_AUDIO_INPUT : 0U) |
        (SOLAR_OS_BOARD_HAS_WIFI ? SOLAR_OS_BOARD_CAP_WIFI : 0U) |
        (SOLAR_OS_BOARD_HAS_BLE ? SOLAR_OS_BOARD_CAP_BLE : 0U) |
        (SOLAR_OS_BOARD_HAS_GPIO ? SOLAR_OS_BOARD_CAP_GPIO : 0U) |
        (SOLAR_OS_BOARD_HAS_ADC ? SOLAR_OS_BOARD_CAP_ADC : 0U) |
        (SOLAR_OS_BOARD_HAS_PWM ? SOLAR_OS_BOARD_CAP_PWM : 0U) |
        (SOLAR_OS_BOARD_HAS_EXPANSION_GPIO ? SOLAR_OS_BOARD_CAP_EXPANSION_GPIO : 0U) |
        (SOLAR_OS_BOARD_HAS_EXPANSION_I2C ? SOLAR_OS_BOARD_CAP_EXPANSION_I2C : 0U) |
        (SOLAR_OS_BOARD_HAS_EXPANSION_SPI ? SOLAR_OS_BOARD_CAP_EXPANSION_SPI : 0U) |
        (SOLAR_OS_BOARD_HAS_EXPANSION_UART ? SOLAR_OS_BOARD_CAP_EXPANSION_UART : 0U) |
        (SOLAR_OS_BOARD_HAS_EXPANSION_ADC ? SOLAR_OS_BOARD_CAP_EXPANSION_ADC : 0U) |
        (SOLAR_OS_BOARD_HAS_EXPANSION_PWM ? SOLAR_OS_BOARD_CAP_EXPANSION_PWM : 0U) |
        (SOLAR_OS_BOARD_HAS_KEY ? SOLAR_OS_BOARD_CAP_KEY : 0U) |
        (SOLAR_OS_BOARD_HAS_BUTTONS ? SOLAR_OS_BOARD_CAP_BUTTONS : 0U) |
        (SOLAR_OS_BOARD_HAS_JOYSTICK ? SOLAR_OS_BOARD_CAP_JOYSTICK : 0U) |
        (SOLAR_OS_BOARD_HAS_ADC_DPAD ? SOLAR_OS_BOARD_CAP_ADC_DPAD : 0U) |
        (SOLAR_OS_BOARD_HAS_STATUS_LED ? SOLAR_OS_BOARD_CAP_STATUS_LED : 0U) |
        (SOLAR_OS_BOARD_HAS_DISPLAY_BRIGHTNESS ? SOLAR_OS_BOARD_CAP_DISPLAY_BRIGHTNESS : 0U) |
        (SOLAR_OS_BOARD_HAS_TEMPERATURE ? SOLAR_OS_BOARD_CAP_TEMPERATURE : 0U) |
        (SOLAR_OS_BOARD_HAS_HUMIDITY ? SOLAR_OS_BOARD_CAP_HUMIDITY : 0U) |
        (SOLAR_OS_BOARD_HAS_PSRAM ? SOLAR_OS_BOARD_CAP_PSRAM : 0U);
}

bool solar_os_board_has(solar_os_board_capability_t capability)
{
    return (solar_os_board_capabilities() & (solar_os_board_capabilities_t)capability) != 0;
}

const char *solar_os_board_capability_name(solar_os_board_capability_t capability)
{
    for (size_t i = 0; i < sizeof(capability_names) / sizeof(capability_names[0]); i++) {
        if (capability_names[i].capability == capability) {
            return capability_names[i].name;
        }
    }
    return "unknown";
}

void solar_os_board_capabilities_format(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    buffer[0] = '\0';
    size_t used = 0;
    bool any = false;
    const solar_os_board_capabilities_t caps = solar_os_board_capabilities();

    for (size_t i = 0; i < sizeof(capability_names) / sizeof(capability_names[0]); i++) {
        if ((caps & (solar_os_board_capabilities_t)capability_names[i].capability) == 0) {
            continue;
        }

        const int written = snprintf(buffer + used,
                                     buffer_len - used,
                                     "%s%s",
                                     any ? " " : "",
                                     capability_names[i].name);
        if (written < 0) {
            buffer[used] = '\0';
            return;
        }

        const size_t add = (size_t)written;
        if (add >= buffer_len - used) {
            buffer[buffer_len - 1] = '\0';
            return;
        }
        used += add;
        any = true;
    }

    if (!any) {
        strlcpy(buffer, "none", buffer_len);
    }
}
