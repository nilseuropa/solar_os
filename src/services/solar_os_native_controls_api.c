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
static const solar_os_native_parameters_api_v1_t parameters_api = {
    .abi_version = SOLAR_OS_NATIVE_PARAMETERS_ABI, .struct_size = sizeof(parameters_api), .count = solar_os_parameter_count,
    .info = parameter_info, .find = parameter_find, .get = parameter_get, .set = parameter_set,
    .get_normalized = parameter_get_normalized, .set_normalized = parameter_set_normalized, .claim = parameter_claim, .release = parameter_release, .set_owned = parameter_set_owned,
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
