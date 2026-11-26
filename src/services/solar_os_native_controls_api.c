#include "solar_os_native_controls_api.h"
#include "solar_os_native_controls_abi.h"
#include "solar_os_native_parameters_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include <stdio.h>
#include <string.h>
#define COPY_FIELD(out, in, field) do { \
    _Static_assert(sizeof((out)->field) == sizeof((in)->field), "native control field capacity"); \
    memcpy((out)->field, (in)->field, sizeof((out)->field)); \
} while (0)
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os.h"
#include "solar_os_native_runtime.h"
#include "solar_os_parameters.h"
static int parameter_info_out(const solar_os_parameter_info_t *in, solar_os_native_parameter_info_v1_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (in->curve) {
    case SOLAR_OS_PARAMETER_CURVE_LINEAR: out->curve = SOLAR_OS_NATIVE_PARAMETER_LINEAR; break;
    case SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC: out->curve = SOLAR_OS_NATIVE_PARAMETER_LOGARITHMIC; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    COPY_FIELD(out, in, path); COPY_FIELD(out, in, owner); COPY_FIELD(out, in, name); COPY_FIELD(out, in, label); COPY_FIELD(out, in, unit);
    out->minimum = in->minimum; out->maximum = in->maximum; out->step = in->step; return SOLAR_OS_NATIVE_OK;
}
static int parameter_info(size_t index, solar_os_native_parameter_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_parameter_info_t value = {0};
    return solar_os_parameter_get_info(index, &value) ? parameter_info_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int parameter_find(const char *path, solar_os_native_parameter_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_parameter_info_t value = {0};
    int result = solar_os_native_result(solar_os_parameter_find(path, &value)); return result ? result : parameter_info_out(&value, out);
}
static int parameter_get(const char *path, float *out) { return solar_os_native_result(solar_os_parameter_get(path, out)); }
static int parameter_set(const char *path, float value) { return solar_os_native_result(solar_os_parameter_set(path, value)); }
static int parameter_get_normalized(const char *path, uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; uint16_t value = 0; int result = solar_os_native_result(solar_os_parameter_get_normalized(path, &value));
    if (!result) *out = value;
    return result;
}
static int parameter_set_normalized(const char *path, uint32_t value)
{ return value > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_parameter_set_normalized(path, value)); }
typedef struct { char path[64], writer[32]; bool acquired; } native_parameter_claim_t;
static int claim_destroy(void *value)
{
    native_parameter_claim_t *state = value;
    if (state->acquired) {
        esp_err_t error = solar_os_parameter_release(state->path, state->writer);
        if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
    }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int parameter_claim(const char *path, const char *writer, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!path || !*path || strlen(path) >= 64 || !writer || !*writer || strlen(writer) >= 32) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_parameter_claim_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.parameter");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(state->path, path); int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_PARAMETER_CLAIM, state, claim_destroy, out);
    if (result) { claim_destroy(state); return result; }
    snprintf(state->writer, sizeof(state->writer), "%08lx/%.22s", (unsigned long)*out, writer);
    result = solar_os_native_result(solar_os_parameter_claim(path, state->writer));
    if (!result) state->acquired = true;
    else if (!solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_PARAMETER_CLAIM)) *out = 0;
    return result;
}
static int parameter_release(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_PARAMETER_CLAIM); }
static int parameter_set_owned(solar_os_native_handle_t id, float value)
{
    native_parameter_claim_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_PARAMETER_CLAIM);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_parameter_set_owned(state->path, state->writer, value));
    solar_os_native_handle_release(id); return result;
}
static int parameter_resource_path(const char *kind, const char *id, const char *name,
                                   char *path, size_t capacity)
{
    return solar_os_native_result(solar_os_parameter_resource_path(kind, id, name, path, capacity));
}

typedef struct {
    int (*get)(void *user, float *value);
    int (*set)(void *user, float value);
    void *user;
} native_parameter_callbacks_t;

typedef struct {
    solar_os_parameter_registration_t registration;
    uint32_t lease;
    native_parameter_callbacks_t callbacks[];
} native_parameter_provider_t;

static esp_err_t parameter_provider_get(void *user, float *value)
{
    native_parameter_callbacks_t *callbacks = user;
    return solar_os_native_callback_result(callbacks->get(callbacks->user, value));
}

static esp_err_t parameter_provider_set(void *user, float value)
{
    native_parameter_callbacks_t *callbacks = user;
    return solar_os_native_callback_result(callbacks->set(callbacks->user, value));
}

