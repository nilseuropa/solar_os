#pragma once
#include "solar_os_native_device_abi.h"
#include "solar_os_native_clock_abi.h"
#include "solar_os_native_rtc_provider_abi.h"
#include "solar_os_native_radio_provider_abi.h"
#include "solar_os_native_io_abi.h"
#include "solar_os_native_sensors_abi.h"
#include "solar_os_native_camera_abi.h"
#include "solar_os_native_nfc_abi.h"
int solar_os_native_backlight_provider_register(const solar_os_native_backlight_provider_v1_t *provider, solar_os_native_handle_t *handle);
int solar_os_native_haptic_provider_register(const solar_os_native_haptic_provider_v1_t *provider, solar_os_native_handle_t *handle);
int solar_os_native_charger_provider_register(const solar_os_native_charger_provider_v1_t *provider, solar_os_native_handle_t *handle);
int solar_os_native_backlight_provider_close(solar_os_native_handle_t handle);
int solar_os_native_haptic_provider_close(solar_os_native_handle_t handle);
int solar_os_native_charger_provider_close(solar_os_native_handle_t handle);
int solar_os_native_clock_provider_register(const solar_os_native_clock_provider_v1_t *provider,
                                             solar_os_native_handle_t *handle);
int solar_os_native_clock_provider_close(solar_os_native_handle_t handle);
int solar_os_native_rtc_provider_register(const solar_os_native_rtc_provider_v1_t *provider,
                                           solar_os_native_handle_t *handle);
int solar_os_native_rtc_provider_close(solar_os_native_handle_t handle);
int solar_os_native_radio_provider_register(const solar_os_native_radio_provider_v1_t *provider,
                                             solar_os_native_handle_t *handle);
int solar_os_native_radio_provider_close(solar_os_native_handle_t handle);
int solar_os_native_gpio_controller_register(const solar_os_native_gpio_controller_provider_v1_t *provider,
                                            solar_os_native_handle_t *handle);
int solar_os_native_gpio_controller_close(solar_os_native_handle_t handle);
int solar_os_native_sensor_provider_register(const solar_os_native_sensor_provider_v1_t *provider,
                                             solar_os_native_handle_t *handle);
int solar_os_native_sensor_provider_close(solar_os_native_handle_t handle);
int solar_os_native_sensor_provider_report(solar_os_native_handle_t handle, uint32_t property, bool enabled);
int solar_os_native_camera_provider_register(const solar_os_native_camera_provider_v1_t *provider,
                                             solar_os_native_handle_t *handle);
int solar_os_native_camera_provider_close(solar_os_native_handle_t handle);
int solar_os_native_nfc_provider_register(const solar_os_native_nfc_provider_v1_t *provider,
                                         solar_os_native_handle_t *handle);
int solar_os_native_nfc_provider_close(solar_os_native_handle_t handle);
