#include "solar_os_native_device.h"
#include "solar_os_native_device_providers.h"
#include "solar_os_native_device_abi.h"
#include "solar_os_native_identity_abi.h"
#include "solar_os_native_rtc_abi.h"
#include "solar_os_native_power_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_config.h"
#include "solar_os_native_sensors_abi.h"
#include <string.h>
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_board_caps.h"
#include "solar_os_identity.h"
#include "solar_os_backlight.h"
#include "solar_os_power.h"
#include "solar_os_shutdown.h"
#include "esp_sleep.h"
static int copy_text(char *out, size_t capacity, const char *text)
{
    if (!text) text = "";
    size_t length = strlen(text);
    if (length >= capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    memcpy(out, text, length + 1); return SOLAR_OS_NATIVE_OK;
}
static int board_capabilities_out(uint64_t value, uint64_t *out)
{
    uint64_t known = 0; *out = 0;
    known |= SOLAR_OS_BOARD_CAP_DISPLAY;
    if (value & SOLAR_OS_BOARD_CAP_DISPLAY) *out |= SOLAR_OS_NATIVE_BOARD_CAP_DISPLAY;
    known |= SOLAR_OS_BOARD_CAP_GFX;
    if (value & SOLAR_OS_BOARD_CAP_GFX) *out |= SOLAR_OS_NATIVE_BOARD_CAP_GFX;
    known |= SOLAR_OS_BOARD_CAP_CDC;
    if (value & SOLAR_OS_BOARD_CAP_CDC) *out |= SOLAR_OS_NATIVE_BOARD_CAP_CDC;
    known |= SOLAR_OS_BOARD_CAP_UART;
    if (value & SOLAR_OS_BOARD_CAP_UART) *out |= SOLAR_OS_NATIVE_BOARD_CAP_UART;
    known |= SOLAR_OS_BOARD_CAP_SD;
    if (value & SOLAR_OS_BOARD_CAP_SD) *out |= SOLAR_OS_NATIVE_BOARD_CAP_SD;
    known |= SOLAR_OS_BOARD_CAP_I2C;
    if (value & SOLAR_OS_BOARD_CAP_I2C) *out |= SOLAR_OS_NATIVE_BOARD_CAP_I2C;
    known |= SOLAR_OS_BOARD_CAP_RTC;
    if (value & SOLAR_OS_BOARD_CAP_RTC) *out |= SOLAR_OS_NATIVE_BOARD_CAP_RTC;
    known |= SOLAR_OS_BOARD_CAP_BATTERY;
    if (value & SOLAR_OS_BOARD_CAP_BATTERY) *out |= SOLAR_OS_NATIVE_BOARD_CAP_BATTERY;
    known |= SOLAR_OS_BOARD_CAP_AUDIO;
    if (value & SOLAR_OS_BOARD_CAP_AUDIO) *out |= SOLAR_OS_NATIVE_BOARD_CAP_AUDIO;
    known |= SOLAR_OS_BOARD_CAP_WIFI;
    if (value & SOLAR_OS_BOARD_CAP_WIFI) *out |= SOLAR_OS_NATIVE_BOARD_CAP_WIFI;
    known |= SOLAR_OS_BOARD_CAP_BLE;
    if (value & SOLAR_OS_BOARD_CAP_BLE) *out |= SOLAR_OS_NATIVE_BOARD_CAP_BLE;
    known |= SOLAR_OS_BOARD_CAP_GPIO;
    if (value & SOLAR_OS_BOARD_CAP_GPIO) *out |= SOLAR_OS_NATIVE_BOARD_CAP_GPIO;
    known |= SOLAR_OS_BOARD_CAP_ADC;
    if (value & SOLAR_OS_BOARD_CAP_ADC) *out |= SOLAR_OS_NATIVE_BOARD_CAP_ADC;
    known |= SOLAR_OS_BOARD_CAP_PWM;
    if (value & SOLAR_OS_BOARD_CAP_PWM) *out |= SOLAR_OS_NATIVE_BOARD_CAP_PWM;
    known |= SOLAR_OS_BOARD_CAP_KEY;
    if (value & SOLAR_OS_BOARD_CAP_KEY) *out |= SOLAR_OS_NATIVE_BOARD_CAP_KEY;
    known |= SOLAR_OS_BOARD_CAP_TEMPERATURE;
    if (value & SOLAR_OS_BOARD_CAP_TEMPERATURE) *out |= SOLAR_OS_NATIVE_BOARD_CAP_TEMPERATURE;
    known |= SOLAR_OS_BOARD_CAP_HUMIDITY;
    if (value & SOLAR_OS_BOARD_CAP_HUMIDITY) *out |= SOLAR_OS_NATIVE_BOARD_CAP_HUMIDITY;
    known |= SOLAR_OS_BOARD_CAP_PSRAM;
    if (value & SOLAR_OS_BOARD_CAP_PSRAM) *out |= SOLAR_OS_NATIVE_BOARD_CAP_PSRAM;
    known |= SOLAR_OS_BOARD_CAP_BUTTONS;
    if (value & SOLAR_OS_BOARD_CAP_BUTTONS) *out |= SOLAR_OS_NATIVE_BOARD_CAP_BUTTONS;
    known |= SOLAR_OS_BOARD_CAP_JOYSTICK;
    if (value & SOLAR_OS_BOARD_CAP_JOYSTICK) *out |= SOLAR_OS_NATIVE_BOARD_CAP_JOYSTICK;
    known |= SOLAR_OS_BOARD_CAP_ADC_DPAD;
    if (value & SOLAR_OS_BOARD_CAP_ADC_DPAD) *out |= SOLAR_OS_NATIVE_BOARD_CAP_ADC_DPAD;
    known |= SOLAR_OS_BOARD_CAP_SPI;
    if (value & SOLAR_OS_BOARD_CAP_SPI) *out |= SOLAR_OS_NATIVE_BOARD_CAP_SPI;
    known |= SOLAR_OS_BOARD_CAP_STATUS_LED;
    if (value & SOLAR_OS_BOARD_CAP_STATUS_LED) *out |= SOLAR_OS_NATIVE_BOARD_CAP_STATUS_LED;
    known |= SOLAR_OS_BOARD_CAP_DISPLAY_BRIGHTNESS;
    if (value & SOLAR_OS_BOARD_CAP_DISPLAY_BRIGHTNESS) *out |= SOLAR_OS_NATIVE_BOARD_CAP_DISPLAY_BRIGHTNESS;
    known |= SOLAR_OS_BOARD_CAP_AUDIO_INPUT;
    if (value & SOLAR_OS_BOARD_CAP_AUDIO_INPUT) *out |= SOLAR_OS_NATIVE_BOARD_CAP_AUDIO_INPUT;
    known |= SOLAR_OS_BOARD_CAP_EXPANSION_GPIO;
    if (value & SOLAR_OS_BOARD_CAP_EXPANSION_GPIO) *out |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_GPIO;
    known |= SOLAR_OS_BOARD_CAP_EXPANSION_I2C;
    if (value & SOLAR_OS_BOARD_CAP_EXPANSION_I2C) *out |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_I2C;
    known |= SOLAR_OS_BOARD_CAP_EXPANSION_SPI;
    if (value & SOLAR_OS_BOARD_CAP_EXPANSION_SPI) *out |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_SPI;
    known |= SOLAR_OS_BOARD_CAP_EXPANSION_UART;
    if (value & SOLAR_OS_BOARD_CAP_EXPANSION_UART) *out |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_UART;
    known |= SOLAR_OS_BOARD_CAP_EXPANSION_ADC;
    if (value & SOLAR_OS_BOARD_CAP_EXPANSION_ADC) *out |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_ADC;
    known |= SOLAR_OS_BOARD_CAP_EXPANSION_PWM;
    if (value & SOLAR_OS_BOARD_CAP_EXPANSION_PWM) *out |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_PWM;
    known |= SOLAR_OS_BOARD_CAP_SIMD;
    if (value & SOLAR_OS_BOARD_CAP_SIMD) *out |= SOLAR_OS_NATIVE_BOARD_CAP_SIMD;
    known |= SOLAR_OS_BOARD_CAP_EXPANSION_I2S;
    if (value & SOLAR_OS_BOARD_CAP_EXPANSION_I2S) *out |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_I2S;
    known |= SOLAR_OS_BOARD_CAP_PS2_KEYBOARD;
    if (value & SOLAR_OS_BOARD_CAP_PS2_KEYBOARD) *out |= SOLAR_OS_NATIVE_BOARD_CAP_PS2_KEYBOARD;
    known |= SOLAR_OS_BOARD_CAP_POINTER;
    if (value & SOLAR_OS_BOARD_CAP_POINTER) *out |= SOLAR_OS_NATIVE_BOARD_CAP_POINTER;
    known |= SOLAR_OS_BOARD_CAP_STREAMING_DISPLAY;
    if (value & SOLAR_OS_BOARD_CAP_STREAMING_DISPLAY) *out |= SOLAR_OS_NATIVE_BOARD_CAP_STREAMING_DISPLAY;
    known |= SOLAR_OS_BOARD_CAP_CAMERA;
    if (value & SOLAR_OS_BOARD_CAP_CAMERA) *out |= SOLAR_OS_NATIVE_BOARD_CAP_CAMERA;
    known |= SOLAR_OS_BOARD_CAP_USB_HOST;
    if (value & SOLAR_OS_BOARD_CAP_USB_HOST) *out |= SOLAR_OS_NATIVE_BOARD_CAP_USB_HOST;
    known |= SOLAR_OS_BOARD_CAP_QEMU;
    if (value & SOLAR_OS_BOARD_CAP_QEMU) *out |= SOLAR_OS_NATIVE_BOARD_CAP_QEMU;
    return (value & ~known) ? SOLAR_OS_NATIVE_ERROR_INVALID_STATE : SOLAR_OS_NATIVE_OK;
}
static int board_capabilities_in(uint64_t value, uint64_t *out)
{
    uint64_t known = 0; *out = 0;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_DISPLAY;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_DISPLAY) *out |= SOLAR_OS_BOARD_CAP_DISPLAY;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_GFX;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_GFX) *out |= SOLAR_OS_BOARD_CAP_GFX;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_CDC;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_CDC) *out |= SOLAR_OS_BOARD_CAP_CDC;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_UART;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_UART) *out |= SOLAR_OS_BOARD_CAP_UART;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_SD;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_SD) *out |= SOLAR_OS_BOARD_CAP_SD;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_I2C;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_I2C) *out |= SOLAR_OS_BOARD_CAP_I2C;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_RTC;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_RTC) *out |= SOLAR_OS_BOARD_CAP_RTC;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_BATTERY;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_BATTERY) *out |= SOLAR_OS_BOARD_CAP_BATTERY;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_AUDIO;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_AUDIO) *out |= SOLAR_OS_BOARD_CAP_AUDIO;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_WIFI;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_WIFI) *out |= SOLAR_OS_BOARD_CAP_WIFI;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_BLE;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_BLE) *out |= SOLAR_OS_BOARD_CAP_BLE;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_GPIO;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_GPIO) *out |= SOLAR_OS_BOARD_CAP_GPIO;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_ADC;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_ADC) *out |= SOLAR_OS_BOARD_CAP_ADC;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_PWM;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_PWM) *out |= SOLAR_OS_BOARD_CAP_PWM;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_KEY;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_KEY) *out |= SOLAR_OS_BOARD_CAP_KEY;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_TEMPERATURE;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_TEMPERATURE) *out |= SOLAR_OS_BOARD_CAP_TEMPERATURE;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_HUMIDITY;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_HUMIDITY) *out |= SOLAR_OS_BOARD_CAP_HUMIDITY;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_PSRAM;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_PSRAM) *out |= SOLAR_OS_BOARD_CAP_PSRAM;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_BUTTONS;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_BUTTONS) *out |= SOLAR_OS_BOARD_CAP_BUTTONS;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_JOYSTICK;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_JOYSTICK) *out |= SOLAR_OS_BOARD_CAP_JOYSTICK;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_ADC_DPAD;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_ADC_DPAD) *out |= SOLAR_OS_BOARD_CAP_ADC_DPAD;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_SPI;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_SPI) *out |= SOLAR_OS_BOARD_CAP_SPI;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_STATUS_LED;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_STATUS_LED) *out |= SOLAR_OS_BOARD_CAP_STATUS_LED;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_DISPLAY_BRIGHTNESS;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_DISPLAY_BRIGHTNESS) *out |= SOLAR_OS_BOARD_CAP_DISPLAY_BRIGHTNESS;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_AUDIO_INPUT;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_AUDIO_INPUT) *out |= SOLAR_OS_BOARD_CAP_AUDIO_INPUT;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_GPIO;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_GPIO) *out |= SOLAR_OS_BOARD_CAP_EXPANSION_GPIO;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_I2C;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_I2C) *out |= SOLAR_OS_BOARD_CAP_EXPANSION_I2C;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_SPI;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_SPI) *out |= SOLAR_OS_BOARD_CAP_EXPANSION_SPI;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_UART;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_UART) *out |= SOLAR_OS_BOARD_CAP_EXPANSION_UART;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_ADC;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_ADC) *out |= SOLAR_OS_BOARD_CAP_EXPANSION_ADC;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_PWM;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_PWM) *out |= SOLAR_OS_BOARD_CAP_EXPANSION_PWM;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_SIMD;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_SIMD) *out |= SOLAR_OS_BOARD_CAP_SIMD;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_I2S;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_EXPANSION_I2S) *out |= SOLAR_OS_BOARD_CAP_EXPANSION_I2S;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_PS2_KEYBOARD;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_PS2_KEYBOARD) *out |= SOLAR_OS_BOARD_CAP_PS2_KEYBOARD;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_POINTER;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_POINTER) *out |= SOLAR_OS_BOARD_CAP_POINTER;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_STREAMING_DISPLAY;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_STREAMING_DISPLAY) *out |= SOLAR_OS_BOARD_CAP_STREAMING_DISPLAY;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_CAMERA;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_CAMERA) *out |= SOLAR_OS_BOARD_CAP_CAMERA;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_USB_HOST;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_USB_HOST) *out |= SOLAR_OS_BOARD_CAP_USB_HOST;
    known |= SOLAR_OS_NATIVE_BOARD_CAP_QEMU;
    if (value & SOLAR_OS_NATIVE_BOARD_CAP_QEMU) *out |= SOLAR_OS_BOARD_CAP_QEMU;
    return (value & ~known) ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : SOLAR_OS_NATIVE_OK;
}
static int board_info(solar_os_native_board_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_board_info_t value = solar_os_board_info();
    int result = copy_text(out->id, sizeof(out->id), value.id);
    if (!result) result = copy_text(out->name, sizeof(out->name), value.name);
    if (!result) result = copy_text(out->module, sizeof(out->module), value.module);
    if (!result) result = board_capabilities_out(value.capabilities, &out->capabilities);
    out->psram_bytes = value.psram_bytes; return result;
}
static int board_capabilities(uint64_t *out)
{ return out ? board_capabilities_out(solar_os_board_capabilities(), out) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; }
static bool board_has(uint64_t in)
{ uint64_t value; return !board_capabilities_in(in, &value) && solar_os_board_has(value); }
static const char *board_capability_name(uint64_t in)
{ uint64_t value; return board_capabilities_in(in, &value) ? NULL : solar_os_board_capability_name(value); }
static int board_capabilities_format(char *out, size_t capacity)
{
    if (!out || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_board_capabilities_format(out, capacity) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
}
static int backlight_status(solar_os_native_backlight_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_backlight_status_t value = {0};
    int result = solar_os_native_result(solar_os_backlight_get_status(&value)); if (result) return result;
    out->follow_display = value.follow_display; out->suspended = value.suspended;
    out->percent = value.percent; out->effective_percent = value.effective_percent;
    out->providers = value.providers; out->last_error = solar_os_native_result(value.last_error); return SOLAR_OS_NATIVE_OK;
}
static int backlight_set_brightness(uint32_t value)
{ return value > 100 ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_backlight_set_brightness((uint8_t)value)); }
static int backlight_display_changed(uint32_t value)
{ return value > 100 ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_backlight_display_changed((uint8_t)value)); }
static int backlight_set_follow(void) { return solar_os_native_result(solar_os_backlight_set_follow()); }
static int backlight_toggle(void) { return solar_os_native_result(solar_os_backlight_toggle()); }
static int backlight_set_suspended(bool suspended) { return solar_os_native_result(solar_os_backlight_set_suspended(suspended)); }
static int profile_in(uint32_t value, solar_os_power_profile_t *out)
{
    switch (value) {
    case SOLAR_OS_NATIVE_POWER_PERFORMANCE: *out = SOLAR_OS_POWER_PROFILE_PERFORMANCE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_BALANCED: *out = SOLAR_OS_POWER_PROFILE_BALANCED; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_BATTERY: *out = SOLAR_OS_POWER_PROFILE_BATTERY; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_LOWPOWER: *out = SOLAR_OS_POWER_PROFILE_LOWPOWER; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int profile_out(solar_os_power_profile_t value, uint32_t *out)
{
    switch (value) {
    case SOLAR_OS_POWER_PROFILE_PERFORMANCE: *out = SOLAR_OS_NATIVE_POWER_PERFORMANCE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_PROFILE_BALANCED: *out = SOLAR_OS_NATIVE_POWER_BALANCED; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_PROFILE_BATTERY: *out = SOLAR_OS_NATIVE_POWER_BATTERY; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_PROFILE_LOWPOWER: *out = SOLAR_OS_NATIVE_POWER_LOWPOWER; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
}
static int key_action_in(uint32_t value, solar_os_power_key_action_t *out)
{
    switch (value) {
    case SOLAR_OS_NATIVE_POWER_KEY_OFF: *out = SOLAR_OS_POWER_KEY_ACTION_OFF; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_SLEEP: *out = SOLAR_OS_POWER_KEY_ACTION_SLEEP; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_SUSPEND: *out = SOLAR_OS_POWER_KEY_ACTION_SUSPEND; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_DEEPSLEEP: *out = SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_BLE_PAIR: *out = SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_ENTER: *out = SOLAR_OS_POWER_KEY_ACTION_ENTER; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_APP_EXIT: *out = SOLAR_OS_POWER_KEY_ACTION_APP_EXIT; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_DEFAULT: *out = SOLAR_OS_POWER_KEY_ACTION_DEFAULT; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_POWER_KEY_SHUTDOWN: *out = SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int key_action_out(solar_os_power_key_action_t value, uint32_t *out)
{
    switch (value) {
    case SOLAR_OS_POWER_KEY_ACTION_OFF: *out = SOLAR_OS_NATIVE_POWER_KEY_OFF; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_SLEEP: *out = SOLAR_OS_NATIVE_POWER_KEY_SLEEP; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_SUSPEND: *out = SOLAR_OS_NATIVE_POWER_KEY_SUSPEND; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP: *out = SOLAR_OS_NATIVE_POWER_KEY_DEEPSLEEP; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR: *out = SOLAR_OS_NATIVE_POWER_KEY_BLE_PAIR; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_ENTER: *out = SOLAR_OS_NATIVE_POWER_KEY_ENTER; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_APP_EXIT: *out = SOLAR_OS_NATIVE_POWER_KEY_APP_EXIT; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_DEFAULT: *out = SOLAR_OS_NATIVE_POWER_KEY_DEFAULT; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN: *out = SOLAR_OS_NATIVE_POWER_KEY_SHUTDOWN; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
}
static int shutdown_stage_in(uint32_t value, solar_os_shutdown_stage_t *out)
{
    switch (value) {
    case SOLAR_OS_NATIVE_SHUTDOWN_WAKE: *out = SOLAR_OS_SHUTDOWN_WAKE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_PORTS: *out = SOLAR_OS_SHUTDOWN_PORTS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_APPS: *out = SOLAR_OS_SHUTDOWN_APPS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_JOBS: *out = SOLAR_OS_SHUTDOWN_JOBS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_RADIOS: *out = SOLAR_OS_SHUTDOWN_RADIOS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_STORAGE: *out = SOLAR_OS_SHUTDOWN_STORAGE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_DISPLAY: *out = SOLAR_OS_SHUTDOWN_DISPLAY; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_POWER: *out = SOLAR_OS_SHUTDOWN_POWER; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_SHUTDOWN_COMPLETE: *out = SOLAR_OS_SHUTDOWN_COMPLETE; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int shutdown_stage_out(solar_os_shutdown_stage_t value, uint32_t *out)
{
    switch (value) {
    case SOLAR_OS_SHUTDOWN_WAKE: *out = SOLAR_OS_NATIVE_SHUTDOWN_WAKE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_PORTS: *out = SOLAR_OS_NATIVE_SHUTDOWN_PORTS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_APPS: *out = SOLAR_OS_NATIVE_SHUTDOWN_APPS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_JOBS: *out = SOLAR_OS_NATIVE_SHUTDOWN_JOBS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_RADIOS: *out = SOLAR_OS_NATIVE_SHUTDOWN_RADIOS; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_STORAGE: *out = SOLAR_OS_NATIVE_SHUTDOWN_STORAGE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_DISPLAY: *out = SOLAR_OS_NATIVE_SHUTDOWN_DISPLAY; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_POWER: *out = SOLAR_OS_NATIVE_SHUTDOWN_POWER; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_SHUTDOWN_COMPLETE: *out = SOLAR_OS_NATIVE_SHUTDOWN_COMPLETE; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
}
static uint32_t wakeup_out(int value)
{
    switch (value) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return SOLAR_OS_NATIVE_WAKE_UNDEFINED;
    case ESP_SLEEP_WAKEUP_EXT0: return SOLAR_OS_NATIVE_WAKE_EXT0;
    case ESP_SLEEP_WAKEUP_EXT1: return SOLAR_OS_NATIVE_WAKE_EXT1;
    case ESP_SLEEP_WAKEUP_TIMER: return SOLAR_OS_NATIVE_WAKE_TIMER;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return SOLAR_OS_NATIVE_WAKE_TOUCHPAD;
    case ESP_SLEEP_WAKEUP_ULP: return SOLAR_OS_NATIVE_WAKE_ULP;
    case ESP_SLEEP_WAKEUP_GPIO: return SOLAR_OS_NATIVE_WAKE_GPIO;
    case ESP_SLEEP_WAKEUP_UART: return SOLAR_OS_NATIVE_WAKE_UART;
    case ESP_SLEEP_WAKEUP_UART1: return SOLAR_OS_NATIVE_WAKE_UART1;
    case ESP_SLEEP_WAKEUP_UART2: return SOLAR_OS_NATIVE_WAKE_UART2;
    case ESP_SLEEP_WAKEUP_WIFI: return SOLAR_OS_NATIVE_WAKE_WIFI;
    case ESP_SLEEP_WAKEUP_COCPU: return SOLAR_OS_NATIVE_WAKE_COCPU;
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG: return SOLAR_OS_NATIVE_WAKE_COCPU_TRAP;
    case ESP_SLEEP_WAKEUP_BT: return SOLAR_OS_NATIVE_WAKE_BT;
    case ESP_SLEEP_WAKEUP_VAD: return SOLAR_OS_NATIVE_WAKE_VAD;
    case ESP_SLEEP_WAKEUP_VBAT_UNDER_VOLT: return SOLAR_OS_NATIVE_WAKE_VBAT_UNDER_VOLT;
    case ESP_SLEEP_WAKEUP_USB: return SOLAR_OS_NATIVE_WAKE_USB;
    default: return SOLAR_OS_NATIVE_WAKE_UNKNOWN;
    }
}
static int power_status(solar_os_native_power_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_power_status_t value = {0}; solar_os_power_get_status(&value);
    int result = profile_out(value.profile, &out->profile);
    if (!result) result = profile_out(value.effective_profile, &out->effective_profile);
    if (!result) result = key_action_out(value.key_action, &out->key_action);
    if (!result) result = key_action_out(value.key_long_action, &out->key_long_action);
    out->initialized = value.initialized;
    out->idle_sleep_ms = value.idle_sleep_ms;
    out->cpu_min_mhz = value.cpu_min_mhz;
    out->cpu_max_mhz = value.cpu_max_mhz;
    out->automatic_light_sleep = value.automatic_light_sleep;
    out->explicit_sleep_active = value.explicit_sleep_active;
    out->suspend_active = value.suspend_active;
    out->automatic_light_sleep_holdoff_ms = value.automatic_light_sleep_holdoff_ms;
    out->pm_configured = value.pm_configured;
    out->bt_sleep_enabled = value.bt_sleep_enabled;
    out->light_sleep_count = value.light_sleep_count;
    out->last_activity_ms = value.last_activity_ms;
    out->last_sleep_duration_ms = value.last_sleep_duration_ms;
    out->last_wakeup_ext1 = value.last_wakeup_ext1;
    out->pm_last_error = solar_os_native_result(value.pm_last_error);
    out->bt_sleep_last_error = solar_os_native_result(value.bt_sleep_last_error);
    out->last_wakeup_cause = wakeup_out(value.last_wakeup_cause); return result;
}
static int power_set_profile(uint32_t value)
{ solar_os_power_profile_t profile; int result = profile_in(value, &profile); return result ? result : solar_os_native_result(solar_os_power_set_profile(profile)); }
static int power_set_key_action(uint32_t value)
{ solar_os_power_key_action_t action; int result = key_action_in(value, &action); return result ? result : solar_os_native_result(solar_os_power_set_key_action(action)); }
static int power_set_key_long_action(uint32_t value)
{ solar_os_power_key_action_t action; int result = key_action_in(value, &action); return result ? result : solar_os_native_result(solar_os_power_set_key_long_action(action)); }
static int power_set_idle_sleep_ms(uint32_t value) { return solar_os_native_result(solar_os_power_set_idle_sleep_ms(value)); }
static int power_hold_automatic_light_sleep(uint32_t value) { return solar_os_native_result(solar_os_power_hold_automatic_light_sleep(value)); }
static int power_off(void) { return solar_os_native_result(solar_os_power_off()); }
static int power_request_shutdown(void) { return solar_os_native_result(solar_os_shutdown_request()); }
static int power_shutdown_status(solar_os_native_shutdown_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); out->requested = solar_os_shutdown_requested(); out->error = solar_os_native_result(solar_os_shutdown_error());
    return shutdown_stage_out(solar_os_shutdown_stage(), &out->stage);
}
static const char *power_profile_name(uint32_t value)
{ solar_os_power_profile_t profile; return profile_in(value, &profile) ? NULL : solar_os_power_profile_name(profile); }
static const char *power_key_action_name(uint32_t value)
{ solar_os_power_key_action_t action; return key_action_in(value, &action) ? NULL : solar_os_power_key_action_name(action); }
static const char *power_shutdown_stage_name(uint32_t value)
{ solar_os_shutdown_stage_t stage; return shutdown_stage_in(value, &stage) ? NULL : solar_os_shutdown_stage_name(stage); }
static int power_parse_profile(const char *text, uint32_t *out)
{
    if (!text || !out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; solar_os_power_profile_t value;
    return solar_os_power_parse_profile(text, &value) ? profile_out(value, out) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
static int power_parse_key_action(const char *text, uint32_t *out)
{
    if (!text || !out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; solar_os_power_key_action_t value;
    return solar_os_power_parse_key_action(text, &value) ? key_action_out(value, out) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
static int identity_get(solar_os_identity_v1_t *identity)
{
    if (!identity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_identity_get(identity);
    return SOLAR_OS_NATIVE_OK;
}

static int identity_set(solar_os_identity_field_t field, const char *value)
{
    return solar_os_native_result(solar_os_identity_set(field, value));
}

static const solar_os_native_identity_api_v1_t identity_api = {
    .abi_version = SOLAR_OS_NATIVE_IDENTITY_ABI,
    .struct_size = sizeof(identity_api),
    .get = identity_get,
    .set = identity_set,
    .format = solar_os_identity_format,
};

static const solar_os_native_board_api_v1_t board_api = {
    .abi_version = SOLAR_OS_NATIVE_BOARD_ABI, .struct_size = sizeof(board_api),
    .info = board_info,
    .capabilities = board_capabilities,
    .has = board_has,
    .capability_name = board_capability_name,
    .capabilities_format = board_capabilities_format,
};
static const solar_os_native_backlight_api_v1_t backlight_api = {
    .abi_version = SOLAR_OS_NATIVE_BACKLIGHT_ABI, .struct_size = sizeof(backlight_api),
    .register_provider = solar_os_native_backlight_provider_register, .close_provider = solar_os_native_backlight_provider_close,
    .status = backlight_status,
    .set_brightness = backlight_set_brightness,
    .set_follow = backlight_set_follow,
    .toggle = backlight_toggle,
    .display_changed = backlight_display_changed,
    .set_suspended = backlight_set_suspended,
};
static const solar_os_native_power_api_v1_t power_api = {
    .abi_version = SOLAR_OS_NATIVE_POWER_ABI, .struct_size = sizeof(power_api),
    .status = power_status,
    .set_profile = power_set_profile,
    .set_idle_sleep_ms = power_set_idle_sleep_ms,
    .set_key_action = power_set_key_action,
    .set_key_long_action = power_set_key_long_action,
    .hold_automatic_light_sleep = power_hold_automatic_light_sleep,
    .off = power_off,
    .request_shutdown = power_request_shutdown,
    .shutdown_status = power_shutdown_status,
    .profile_name = power_profile_name,
    .parse_profile = power_parse_profile,
    .key_action_name = power_key_action_name,
    .parse_key_action = power_parse_key_action,
    .shutdown_stage_name = power_shutdown_stage_name,
    .note_activity = solar_os_power_note_activity,
    .should_idle_sleep = solar_os_power_should_idle_sleep,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_status_led.h"
static int status_led_set(bool on)
{ return solar_os_native_result(solar_os_status_led_set(on)); }
static int status_led_toggle(bool * out)
{ return solar_os_native_result(solar_os_status_led_toggle(out)); }
static int status_led_get(bool * out)
{ return solar_os_native_result(solar_os_status_led_get(out)); }
static const solar_os_native_status_led_api_v1_t status_led_api = {
    .abi_version = SOLAR_OS_NATIVE_STATUS_LED_ABI, .struct_size = sizeof(status_led_api),
    .set = status_led_set,
    .toggle = status_led_toggle,
    .get = status_led_get,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_BATTERY
#include "solar_os_battery.h"
static int battery_trend_in(uint32_t value, solar_os_battery_trend_t *out)
{
    switch (value) {
    case SOLAR_OS_NATIVE_BATTERY_UNKNOWN: *out = SOLAR_OS_BATTERY_TREND_UNKNOWN; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_BATTERY_DISCHARGING: *out = SOLAR_OS_BATTERY_TREND_DISCHARGING; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_BATTERY_FLAT: *out = SOLAR_OS_BATTERY_TREND_FLAT; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_BATTERY_CHARGING: *out = SOLAR_OS_BATTERY_TREND_CHARGING; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int battery_trend_out(solar_os_battery_trend_t value, uint32_t *out)
{
    switch (value) {
    case SOLAR_OS_BATTERY_TREND_UNKNOWN: *out = SOLAR_OS_NATIVE_BATTERY_UNKNOWN; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_BATTERY_TREND_DISCHARGING: *out = SOLAR_OS_NATIVE_BATTERY_DISCHARGING; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_BATTERY_TREND_FLAT: *out = SOLAR_OS_NATIVE_BATTERY_FLAT; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_BATTERY_TREND_CHARGING: *out = SOLAR_OS_NATIVE_BATTERY_CHARGING; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
}
static int battery_status(solar_os_native_battery_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_battery_status_t value = {0};
    int result = solar_os_native_result(solar_os_battery_get_status(&value)); if (result) return result;
    out->voltage_mv = value.voltage_mv;
    out->percent = value.percent;
    out->percent_estimated = value.percent_estimated;
    out->adc_calibrated = value.adc_calibrated;
    out->external_power = value.external_power;
    out->charging = value.charging;
    out->charging_known = value.charging_known;
    out->external_power_known = value.external_power_known;
    return SOLAR_OS_NATIVE_OK;
}
static int battery_config(solar_os_native_battery_config_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_battery_config_t value = {0}; solar_os_battery_get_config(&value);
    out->capacity_mah = value.capacity_mah; out->min_voltage_mv = value.min_voltage_mv;
    out->max_voltage_mv = value.max_voltage_mv; out->runtime_min = value.runtime_min; return SOLAR_OS_NATIVE_OK;
}
static void battery_window(const solar_os_battery_window_status_t *value, solar_os_native_battery_window_v1_t *out)
{
    out->valid = value->valid; out->span_ms = value->span_ms; out->voltage_drop_mvh = value->voltage_drop_mvh;
    out->current_valid = value->current_valid; out->current_ma = value->current_ma;
}
static int battery_monitor_status(solar_os_native_battery_monitor_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_battery_monitor_status_t value = {0}; solar_os_battery_monitor_get_status(&value);
    out->running = value.running;
    out->interval_ms = value.interval_ms;
    out->sample_count = value.sample_count;
    out->last_sample_ms = value.last_sample_ms;
    out->last_voltage_mv = value.last_voltage_mv;
    out->last_percent = value.last_percent;
    out->adc_calibrated = value.adc_calibrated;
    out->external_power = value.external_power;
    out->slope_mvh = value.slope_mvh;
    out->time_left_min = value.time_left_min;
    out->time_left_valid = value.time_left_valid;
    out->external_power_known = value.external_power_known;
    out->time_left_seeded = value.time_left_seeded;
    out->full_runtime_min = value.full_runtime_min;
    out->last_error = solar_os_native_result(value.last_error);
    battery_window(&value.short_window, &out->short_window); battery_window(&value.long_window, &out->long_window);
    return battery_trend_out(value.trend, &out->trend);
}
static const char *battery_trend_name(uint32_t value)
{ solar_os_battery_trend_t trend; return battery_trend_in(value, &trend) ? NULL : solar_os_battery_trend_name(trend); }
static int battery_set_capacity_mah(uint32_t value)
{ return solar_os_native_result(solar_os_battery_set_capacity_mah(value)); }
static int battery_set_runtime_min(uint32_t value)
{ return solar_os_native_result(solar_os_battery_set_runtime_min(value)); }
static int battery_monitor_start(uint32_t value)
{ return solar_os_native_result(solar_os_battery_monitor_start(value)); }
static int battery_monitor_sample(uint32_t value)
{ return solar_os_native_result(solar_os_battery_monitor_sample(value)); }
static int battery_set_min_voltage_mv(uint32_t value)
{ return value > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_battery_set_min_voltage_mv((uint16_t)value)); }
static int battery_set_max_voltage_mv(uint32_t value)
{ return value > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_battery_set_max_voltage_mv((uint16_t)value)); }
static const solar_os_native_battery_api_v1_t battery_api = {
    .abi_version = SOLAR_OS_NATIVE_BATTERY_ABI, .struct_size = sizeof(battery_api),
    .status = battery_status,
    .config = battery_config,
    .set_capacity_mah = battery_set_capacity_mah,
    .set_min_voltage_mv = battery_set_min_voltage_mv,
    .set_max_voltage_mv = battery_set_max_voltage_mv,
    .set_runtime_min = battery_set_runtime_min,
    .monitor_start = battery_monitor_start,
    .monitor_sample = battery_monitor_sample,
    .monitor_status = battery_monitor_status,
    .trend_name = battery_trend_name,
    .has_provider = solar_os_battery_has_provider,
    .monitor_stop = solar_os_battery_monitor_stop,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CHARGER
#include "solar_os_charger.h"
static int charger_state_in(uint32_t value, solar_os_charger_state_t *out)
{
    switch (value) {
    case SOLAR_OS_NATIVE_CHARGER_UNKNOWN: *out = SOLAR_OS_CHARGER_STATE_UNKNOWN; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_CHARGER_NOT_CHARGING: *out = SOLAR_OS_CHARGER_STATE_NOT_CHARGING; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_CHARGER_PRECHARGE: *out = SOLAR_OS_CHARGER_STATE_PRECHARGE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_CHARGER_FAST_CHARGE: *out = SOLAR_OS_CHARGER_STATE_FAST_CHARGE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_NATIVE_CHARGER_DONE: *out = SOLAR_OS_CHARGER_STATE_DONE; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int charger_state_out(solar_os_charger_state_t value, uint32_t *out)
{
    switch (value) {
    case SOLAR_OS_CHARGER_STATE_UNKNOWN: *out = SOLAR_OS_NATIVE_CHARGER_UNKNOWN; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_CHARGER_STATE_NOT_CHARGING: *out = SOLAR_OS_NATIVE_CHARGER_NOT_CHARGING; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_CHARGER_STATE_PRECHARGE: *out = SOLAR_OS_NATIVE_CHARGER_PRECHARGE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_CHARGER_STATE_FAST_CHARGE: *out = SOLAR_OS_NATIVE_CHARGER_FAST_CHARGE; return SOLAR_OS_NATIVE_OK;
    case SOLAR_OS_CHARGER_STATE_DONE: *out = SOLAR_OS_NATIVE_CHARGER_DONE; return SOLAR_OS_NATIVE_OK;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
}
static void charger_range(const solar_os_charger_range_t *value, solar_os_native_charger_range_v1_t *out)
{ out->minimum = value->minimum; out->maximum = value->maximum; out->step = value->step; }
static int charger_info(size_t index, solar_os_native_charger_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_charger_info_t value = {0};
    if (!solar_os_charger_get(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    _Static_assert(sizeof(out->name) == sizeof(value.name), "charger name capacity");
    _Static_assert(sizeof(out->driver) == sizeof(value.driver), "charger driver capacity");
    memcpy(out->name, value.name, sizeof(out->name)); memcpy(out->driver, value.driver, sizeof(out->driver));
    charger_range(&value.input_current_limit_ma, &out->input_current_limit_ma);
    charger_range(&value.charge_current_ma, &out->charge_current_ma);
    charger_range(&value.charge_voltage_mv, &out->charge_voltage_mv); return SOLAR_OS_NATIVE_OK;
}
static int charger_status(const char *name, solar_os_native_charger_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_charger_status_t value = {0};
    int result = solar_os_native_result(solar_os_charger_read_status(name, &value)); if (result) return result;
    out->enabled = value.enabled;
    out->input_present = value.input_present;
    out->power_good = value.power_good;
    out->input_current_limit_ma = value.input_current_limit_ma;
    out->charge_current_ma = value.charge_current_ma;
    out->charge_voltage_mv = value.charge_voltage_mv;
    out->fault = value.fault;
    return charger_state_out(value.state, &out->state);
}
static const char *charger_state_name(uint32_t value)
{ solar_os_charger_state_t state; return charger_state_in(value, &state) ? NULL : solar_os_charger_state_name(state); }
static int charger_set_enabled(const char *name, bool value)
{ return solar_os_native_result(solar_os_charger_set_enabled(name, value)); }
static int charger_set_input_current_limit(const char *name, uint32_t value)
{ return value > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_charger_set_input_current_limit(name, (uint16_t)value)); }
static int charger_set_charge_current(const char *name, uint32_t value)
{ return value > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_charger_set_charge_current(name, (uint16_t)value)); }
static int charger_set_charge_voltage(const char *name, uint32_t value)
{ return value > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_charger_set_charge_voltage(name, (uint16_t)value)); }
static const solar_os_native_charger_api_v1_t charger_api = {
    .abi_version = SOLAR_OS_NATIVE_CHARGER_ABI, .struct_size = sizeof(charger_api),
    .register_provider = solar_os_native_charger_provider_register, .close_provider = solar_os_native_charger_provider_close,
    .info = charger_info,
    .status = charger_status,
    .set_enabled = charger_set_enabled,
    .set_input_current_limit = charger_set_input_current_limit,
    .set_charge_current = charger_set_charge_current,
    .set_charge_voltage = charger_set_charge_voltage,
    .state_name = charger_state_name,
    .count = solar_os_charger_count,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_HAPTIC
#include "solar_os_haptic.h"
static int haptic_info(size_t index, solar_os_native_haptic_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_haptic_info_t value = {0};
    if (!solar_os_haptic_get(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    _Static_assert(sizeof(out->name) == sizeof(value.name), "haptic name capacity");
    _Static_assert(sizeof(out->driver) == sizeof(value.driver), "haptic driver capacity");
    memcpy(out->name, value.name, sizeof(out->name)); memcpy(out->driver, value.driver, sizeof(out->driver));
    out->effect_count = value.effect_count; return SOLAR_OS_NATIVE_OK;
}
static int haptic_play_effect(const char *name, uint32_t effect)
{ return effect > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_haptic_play_effect(name, (uint16_t)effect)); }
static int haptic_stop(const char *name) { return solar_os_native_result(solar_os_haptic_stop(name)); }
static const solar_os_native_haptic_api_v1_t haptic_api = {
    .abi_version = SOLAR_OS_NATIVE_HAPTIC_ABI, .struct_size = sizeof(haptic_api),
    .register_provider = solar_os_native_haptic_provider_register, .close_provider = solar_os_native_haptic_provider_close,
    .info = haptic_info,
    .play_effect = haptic_play_effect,
    .stop = haptic_stop,
    .count = solar_os_haptic_count,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RTC
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_rtc.h"
#include <stdatomic.h>
#include <stdio.h>

typedef struct {
    char owner[SOLAR_OS_RTC_OWNER_NAME_MAX];
    atomic_bool ready;
} native_rtc_lease_t;

static int rtc_info(solar_os_rtc_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *info = (solar_os_rtc_info_t){0};
    return solar_os_native_result(solar_os_rtc_get_info(info));
}

static int rtc_lease_destroy(void *opaque)
{
    native_rtc_lease_t *state = opaque;
    if (!atomic_load(&state->ready)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    const int result = solar_os_native_result(solar_os_rtc_release_owner_checked(state->owner));
    if (result) return result;
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int rtc_lease_open(const char *label, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!label || !*label) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_rtc_info_t info;
    int result = rtc_info(&info);
    if (result) return result;
    if (!(info.capabilities & (SOLAR_OS_RTC_CAP_ALARM | SOLAR_OS_RTC_CAP_COUNTDOWN)))
        return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;

    native_rtc_lease_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.rtc.lease");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    atomic_init(&state->ready, false);
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_RTC_LEASE;
    const solar_os_app_t *app = solar_os_application_descriptor();
    solar_os_native_handle_t handle;
    result = app ? solar_os_native_handle_create_owned(kind, state, rtc_lease_destroy, app, &handle) :
        solar_os_native_handle_create(kind, state, rtc_lease_destroy, &handle);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    snprintf(state->owner, sizeof(state->owner), "%08lx/%.14s", (unsigned long)handle, label);
    atomic_store(&state->ready, true);
    *out = handle;
    return SOLAR_OS_NATIVE_OK;
}

static int rtc_lease_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_RTC_LEASE);
}

static int rtc_program(solar_os_native_handle_t handle, const solar_os_rtc_timer_request_v1_t *request)
{
    if (!request) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_rtc_lease_t *state = solar_os_native_handle_acquire(handle, SOLAR_OS_NATIVE_HANDLE_RTC_LEASE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = atomic_load(&state->ready) ?
        solar_os_native_result(solar_os_rtc_program_timer(state->owner, request)) : SOLAR_OS_NATIVE_ERROR_BUSY;
    solar_os_native_handle_release(handle);
    return result;
}

static int rtc_cancel(solar_os_native_handle_t handle, solar_os_rtc_timer_kind_t kind)
{
    native_rtc_lease_t *state = solar_os_native_handle_acquire(handle, SOLAR_OS_NATIVE_HANDLE_RTC_LEASE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = atomic_load(&state->ready) ?
        solar_os_native_result(solar_os_rtc_cancel_timer(state->owner, kind)) : SOLAR_OS_NATIVE_ERROR_BUSY;
    solar_os_native_handle_release(handle);
    return result;
}

static int rtc_interrupt_status(uint32_t *pending)
{
    if (!pending) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *pending = 0;
    uint32_t bits = 0;
    const int result = solar_os_native_result(solar_os_rtc_get_interrupt_status(&bits));
    if (!result) *pending = bits;
    return result;
}

static int rtc_interrupt_clear(uint32_t pending)
{
    return solar_os_native_result(solar_os_rtc_clear_interrupt_status(pending));
}

static const solar_os_native_rtc_api_v1_t rtc_api = {
    .abi_version = SOLAR_OS_NATIVE_RTC_ABI,
    .struct_size = sizeof(rtc_api),
    .available = solar_os_rtc_has_provider,
    .info = rtc_info,
    .lease_open = rtc_lease_open,
    .program = rtc_program,
    .cancel = rtc_cancel,
    .lease_close = rtc_lease_close,
    .interrupt_status = rtc_interrupt_status,
    .interrupt_clear = rtc_interrupt_clear,
};
static const solar_os_native_rtc_provider_api_v1_t rtc_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_RTC_PROVIDER_ABI,
    .struct_size = sizeof(rtc_provider_api),
    .provider_register = solar_os_native_rtc_provider_register,
    .provider_close = solar_os_native_rtc_provider_close,
};
#endif

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
#include "solar_os_sensors.h"
static int sensors_environment(const char *name, uint32_t fields, solar_os_native_environment_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_native_environment_v1_t value = {0};
    esp_err_t error;
    switch (fields) {
    case SOLAR_OS_SENSOR_CAP_TEMPERATURE:
        error = solar_os_sensors_read_temperature(name, &value.temperature_c);
        break;
    case SOLAR_OS_SENSOR_CAP_HUMIDITY:
        error = solar_os_sensors_read_humidity(name, &value.humidity_percent);
        break;
    case SOLAR_OS_SENSOR_CAP_TEMPERATURE | SOLAR_OS_SENSOR_CAP_HUMIDITY:
        error = name ? solar_os_sensors_read_environment_from(name, &value) :
                       solar_os_sensors_read_environment(&value);
        break;
    default:
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    if (error == ESP_OK) *out = value;
    return solar_os_native_result(error);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
#include "solar_os_imu.h"
static int sensors_imu(const char *name, uint32_t timeout_ms, solar_os_native_imu_sample_v1_t *sample)
{ return solar_os_native_result(solar_os_imu_read_sample(name, timeout_ms, sample)); }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
#include "solar_os_gnss.h"
static int sensors_gnss_fix(const char *name, uint32_t timeout_ms, solar_os_native_gnss_fix_v1_t *fix)
{ return solar_os_native_result(solar_os_gnss_read_fix(name, timeout_ms, fix)); }
static int sensors_gnss_status(const char *name, uint32_t timeout_ms, solar_os_native_gnss_status_v1_t *status)
{ return solar_os_native_result(solar_os_gnss_get_status(name, timeout_ms, status)); }
static int sensors_gnss_set(const char *name, uint32_t property, bool enabled)
{
    if (property == SOLAR_OS_NATIVE_GNSS_POWER_CONTROL)
        return solar_os_native_result(solar_os_gnss_set_power(name, enabled));
    if (property == SOLAR_OS_NATIVE_GNSS_ENABLE_CONTROL)
        return solar_os_native_result(solar_os_gnss_set_enabled(name, enabled));
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
#endif

static size_t sensors_count(uint32_t kind)
{
    switch (kind) {
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    case SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT: return solar_os_sensors_count();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
    case SOLAR_OS_NATIVE_SENSOR_IMU: return solar_os_imu_count();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
    case SOLAR_OS_NATIVE_SENSOR_GNSS: return solar_os_gnss_count();
#endif
    default: return 0;
    }
}

static int sensors_info(uint32_t kind, size_t index, solar_os_native_sensor_info_v1_t *info)
{
    (void)index;
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    switch (kind) {
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    case SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT:
        if (solar_os_sensors_get(index, info)) return SOLAR_OS_NATIVE_OK;
        break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
    case SOLAR_OS_NATIVE_SENSOR_IMU:
        if (solar_os_imu_get(index, info)) return SOLAR_OS_NATIVE_OK;
        break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
    case SOLAR_OS_NATIVE_SENSOR_GNSS: {
        solar_os_gnss_info_t receiver;
        _Static_assert(sizeof(info->name) == sizeof(receiver.name) &&
                       sizeof(info->driver) == sizeof(receiver.driver), "GNSS metadata capacity matches the ABI");
        if (!solar_os_gnss_get(index, &receiver)) break;
        memcpy(info->name, receiver.name, sizeof(info->name));
        memcpy(info->driver, receiver.driver, sizeof(info->driver));
        info->capabilities = (receiver.power_control ? SOLAR_OS_NATIVE_GNSS_POWER_CONTROL : 0U) |
            (receiver.enable_control ? SOLAR_OS_NATIVE_GNSS_ENABLE_CONTROL : 0U);
        info->state = (receiver.powered ? SOLAR_OS_NATIVE_GNSS_POWERED : 0U) |
            (receiver.enabled ? SOLAR_OS_NATIVE_GNSS_ENABLED : 0U);
        return SOLAR_OS_NATIVE_OK;
    }
#endif
    default: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static const solar_os_native_sensors_api_v1_t sensors_api = {
    .abi_version = SOLAR_OS_NATIVE_SENSORS_ABI, .struct_size = sizeof(sensors_api),
    .count = sensors_count, .info = sensors_info,
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS
    .environment = sensors_environment,
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMU
    .imu = sensors_imu,
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GNSS
    .gnss_fix = sensors_gnss_fix, .gnss_status = sensors_gnss_status, .gnss_set = sensors_gnss_set,
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SENSORS || SOLAR_OS_PACKAGE_SERVICE_IMU || SOLAR_OS_PACKAGE_SERVICE_GNSS
    .provider_register = solar_os_native_sensor_provider_register,
    .provider_close = solar_os_native_sensor_provider_close,
    .provider_report = solar_os_native_sensor_provider_report,
#endif
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MODEM
#include "solar_os_modem.h"

static int modem_info(size_t index, solar_os_native_modem_info_v1_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_modem_get(index, info) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int modem_status(const char *name, solar_os_native_modem_status_v1_t *status)
{ return solar_os_native_result(solar_os_modem_get_status(name, status)); }
static int modem_set_active(const char *name, uint32_t target, bool active)
{
    if (target == SOLAR_OS_MODEM_SUPPLY)
        return solar_os_native_result(solar_os_modem_set_power(name, active));
    if (target == SOLAR_OS_MODEM_PACKET_DATA)
        return solar_os_native_result(solar_os_modem_set_data_active(name, active));
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
static int modem_reset(const char *name)
{ return solar_os_native_result(solar_os_modem_reset(name)); }
static int modem_profile_read(const char *name, solar_os_native_modem_profile_v1_t *profile)
{ return solar_os_native_result(solar_os_modem_profile_get(name, profile)); }
static int modem_profile_write(const char *name, const solar_os_native_modem_profile_v1_t *profile)
{ return solar_os_native_result(profile ? solar_os_modem_profile_set(name, profile) : solar_os_modem_profile_clear(name)); }
static int modem_profile_validate(const solar_os_native_modem_profile_v1_t *profile)
{ return solar_os_native_result(solar_os_modem_profile_validate(profile)); }
static int modem_rate_read(const char *name, solar_os_native_modem_transport_rate_info_v1_t *info)
{ return solar_os_native_result(solar_os_modem_transport_rate_get(name, info)); }
static int modem_rate_write(const char *name, uint32_t rate)
{ return solar_os_native_result(solar_os_modem_transport_rate_set(name, rate)); }
static int modem_unlock(const char *name, const char *pin)
{ return solar_os_native_result(solar_os_modem_unlock_sim(name, pin)); }
static int modem_command(const char *name, const char *command, uint32_t timeout_ms, char *response, size_t capacity)
{ return solar_os_native_result(solar_os_modem_command(name, command, timeout_ms, response, capacity)); }

static int modem_parse(uint32_t kind, const char *text, uint32_t *value)
{
    if (kind == SOLAR_OS_MODEM_LABEL_IP_TYPE && solar_os_modem_ip_type_parse(text, value)) return SOLAR_OS_NATIVE_OK;
    if (kind == SOLAR_OS_MODEM_LABEL_AUTH && solar_os_modem_auth_parse(text, value)) return SOLAR_OS_NATIVE_OK;
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
static const char *modem_label(uint32_t kind, uint32_t value)
{
    switch (kind) {
    case SOLAR_OS_MODEM_LABEL_IP_TYPE: return solar_os_modem_ip_type_name(value);
    case SOLAR_OS_MODEM_LABEL_AUTH: return solar_os_modem_auth_name(value);
    case SOLAR_OS_MODEM_LABEL_REGISTRATION: return solar_os_modem_registration_name(value);
    case SOLAR_OS_MODEM_LABEL_NETWORK_STATE: return solar_os_modem_network_state_name(value);
    default: return NULL;
    }
}
static const solar_os_native_modem_api_v1_t modem_api = {
    .abi_version = SOLAR_OS_NATIVE_MODEM_ABI, .struct_size = sizeof(modem_api),
    .count = solar_os_modem_count, .info = modem_info, .status = modem_status,
    .set_active = modem_set_active, .reset = modem_reset, .profile_read = modem_profile_read,
    .profile_write = modem_profile_write, .profile_validate = modem_profile_validate,
    .transport_rate_read = modem_rate_read, .transport_rate_write = modem_rate_write,
    .unlock_sim = modem_unlock, .command = modem_command, .parse = modem_parse, .label = modem_label,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_NFC
#include "solar_os_nfc.h"

static int nfc_set_power(const char *name, bool enabled)
{ return solar_os_native_result(solar_os_nfc_set_power(name, enabled)); }

static int nfc_scan(const char *name, uint32_t timeout_ms, solar_os_nfc_tag_t *tag)
{
    if (!tag) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *tag = (solar_os_nfc_tag_t){0};
    return solar_os_native_result(solar_os_nfc_scan(name, timeout_ms, tag));
}

static const solar_os_native_nfc_api_v1_t nfc_api = {
    .abi_version = SOLAR_OS_NATIVE_NFC_ABI, .struct_size = sizeof(nfc_api),
    .count = solar_os_nfc_count, .info = solar_os_nfc_get, .set_power = nfc_set_power, .scan = nfc_scan,
    .provider_register = solar_os_native_nfc_provider_register, .provider_close = solar_os_native_nfc_provider_close,
};
#endif

const void *solar_os_native_device_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_device_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