static int parameter_provider_destroy(void *value)
{
    native_parameter_provider_t *state = value;
    if (state->registration.token) {
        esp_err_t error = solar_os_parameters_unregister(&state->registration);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error != ESP_OK) return solar_os_native_result(error);
    }
    if (state->lease) {
        int result = solar_os_native_library_release(state->lease);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int parameter_provider_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_PARAMETER_PROVIDER);
}

static int parameter_provider_register(const char *owner, uint32_t lease,
    const solar_os_native_parameter_definition_v1_t *definitions, size_t count,
    solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!owner || !definitions || !count || count > SOLAR_OS_PARAMETER_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_app_t *app = lease ? NULL : solar_os_application_descriptor();
    if (!lease && !app) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;

    native_parameter_provider_t *state = solar_os_memory_calloc(1,
        sizeof(*state) + count * sizeof(state->callbacks[0]),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.parameters.provider");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_parameter_definition_t *core = solar_os_memory_calloc(count, sizeof(*core),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.parameters.definitions");
    int result = core ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    for (size_t i = 0; !result && i < count; i++) {
        const solar_os_native_parameter_definition_v1_t *in = &definitions[i];
        if (!in->get || !in->set || in->curve > SOLAR_OS_NATIVE_PARAMETER_LOGARITHMIC) {
            result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            break;
        }
        state->callbacks[i] = (native_parameter_callbacks_t){in->get, in->set, in->user};
        core[i] = (solar_os_parameter_definition_t){
            .name = in->name, .label = in->label, .unit = in->unit,
            .minimum = in->minimum, .maximum = in->maximum, .step = in->step,
            .curve = in->curve == SOLAR_OS_NATIVE_PARAMETER_LINEAR ?
                SOLAR_OS_PARAMETER_CURVE_LINEAR : SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC,
            .get = parameter_provider_get, .set = parameter_provider_set, .user = &state->callbacks[i],
        };
    }
    if (!result && lease) result = solar_os_native_library_retain(lease, &state->lease);
    if (!result) {
        const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_PARAMETER_PROVIDER;
        result = app ? solar_os_native_handle_create_owned(kind, state, parameter_provider_destroy, app, out) :
            solar_os_native_handle_create(kind, state, parameter_provider_destroy, out);
    }
    if (!result) {
        (void)solar_os_native_handle_acquire(*out, SOLAR_OS_NATIVE_HANDLE_PARAMETER_PROVIDER);
        result = solar_os_native_result(solar_os_parameters_register(owner, core, count, &state->registration));
        solar_os_native_handle_release(*out);
    }
    solar_os_memory_free(core);
    if (result) {
        if (*out) {
            if (!parameter_provider_close(*out)) *out = 0;
        } else {
            (void)parameter_provider_destroy(state);
        }
    }
    return result;
}

static int parameter_provider_update_range(solar_os_native_handle_t handle,
    const char *name, float minimum, float maximum)
{
    native_parameter_provider_t *state =
        solar_os_native_handle_acquire(handle, SOLAR_OS_NATIVE_HANDLE_PARAMETER_PROVIDER);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(
        solar_os_parameters_update_range(&state->registration, name, minimum, maximum));
    solar_os_native_handle_release(handle);
    return result;
}

static const char *parameter_curve_name(uint32_t curve)
{
    switch (curve) {
    case SOLAR_OS_NATIVE_PARAMETER_LINEAR: return solar_os_parameter_curve_name(SOLAR_OS_PARAMETER_CURVE_LINEAR);
    case SOLAR_OS_NATIVE_PARAMETER_LOGARITHMIC: return solar_os_parameter_curve_name(SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC);
    default: return NULL;
    }
}

