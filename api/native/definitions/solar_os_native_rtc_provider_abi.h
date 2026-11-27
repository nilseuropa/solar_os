#pragma once

#include "solar_os_native_rtc_abi.h"
#include "solar_os_native_clock_abi.h"

#define SOLAR_OS_NATIVE_RTC_PROVIDER_SERVICE "clock.rtc.provider"
#define SOLAR_OS_NATIVE_RTC_PROVIDER_ABI 1U

typedef struct {
    solar_os_native_clock_provider_v1_t calendar;
    uint32_t capabilities;
    int32_t interrupt_gpio, interrupt_active_level;
    int (*program_timer)(void *user, const solar_os_rtc_timer_request_v1_t *request);
    int (*cancel_timer)(void *user, solar_os_rtc_timer_kind_t kind);
    int (*interrupt_status)(void *user, uint32_t *pending);
    int (*interrupt_clear)(void *user, uint32_t pending);
} solar_os_native_rtc_provider_v1_t;

/* A hardware provider supplies the system calendar and optional RTC timers.
 * All callbacks use calendar.user. The host retains calendar.library_lease.
 * Advertise exactly the supported capabilities, including CALENDAR. The
 * timer and interrupt callback pairs must agree with their capability bits.
 * Registration clears old alarms and interrupts before publishing the
 * provider. Close retires timer state and the calendar before releasing
 * the code lease. Keep user state alive until close succeeds. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*provider_register)(const solar_os_native_rtc_provider_v1_t *provider,
                             solar_os_native_handle_t *registration);
    int (*provider_close)(solar_os_native_handle_t registration);
} solar_os_native_rtc_provider_api_v1_t;
