#include "solar_os_controls_runtime.h"

#include <stdatomic.h>

#include "solar_os_config.h"
#include "solar_os_controls.h"
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
#include "solar_os_midi.h"
#endif

#define CONTROLS_PICKUP_TOLERANCE 512U

static atomic_flag sampling = ATOMIC_FLAG_INIT;
static atomic_flag applying = ATOMIC_FLAG_INIT;

esp_err_t solar_os_controls_sample(uint32_t now_ms,
                                  uint32_t window_ms,
                                  solar_os_control_pass_t *pass)
{
    if (pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pass = (solar_os_control_pass_t){0};
    if (atomic_flag_test_and_set_explicit(&sampling, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t count = solar_os_control_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_control_info_t control;
        if (!solar_os_control_get_info(i, &control) ||
            control.config.source[0] == '\0') {
            continue;
        }
        solar_os_stream_handle_t stream =
            (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        esp_err_t err = solar_os_stream_open(control.config.source,
                                             "controls",
                                             &stream);
        float value = 0.0f;
        if (err == ESP_OK) {
            const solar_os_stream_read_options_t options = {
                .window_ms = window_ms,
            };
            err = solar_os_stream_read_scalar(&stream, &options, &value);
            solar_os_stream_close(&stream);
        }
        if (err == ESP_OK) {
            err = solar_os_control_publish_sample(control.config.name,
                                                  value,
                                                  now_ms);
        }
        if (err == ESP_OK) {
            pass->succeeded++;
        } else {
            solar_os_control_note_read_error(control.config.name, err);
            pass->errors++;
        }
    }
    atomic_flag_clear_explicit(&sampling, memory_order_release);
    return ESP_OK;
}

static bool controls_crossed(uint16_t previous,
                             uint16_t current,
                             uint16_t target)
{
    const uint16_t lower = previous < current ? previous : current;
    const uint16_t upper = previous < current ? current : previous;
    const uint16_t distance = current > target ? current - target : target - current;
    return (target >= lower && target <= upper) ||
           distance <= CONTROLS_PICKUP_TOLERANCE;
}

static void apply_parameter(
    const solar_os_control_info_t *control,
    const solar_os_control_binding_info_t *binding,
    solar_os_control_pass_t *pass)
{
    uint16_t target_value = 0U;
    esp_err_t err = solar_os_parameter_get_normalized(binding->parameter,
                                                      &target_value);
    if (err != ESP_OK) {
        solar_os_control_binding_note(binding->id,
                                      binding->last_generation,
                                      binding->pickup_seen,
                                      binding->pickup_latched,
                                      binding->pickup_previous,
                                      binding->last_target_value,
                                      err,
                                      false);
        return;
    }
    if (binding->last_generation == control->generation &&
        binding->last_error == ESP_OK) {
        if (!binding->pickup) {
            return;
        }
        const uint16_t distance = binding->last_target_value > target_value ?
            binding->last_target_value - target_value :
            target_value - binding->last_target_value;
        if (binding->pickup_latched) {
            if (distance > CONTROLS_PICKUP_TOLERANCE) {
                solar_os_control_binding_note(binding->id,
                                              binding->last_generation,
                                              false,
                                              false,
                                              control->normalized,
                                              target_value,
                                              ESP_OK,
                                              false);
            }
            return;
        }
    }

    bool pickup_seen = binding->pickup_seen;
    bool pickup_latched = binding->pickup_latched;
    uint16_t pickup_previous = binding->pickup_previous;
    if (binding->pickup && !pickup_latched) {
        if (!pickup_seen) {
            pickup_seen = true;
            pickup_previous = control->normalized;
            solar_os_control_binding_note(binding->id,
                                          binding->last_generation,
                                          pickup_seen,
                                          false,
                                          pickup_previous,
                                          target_value,
                                          ESP_OK,
                                          false);
            return;
        }
        if (!controls_crossed(pickup_previous,
                              control->normalized,
                              target_value)) {
            solar_os_control_binding_note(binding->id,
                                          binding->last_generation,
                                          true,
                                          false,
                                          control->normalized,
                                          target_value,
                                          ESP_OK,
                                          false);
            return;
        }
        pickup_latched = true;
    }

    err = solar_os_parameter_set_normalized(binding->parameter,
                                            control->normalized);
    uint16_t applied_target = control->normalized;
    if (err == ESP_OK) {
        (void)solar_os_parameter_get_normalized(binding->parameter,
                                                &applied_target);
    }
    solar_os_control_binding_note(binding->id,
                                  control->generation,
                                  pickup_seen,
                                  pickup_latched,
                                  control->normalized,
                                  applied_target,
                                  err,
                                  err == ESP_OK);
    if (err == ESP_OK) {
        pass->succeeded++;
    } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_STATE) {
        pass->errors++;
    }
}

static void apply_midi(
    const solar_os_control_info_t *control,
    const solar_os_control_binding_info_t *binding,
    solar_os_control_pass_t *pass)
{
    if (binding->last_generation == control->generation &&
        binding->last_error == ESP_OK) {
        return;
    }
#if SOLAR_OS_PACKAGE_SERVICE_MIDI
    const solar_os_midi_message_t message = {
        .status = (uint8_t)(0xb0U | (binding->midi_channel - 1U)),
        .data1 = binding->midi_controller,
        .data2 = (uint8_t)(((uint32_t)control->normalized * 127U + 32767U) /
                          SOLAR_OS_CONTROL_NORMALIZED_MAX),
        .length = 3U,
    };
    const esp_err_t err = solar_os_midi_send(&message);
#else
    const esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#endif
    solar_os_control_binding_note(binding->id,
                                  control->generation,
                                  false,
                                  true,
                                  control->normalized,
                                  control->normalized,
                                  err,
                                  err == ESP_OK);
    if (err == ESP_OK) {
        pass->succeeded++;
    } else if (err != ESP_ERR_INVALID_STATE) {
        pass->errors++;
    }
}

esp_err_t solar_os_controls_apply_bindings(solar_os_control_pass_t *pass)
{
    if (pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pass = (solar_os_control_pass_t){0};
    if (atomic_flag_test_and_set_explicit(&applying, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t count = solar_os_control_binding_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_control_binding_info_t binding;
        solar_os_control_info_t control;
        if (!solar_os_control_binding_get(i, &binding) ||
            solar_os_control_find(binding.control, &control) != ESP_OK ||
            !control.has_value) {
            continue;
        }
        if (binding.target == SOLAR_OS_CONTROL_TARGET_MIDI_CC) {
            apply_midi(&control, &binding, pass);
        } else {
            apply_parameter(&control, &binding, pass);
        }
    }
    atomic_flag_clear_explicit(&applying, memory_order_release);
    return ESP_OK;
}
