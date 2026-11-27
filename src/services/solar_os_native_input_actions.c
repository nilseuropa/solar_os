#include "solar_os_native_input_actions.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_input_actions_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_input_actions.h"
#include "solar_os_input_action_match.h"
static int actions_limits(solar_os_native_input_action_limits_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_input_action_limits_v1_t){
        .maximum_bindings = SOLAR_OS_INPUT_ACTION_MAX_BINDINGS,
        .maximum_cooldown_ms = SOLAR_OS_INPUT_ACTION_COOLDOWN_MAX_MS,
        .default_cooldown_ms = SOLAR_OS_INPUT_ACTION_COOLDOWN_DEFAULT_MS,
    };
    return SOLAR_OS_NATIVE_OK;
}
static int actions_start(void)
{ return solar_os_native_result(solar_os_input_actions_start()); }
static int actions_stop(void)
{
    solar_os_input_actions_stop();
    return solar_os_input_actions_worker_active() ? SOLAR_OS_NATIVE_ERROR_BUSY : SOLAR_OS_NATIVE_OK;
}
static int actions_bind(const char *source, solar_os_input_gesture_t gesture, bool any_direction,
    solar_os_input_gesture_direction_t direction, uint32_t cooldown_ms, const char *command, uint32_t *out)
{
    if (out) *out = 0;
    return solar_os_native_result(solar_os_input_actions_bind(source, gesture, any_direction, direction, cooldown_ms, command, out));
}
static bool actions_get(size_t index, solar_os_input_action_binding_t *out)
{
    if (!out) return false;
    *out = (solar_os_input_action_binding_t){0};
    return solar_os_input_actions_get(index, out);
}
static int actions_unbind(uint32_t id)
{ return solar_os_native_result(solar_os_input_actions_unbind(id)); }
static int actions_emit_key(const char *name)
{ return solar_os_native_result(solar_os_input_actions_emit_key(name)); }
static const solar_os_native_input_actions_api_v1_t input_actions_api = {
    .abi_version = SOLAR_OS_NATIVE_INPUT_ACTIONS_ABI, .struct_size = sizeof(input_actions_api),
    .limits = actions_limits, .start = actions_start, .stop = actions_stop,
    .running = solar_os_input_actions_running, .worker_active = solar_os_input_actions_worker_active,
    .bind = actions_bind, .count = solar_os_input_actions_count, .get = actions_get,
    .unbind = actions_unbind, .clear = solar_os_input_actions_clear, .emit_key = actions_emit_key,
    .matches = solar_os_input_action_matches,
};
#endif
const void *solar_os_native_input_actions_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_input_actions_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
