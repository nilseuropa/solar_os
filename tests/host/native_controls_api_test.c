#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_controls_api.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"
#include "solar_os_controls.h"
static unsigned allocations;
static float parameter_value = 1;
size_t strlcpy(char *out, const char *in, size_t capacity)
{ size_t size = strlen(in); if (capacity) { size_t n = size < capacity - 1 ? size : capacity - 1; memcpy(out, in, n); out[n] = 0; } return size; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); void *p = calloc(n, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
esp_err_t solar_os_stream_get_info(const char *id, solar_os_stream_info_t *out)
{ assert(!strcmp(id, "source")); *out = (solar_os_stream_info_t){.type = SOLAR_OS_STREAM_TYPE_SCALAR}; return ESP_OK; }
static esp_err_t get_parameter(void *user, float *out) { assert(user == &parameter_value); *out = parameter_value; return ESP_OK; }
static esp_err_t set_parameter(void *user, float value) { assert(user == &parameter_value); parameter_value = value; return ESP_OK; }
int main(void)
{
    const solar_os_native_parameters_api_v1_t *parameters = solar_os_native_controls_api_get_service("parameters", 1, sizeof(*parameters)); assert(parameters);
    const solar_os_native_controls_api_v1_t *controls = solar_os_native_controls_api_get_service("controls", 1, sizeof(*controls)); assert(controls);
    solar_os_parameter_definition_t definition = {.name = "gain", .label = "Gain", .unit = "dB", .minimum = 1, .maximum = 100,
        .step = 0, .curve = SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC, .get = get_parameter, .set = set_parameter, .user = &parameter_value};
    solar_os_parameter_registration_t registration = SOLAR_OS_PARAMETER_REGISTRATION_INIT;
    assert(!solar_os_parameters_register("test", &definition, 1, &registration));
    solar_os_native_parameter_info_v1_t info; assert(parameters->count() == 1 && !parameters->find("test.gain", &info));
    assert(info.curve == SOLAR_OS_NATIVE_PARAMETER_LOGARITHMIC && info.minimum == 1 && info.maximum == 100 && !strcmp(info.unit, "dB"));
    assert(!parameters->set_normalized("test.gain", 65535) && parameter_value == 100);
    assert(parameters->set_normalized("test.gain", 65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && parameter_value == 100);
    solar_os_native_handle_t claim, other;
    assert(!parameters->claim("test.gain", "writer", &claim));
    assert(parameters->claim("test.gain", "writer", &other) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other);
    assert(parameters->set("test.gain", 10) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!parameters->set_owned(claim, 10) && parameter_value == 10);
    uint32_t normalized; assert(!parameters->get_normalized("test.gain", &normalized) && normalized >= 32767 && normalized <= 32768);
    assert(!solar_os_parameters_unregister(&registration));
    assert(!solar_os_parameters_register("test", &definition, 1, &registration));
    assert(!parameters->claim("test.gain", "writer", &other));
    assert(parameters->set_owned(claim, 20) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && parameter_value == 10);
    assert(!parameters->release(claim));
    assert(parameters->set("test.gain", 20) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!parameters->set_owned(other, 20) && parameter_value == 20); assert(!parameters->release(other));
    solar_os_native_control_config_v1_t config = {.name = "knob", .source = "source", .input_minimum = -1, .input_maximum = 1, .inverted = 1};
    assert(!controls->create(&config) && controls->count() == 1);
    memset(config.source, '?', sizeof(config.source)); assert(controls->create(&config) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(controls->set("knob", 65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!controls->set("knob", 50000)); uint32_t value; assert(!controls->get("knob", &value) && value == 50000);
    solar_os_native_control_info_v1_t control; assert(!controls->find("knob", &control) && control.config.inverted && !strcmp(control.config.source, "source"));
    uint32_t binding; assert(!controls->bind_parameter("knob", "test.gain", true, &binding));
    solar_os_control_binding_note(binding, 123, true, true, 10, 11, ESP_ERR_TIMEOUT, false);
    solar_os_native_control_binding_v1_t binding_info; assert(!controls->binding_info(0, &binding_info));
    assert(binding_info.id == binding && binding_info.target == SOLAR_OS_NATIVE_CONTROL_PARAMETER && binding_info.pickup_latched && binding_info.pickup_seen);
    assert(binding_info.last_generation == 0 && binding_info.pickup_previous == 10 && binding_info.last_target_value == 11 && binding_info.error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    solar_os_control_binding_note(binding, 123, true, true, 10, 11, ESP_OK, true);
    assert(!controls->binding_info(0, &binding_info) && binding_info.last_generation == 123 && binding_info.applied == 1 && binding_info.errors == 1);
    assert(controls->bind_midi_cc("knob", 256, 1, &binding) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !binding);
    assert(!controls->bind_midi_cc("knob", 1, 127, &binding) && controls->binding_count() == 2);
    assert(!controls->binding_info(1, &binding_info) && binding_info.target == SOLAR_OS_NATIVE_CONTROL_MIDI_CC && binding_info.midi_controller == 127);
    size_t removed; assert(!controls->unbind("knob", &removed) && removed == 2);
    assert(!controls->delete_control("knob") && !controls->count()); assert(!solar_os_parameters_unregister(&registration));
    assert(!allocations && !solar_os_native_handle_count()); puts("native parameters and controls with real registries: values, claims and bindings passed"); return 0;
}
