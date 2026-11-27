#include "solar_os_native_controllers.h"
#include "solar_os_controllers.h"
#include "esp_timer.h"
#include <string.h>

static int result(esp_err_t e)
{
    switch(e) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}
static int open_controller(const char *name, const char *owner,
    const solar_os_native_controller_config_v1_t *c, uint32_t *id)
{ return result(solar_os_controller_open_owned(name,owner,c,id)); }
static int evaluate(uint32_t id, const solar_os_native_controller_sample_v1_t *s,
    solar_os_native_controller_output_v1_t *out)
{ return result(solar_os_controller_evaluate_owned(id,s,(uint64_t)esp_timer_get_time(),out)); }
static int pause_controller(uint32_t id)
{ return result(solar_os_controller_pause_owned(id)); }
static int fault_controller(uint32_t id, int error)
{
    esp_err_t e;
    switch(error) {
    case SOLAR_OS_NATIVE_OK: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: e=ESP_ERR_TIMEOUT; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: e=ESP_ERR_INVALID_ARG; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: e=ESP_ERR_NOT_FOUND; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: e=ESP_ERR_INVALID_STATE; break;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: e=ESP_ERR_NO_MEM; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: e=ESP_ERR_NOT_SUPPORTED; break;
    default: e=ESP_FAIL; break;
    }
    return result(solar_os_controller_fault_owned(id,e));
}
static int close_controller(uint32_t id)
{ return result(solar_os_controller_close_owned(id)); }
static int open_bound(const char *name,const char *owner,const solar_os_native_bound_controller_config_v1_t *c,uint32_t *id)
{ return result(solar_os_controller_open_bound(name,owner,c,id)); }
static int command_bound(uint32_t id,float target,uint64_t acquired_us)
{ return result(solar_os_controller_command_owned(id,target,acquired_us)); }
static int step_bound(uint32_t id,solar_os_native_bound_controller_status_v1_t *s)
{
    esp_err_t err=solar_os_controller_step_bound(id,s);
    if(s && err!=ESP_ERR_NOT_FOUND) s->last_error=result(s->last_error);
    return result(err);
}
static int status_bound(uint32_t id,solar_os_native_bound_controller_status_v1_t *s)
{
    esp_err_t err=solar_os_controller_status_bound(id,s);
    if(err==ESP_OK) s->last_error=result(s->last_error);
    return result(err);
}

static int named_create(const char *name, solar_os_controller_kind_t kind)
{
    return result(solar_os_controller_create(name, kind));
}

static int named_remove(const char *name)
{
    return result(solar_os_controller_delete(name));
}

