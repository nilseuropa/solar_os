#include "solar_os_native_input_hardware.h"
#include "solar_os_config.h"
#include "solar_os_native_support.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#if SOLAR_OS_PACKAGE_SERVICE_BUTTONS
#include "solar_os_buttons.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
#include "solar_os_adc_dpad.h"
#endif

static size_t input_button_count(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_BUTTONS
    return solar_os_buttons_count();
#else
    return 0;
#endif
}

static int input_button_at(size_t index, solar_os_native_button_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_button_status_v1_t){0};
#if SOLAR_OS_PACKAGE_SERVICE_BUTTONS
    solar_os_button_debug_info_t state;
    if (!solar_os_buttons_debug_get(index, &state)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (state.name && strlen(state.name) >= sizeof(out->name)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    *out = (solar_os_native_button_status_v1_t){
        .pin = state.pin,
        .key = state.key,
        .raw_pressed = state.raw_pressed,
        .stable_pressed = state.stable_pressed,
        .raw_transitions = state.raw_transitions,
        .stable_transitions = state.stable_transitions,
        .dropped_edges = state.dropped_edges,
    };
    if (state.name) strcpy(out->name, state.name);
    return SOLAR_OS_NATIVE_OK;
#else
    (void)index;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static size_t input_dpad_axis_count(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
    return solar_os_adc_dpad_axis_count();
#else
    return 0;
#endif
}

static int input_dpad_axis_at(size_t index, solar_os_native_dpad_axis_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_dpad_axis_status_v1_t){0};
#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
    _Static_assert(SOLAR_OS_NATIVE_DPAD_IDLE == SOLAR_OS_ADC_DPAD_ZONE_IDLE &&
                   SOLAR_OS_NATIVE_DPAD_MID == SOLAR_OS_ADC_DPAD_ZONE_MID &&
                   SOLAR_OS_NATIVE_DPAD_HIGH == SOLAR_OS_ADC_DPAD_ZONE_HIGH, "D-pad zone values");
    solar_os_adc_dpad_axis_status_t state;
    if (!solar_os_adc_dpad_get_axis_status(index, &state)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (state.name && strlen(state.name) >= sizeof(out->name)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    *out = (solar_os_native_dpad_axis_status_v1_t){
        .pin = state.pin,
        .raw = state.raw,
        .read_error = solar_os_native_result(state.read_error),
        .zone = state.zone,
        .mid_key = state.mid_key,
        .high_key = state.high_key,
        .idle_max = state.idle_max,
        .mid_min = state.mid_min,
        .mid_max = state.mid_max,
        .high_min = state.high_min,
        .initialized = state.initialized,
        .raw_valid = state.raw_valid,
    };
    if (state.name) strcpy(out->name, state.name);
    return SOLAR_OS_NATIVE_OK;
#else
    (void)index;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int input_dpad_calibrate(bool reset)
{
#if SOLAR_OS_PACKAGE_SERVICE_ADC_DPAD
    return solar_os_native_result(reset ? solar_os_adc_dpad_calibrate_reset() :
                                  solar_os_adc_dpad_calibrate_idle());
#else
    (void)reset;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static const solar_os_native_input_hardware_api_v1_t input_hardware_api = {
    .abi_version = SOLAR_OS_NATIVE_INPUT_HARDWARE_ABI,
    .struct_size = sizeof(input_hardware_api),
    .button_count = input_button_count,
    .button_at = input_button_at,
    .dpad_axis_count = input_dpad_axis_count,
    .dpad_axis_at = input_dpad_axis_at,
    .calibrate_dpad = input_dpad_calibrate,
};
#endif

const void *solar_os_native_input_hardware_get_service(const char *name, uint32_t abi,
                                                        uint32_t minimum)
{
#include "generated/native_input_hardware_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
