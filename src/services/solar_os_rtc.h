#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_time.h"
#include "solar_os_rtc_types_v1.h"

typedef esp_err_t (*solar_os_rtc_set_alarm_fn_t)(
    void *user,
    const solar_os_rtc_alarm_t *alarm);
typedef esp_err_t (*solar_os_rtc_disable_alarm_fn_t)(void *user);
typedef esp_err_t (*solar_os_rtc_set_countdown_fn_t)(
    void *user,
    uint32_t period_seconds,
    bool repeat);
typedef esp_err_t (*solar_os_rtc_disable_countdown_fn_t)(void *user);
typedef esp_err_t (*solar_os_rtc_get_interrupt_status_fn_t)(
    void *user,
    uint32_t *interrupts);
typedef esp_err_t (*solar_os_rtc_clear_interrupt_status_fn_t)(
    void *user,
    uint32_t interrupts);

typedef struct {
    solar_os_time_provider_get_fn_t get_utc_datetime;
    solar_os_time_provider_set_fn_t set_utc_datetime;
    solar_os_rtc_set_alarm_fn_t set_alarm;
    solar_os_rtc_disable_alarm_fn_t disable_alarm;
    solar_os_rtc_set_countdown_fn_t set_countdown;
    solar_os_rtc_disable_countdown_fn_t disable_countdown;
    solar_os_rtc_get_interrupt_status_fn_t get_interrupt_status;
    solar_os_rtc_clear_interrupt_status_fn_t clear_interrupt_status;
    void *user;
    int interrupt_gpio;
    int interrupt_active_level;
} solar_os_rtc_provider_t;

esp_err_t solar_os_rtc_register_provider(
    const char *owner,
    const solar_os_rtc_provider_t *provider);
esp_err_t solar_os_rtc_unregister_provider(const char *owner);
esp_err_t solar_os_rtc_unregister_provider_owned(const char *owner, const void *user);
bool solar_os_rtc_has_provider(void);
esp_err_t solar_os_rtc_get_info(solar_os_rtc_info_t *info);
esp_err_t solar_os_rtc_set_alarm_for(const char *owner,
                                     const solar_os_rtc_alarm_t *alarm);
esp_err_t solar_os_rtc_disable_alarm_for(const char *owner);
esp_err_t solar_os_rtc_set_countdown_for(const char *owner,
                                         uint32_t period_seconds,
                                         bool repeat);
esp_err_t solar_os_rtc_disable_countdown_for(const char *owner);
esp_err_t solar_os_rtc_program_timer(const char *owner, const solar_os_rtc_timer_request_v1_t *request);
esp_err_t solar_os_rtc_cancel_timer(const char *owner, solar_os_rtc_timer_kind_t kind);
/* Attempts both timers. A failed cancellation keeps ownership for retry. */
esp_err_t solar_os_rtc_release_owner_checked(const char *owner);
void solar_os_rtc_release_owner(const char *owner);
esp_err_t solar_os_rtc_set_alarm(const solar_os_rtc_alarm_t *alarm);
esp_err_t solar_os_rtc_disable_alarm(void);
esp_err_t solar_os_rtc_set_countdown(uint32_t period_seconds, bool repeat);
esp_err_t solar_os_rtc_disable_countdown(void);
esp_err_t solar_os_rtc_get_interrupt_status(uint32_t *interrupts);
esp_err_t solar_os_rtc_clear_interrupt_status(uint32_t interrupts);