static int named_info(size_t index, solar_os_controller_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    if (!solar_os_controller_get_info(index, info)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    info->last_error = result(info->last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int named_find(const char *name, solar_os_controller_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    if (!name || !*name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const esp_err_t error = solar_os_controller_find(name, info);
    if (error == ESP_OK) info->last_error = result(info->last_error);
    return result(error);
}

static int named_configure(const char *name, const char *key, float value)
{
    return result(solar_os_controller_configure(name, key, value));
}

static int named_bind(const char *name, const char *source, const char *target)
{
    return result(solar_os_controller_bind(name, source, target));
}

static int named_feed(const char *name, float measurement, uint64_t sample_ms)
{
    return result(solar_os_controller_feed(name, measurement, sample_ms));
}

static int named_step(const char *name, float measurement, float dt, float *output)
{
    if (!output) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *output = 0;
    return result(solar_os_controller_step(name, measurement, dt, output));
}

static int named_mode(const char *name, bool automatic, float manual_output)
{
    return result(automatic ? solar_os_controller_auto(name) : solar_os_controller_manual(name, manual_output));
}

static int named_reset(const char *name)
{
    return result(solar_os_controller_reset(name));
}

static int named_start(const char *name)
{
    return result(solar_os_controller_start(name));
}

static int named_stop(const char *name)
{
    return result(solar_os_controller_stop(name));
}

static int named_stop_all(void)
{
    return result(solar_os_controllers_stop_all());
}

static int named_tune(const char *name, float band, uint32_t points, uint32_t timeout_ms)
{
    return result(solar_os_controller_tune(name, band, points, timeout_ms));
}

static const char *tune_label(solar_os_controller_tune_label_t kind, uint32_t value)
{
    switch (kind) {
    case SOLAR_OS_CONTROLLER_TUNE_LABEL_STATE: return solar_os_controller_tune_state_name(value);
    case SOLAR_OS_CONTROLLER_TUNE_LABEL_REASON: return solar_os_controller_tune_reason_name(value);
    case SOLAR_OS_CONTROLLER_TUNE_LABEL_PHASE: return solar_os_controller_tune_phase_name(value);
    }
    return NULL;
}

static const solar_os_native_controllers_api_v1_t api={
    .abi_version=SOLAR_OS_NATIVE_CONTROLLERS_ABI,.struct_size=sizeof(api),
    .open=open_controller,.evaluate=evaluate,.pause=pause_controller,.fault=fault_controller,.close=close_controller,
    .open_bound=open_bound,.command=command_bound,.step_bound=step_bound,.status_bound=status_bound,
    .create = named_create,
    .remove = named_remove,
    .count = solar_os_controller_count,
    .info = named_info,
    .find = named_find,
    .configure = named_configure,
    .bind = named_bind,
    .feed = named_feed,
    .step = named_step,
    .set_mode = named_mode,
    .reset = named_reset,
    .start = named_start,
    .stop = named_stop,
    .tune = named_tune,
    .tune_label = tune_label,
    .stop_all = named_stop_all,
};

static void math_defaults(solar_os_controller_kind_t kind, solar_os_controller_math_config_t *config)
{
    if (config) *config = solar_os_controller_math_defaults(kind);
}

static bool math_set_mode(const solar_os_controller_math_config_t *config,
                          solar_os_controller_math_state_t *state, bool automatic, float output)
{
    if (!automatic) return solar_os_controller_math_manual(config, state, output);
    if (!state) return false;
    solar_os_controller_math_auto(state);
    return true;
}

static void tuner_storage(size_t *bytes, size_t *alignment)
{
    if (bytes) *bytes = sizeof(solar_os_controller_tuner_t);
    if (alignment) *alignment = _Alignof(solar_os_controller_tuner_t);
}

static bool tuner_storage_valid(const void *storage, size_t capacity)
{
    return storage && capacity >= sizeof(solar_os_controller_tuner_t) &&
        (uintptr_t)storage % _Alignof(solar_os_controller_tuner_t) == 0;
}

static bool tuner_begin(void *storage, size_t capacity, const solar_os_controller_tune_config_t *config)
{
    return tuner_storage_valid(storage, capacity) && solar_os_controller_tuner_init(storage, config);
}

static bool tuner_step(void *storage, size_t capacity, float measurement, float dt, float *output)
{
    return tuner_storage_valid(storage, capacity) &&
        solar_os_controller_tuner_step(storage, measurement, dt, output);
}

static bool tuner_result(const void *storage, size_t capacity, solar_os_controller_tune_result_t *out)
{
    if (!out || !tuner_storage_valid(storage, capacity)) return false;
    const solar_os_controller_tuner_t *tuner = storage;
    *out = tuner->result;
    return true;
}

static const solar_os_native_controller_math_api_v1_t math_api = {
    .abi_version = SOLAR_OS_NATIVE_CONTROLLER_MATH_ABI,
    .struct_size = sizeof(math_api),
    .defaults = math_defaults,
    .valid = solar_os_controller_math_valid,
    .reset = solar_os_controller_math_reset,
    .set_mode = math_set_mode,
    .step = solar_os_controller_math_step,
    .tuner_storage = tuner_storage,
    .tuner_begin = tuner_begin,
    .tuner_step = tuner_step,
    .tuner_result = tuner_result,
};

const void *solar_os_native_controllers_get_service(const char *name, uint32_t abi, uint32_t size)
{
    const uint32_t minimum = size;
#include "generated/native_controllers_dispatch.inc"
    return NULL;
}
