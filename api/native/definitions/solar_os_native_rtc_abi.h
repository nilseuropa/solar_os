#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_rtc_types_v1.h"

#define SOLAR_OS_NATIVE_RTC_SERVICE "clock.rtc"
#define SOLAR_OS_NATIVE_RTC_ABI 1U

/* The RTC service handles hardware timers and interrupts. Use clock for
 * calendar operations. Alarm fields match UTC. Timers use the current RTC
 * provider. Each lease has
 * its own owner even when labels match. The alarm and countdown can have
 * different owners. Closing cancels the timers still owned by this lease.
 * A failed cancellation retains the handle for retry. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*available)(void);
    int (*info)(solar_os_rtc_info_t *info);
    int (*lease_open)(const char *label, solar_os_native_handle_t *lease);
    int (*program)(solar_os_native_handle_t lease, const solar_os_rtc_timer_request_v1_t *request);
    int (*cancel)(solar_os_native_handle_t lease, solar_os_rtc_timer_kind_t kind);
    int (*lease_close)(solar_os_native_handle_t lease);
    int (*interrupt_status)(uint32_t *pending);
    int (*interrupt_clear)(uint32_t pending);
} solar_os_native_rtc_api_v1_t;