static const solar_os_native_parameters_api_v1_t parameters_api = {
    .abi_version = SOLAR_OS_NATIVE_PARAMETERS_ABI, .struct_size = sizeof(parameters_api), .count = solar_os_parameter_count,
    .info = parameter_info, .find = parameter_find, .get = parameter_get, .set = parameter_set,
    .get_normalized = parameter_get_normalized, .set_normalized = parameter_set_normalized, .claim = parameter_claim, .release = parameter_release, .set_owned = parameter_set_owned,
    .resource_path = parameter_resource_path,
    .provider_register = parameter_provider_register,
    .provider_update_range = parameter_provider_update_range,
    .provider_close = parameter_provider_close,
    .curve_name = parameter_curve_name,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
#include "solar_os_controls.h"
static void config_out(const solar_os_control_config_t *in, solar_os_native_control_config_v1_t *out)
{
    memset(out, 0, sizeof(*out)); COPY_FIELD(out, in, name); COPY_FIELD(out, in, source);
    out->input_minimum = in->input_minimum; out->input_maximum = in->input_maximum; out->deadband = in->deadband;
    out->smoothing_ms = in->smoothing_ms; out->inverted = in->inverted;
}
static int control_create(const solar_os_native_control_config_v1_t *in)
{
    if (!in || !memchr(in->name, 0, sizeof(in->name)) || !memchr(in->source, 0, sizeof(in->source)) || in->inverted > 1) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_control_config_t out = {.input_minimum = in->input_minimum, .input_maximum = in->input_maximum,
        .deadband = in->deadband, .smoothing_ms = in->smoothing_ms, .inverted = in->inverted != 0};
    COPY_FIELD(&out, in, name); COPY_FIELD(&out, in, source); return solar_os_native_result(solar_os_control_create(&out));
}
static int control_delete(const char *name) { return solar_os_native_result(solar_os_control_delete(name)); }
static void control_info_out(const solar_os_control_info_t *in, solar_os_native_control_info_v1_t *out)
{
    memset(out, 0, sizeof(*out)); config_out(&in->config, &out->config);
    out->has_value = in->has_value; out->normalized = in->normalized; out->generation = in->generation; out->samples = in->samples;
    out->updates = in->updates; out->read_errors = in->read_errors; out->source_value = in->source_value; out->error = solar_os_native_result(in->last_error);
}
static int control_info(size_t index, solar_os_native_control_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_control_info_t value = {0};
    if (!solar_os_control_get_info(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    control_info_out(&value, out); return SOLAR_OS_NATIVE_OK;
}
static int control_find(const char *name, solar_os_native_control_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_control_info_t value = {0}; int result = solar_os_native_result(solar_os_control_find(name, &value));
    if (!result) control_info_out(&value, out);
    return result;
}
static int control_get(const char *name, uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; uint16_t value = 0; int result = solar_os_native_result(solar_os_control_get(name, &value));
    if (!result) *out = value;
    return result;
}
static int control_set(const char *name, uint32_t value)
{ return value > UINT16_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT : solar_os_native_result(solar_os_control_set(name, value)); }
static int bind_parameter(const char *control, const char *parameter, bool pickup, uint32_t *out)
{ return solar_os_native_result(solar_os_control_bind_parameter(control, parameter, pickup, out)); }
static int bind_midi_cc(const char *control, uint32_t channel, uint32_t controller, uint32_t *out)
{
    if (out) *out = 0;
    return channel > UINT8_MAX || controller > UINT8_MAX ? SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT :
        solar_os_native_result(solar_os_control_bind_midi_cc(control, channel, controller, out));
}
static int control_unbind(const char *control, size_t *removed) { return solar_os_native_result(solar_os_control_unbind(control, removed)); }
static int binding_info(size_t index, solar_os_native_control_binding_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_control_binding_info_t value = {0};
    if (!solar_os_control_binding_get(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    switch (value.target) {
    case SOLAR_OS_CONTROL_TARGET_PARAMETER: out->target = SOLAR_OS_NATIVE_CONTROL_PARAMETER; break;
    case SOLAR_OS_CONTROL_TARGET_MIDI_CC: out->target = SOLAR_OS_NATIVE_CONTROL_MIDI_CC; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    out->id = value.id; COPY_FIELD(out, &value, control); COPY_FIELD(out, &value, parameter);
#define SCALAR(field) out->field = value.field
    SCALAR(midi_channel); SCALAR(midi_controller); SCALAR(pickup); SCALAR(pickup_seen); SCALAR(pickup_latched);
    SCALAR(pickup_previous); SCALAR(last_target_value); SCALAR(last_generation); SCALAR(applied); SCALAR(errors);
#undef SCALAR
    out->error = solar_os_native_result(value.last_error); return SOLAR_OS_NATIVE_OK;
}
static const solar_os_native_controls_api_v1_t controls_api = {
    .abi_version = SOLAR_OS_NATIVE_CONTROLS_ABI, .struct_size = sizeof(controls_api), .create = control_create, .delete_control = control_delete,
    .clear = solar_os_control_clear, .count = solar_os_control_count, .info = control_info, .find = control_find, .get = control_get, .set = control_set,
    .bind_parameter = bind_parameter, .bind_midi_cc = bind_midi_cc, .unbind = control_unbind, .binding_count = solar_os_control_binding_count, .binding_info = binding_info,
};
#endif
const void *solar_os_native_controls_api_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_controls_api_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
