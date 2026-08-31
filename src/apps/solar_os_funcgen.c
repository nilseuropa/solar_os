#include "solar_os_funcgen.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "solar_os_audio.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_parameters.h"
#include "solar_os_shell_io.h"
#include "solar_os_signal_widgets.h"
#include "solar_os_synth.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define FUNCGEN_OWNER "app:funcgen"
#define FUNCGEN_PARAMETER_OWNER "funcgen"
#define FUNCGEN_BLOCK_FRAMES 128U
#define FUNCGEN_REFRESH_MS 40U
#define FUNCGEN_TICK_DEADLINE_MS 10U
#define FUNCGEN_SCOPE_SAMPLES 256U
#define FUNCGEN_DISPLAY_HPM_HZ_TENTHS 255U
#define FUNCGEN_FREQUENCY_MIN_HZ 20U
#define FUNCGEN_FREQUENCY_MAX_HZ 8000U
#define FUNCGEN_FREQUENCY_DEFAULT_HZ 440U
#define FUNCGEN_AMPLITUDE_DEFAULT_PERCENT 25U
#define FUNCGEN_PULSE_DEFAULT_PERCENT 50U
#define FUNCGEN_SWEEP_TIME_DEFAULT_MS 5000U
#define FUNCGEN_SWEEP_TIME_MIN_MS 100U
#define FUNCGEN_SWEEP_TIME_MAX_MS 60000U
#define FUNCGEN_OUTPUT_MAX (SOLAR_OS_AUDIO_DEVICE_MAX + 1U)
#define FUNCGEN_TWO_PI 6.28318530717958647692f

typedef enum {
    FUNCGEN_MODE_TUI,
    FUNCGEN_MODE_GRAPHICS,
} funcgen_mode_t;

typedef enum {
    FUNCGEN_WAVE_SINE,
    FUNCGEN_WAVE_SQUARE,
    FUNCGEN_WAVE_TRIANGLE,
    FUNCGEN_WAVE_SAW,
    FUNCGEN_WAVE_PULSE,
    FUNCGEN_WAVE_NOISE,
    FUNCGEN_WAVE_COUNT,
} funcgen_waveform_t;

typedef enum {
    FUNCGEN_CONTROL_WAVEFORM,
    FUNCGEN_CONTROL_FREQUENCY,
    FUNCGEN_CONTROL_AMPLITUDE,
    FUNCGEN_CONTROL_PULSE_WIDTH,
    FUNCGEN_CONTROL_SWEEP,
    FUNCGEN_CONTROL_SWEEP_END,
    FUNCGEN_CONTROL_SWEEP_TIME,
    FUNCGEN_CONTROL_OUTPUT,
    FUNCGEN_CONTROL_COUNT,
} funcgen_control_t;

typedef enum {
    FUNCGEN_PARAMETER_WAVEFORM,
    FUNCGEN_PARAMETER_FREQUENCY,
    FUNCGEN_PARAMETER_AMPLITUDE,
    FUNCGEN_PARAMETER_PULSE_WIDTH,
    FUNCGEN_PARAMETER_SWEEP,
    FUNCGEN_PARAMETER_SWEEP_END,
    FUNCGEN_PARAMETER_SWEEP_TIME,
    FUNCGEN_PARAMETER_OUTPUT,
    FUNCGEN_PARAMETER_ENABLED,
} funcgen_parameter_t;

typedef struct {
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    char stream[SOLAR_OS_STREAM_ID_MAX];
} funcgen_output_t;

typedef struct {
    funcgen_mode_t mode;
    solar_os_tui_t tui;
    solar_os_shell_io_t fallback_io;
    solar_os_oscilloscope_widget_t *scope;
    solar_os_parameter_registration_t parameter_registration;
    funcgen_output_t outputs[FUNCGEN_OUTPUT_MAX];
    size_t output_count;
    size_t output_index;
    funcgen_control_t selected;
    funcgen_waveform_t waveform;
    uint32_t frequency_hz;
    uint32_t sweep_end_hz;
    uint32_t sweep_time_ms;
    uint8_t amplitude_percent;
    uint8_t pulse_width_percent;
    bool sweep_enabled;
    bool enabled;
    bool ui_started;
    bool suspended;
    bool high_refresh_active;
    volatile bool redraw;
    esp_err_t last_error;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
} funcgen_app_state_t;

/* Only the time-critical oscillator state stays in internal SRAM. */
typedef struct {
    funcgen_waveform_t waveform;
    uint32_t frequency_hz;
    uint32_t sweep_end_hz;
    uint32_t sweep_time_ms;
    uint32_t phase;
    uint32_t noise;
    uint64_t sweep_frames;
    uint32_t generation;
    uint8_t amplitude_percent;
    uint8_t pulse_width_percent;
    bool sweep_enabled;
} funcgen_render_state_t;

typedef struct {
    funcgen_app_state_t app;
    funcgen_render_state_t render;
} funcgen_cold_state_t;

static void *funcgen_state;
#define funcgen (((funcgen_cold_state_t *)funcgen_state)->app)
#define funcgen_render_state (((funcgen_cold_state_t *)funcgen_state)->render)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("audio render callback spinlock")
static portMUX_TYPE funcgen_render_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *const funcgen_wave_names[] = {
    "Sine", "Square", "Triangle", "Saw", "Pulse", "Noise",
};

static solar_os_shell_io_t *funcgen_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL ||
        solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&funcgen.fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &funcgen.fallback_io);
        io = &funcgen.fallback_io;
    }
    return io;
}

static bool funcgen_graphical_session(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = funcgen_io(ctx);
    return solar_os_context_gfx(ctx) != NULL &&
           (io == NULL ||
            solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT);
}

static const char *funcgen_output_stream(void)
{
    return funcgen.output_index > 0U &&
                   funcgen.output_index < funcgen.output_count
               ? funcgen.outputs[funcgen.output_index].stream
               : NULL;
}

static const char *funcgen_output_label(void)
{
    const char *stream = funcgen_output_stream();
    return stream != NULL ? stream : "auto";
}

static void funcgen_discover_outputs(void)
{
    funcgen.output_count = 1U;
    strlcpy(funcgen.outputs[0].id, "auto", sizeof(funcgen.outputs[0].id));
    strlcpy(funcgen.outputs[0].stream, "", sizeof(funcgen.outputs[0].stream));
    for (size_t i = 0U; i < solar_os_audio_device_count() &&
                        funcgen.output_count < FUNCGEN_OUTPUT_MAX;
         i++) {
        solar_os_audio_device_info_t device;
        if (!solar_os_audio_device_get(i, &device) ||
            (device.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) == 0U ||
            device.playback_stream[0] == '\0') {
            continue;
        }
        funcgen_output_t *output = &funcgen.outputs[funcgen.output_count++];
        strlcpy(output->id, device.id, sizeof(output->id));
        strlcpy(output->stream, device.playback_stream, sizeof(output->stream));
    }
}

static void funcgen_sync_render(bool reset)
{
    portENTER_CRITICAL(&funcgen_render_lock);
    funcgen_render_state.waveform = funcgen.waveform;
    funcgen_render_state.frequency_hz = funcgen.frequency_hz;
    funcgen_render_state.sweep_end_hz = funcgen.sweep_end_hz;
    funcgen_render_state.sweep_time_ms = funcgen.sweep_time_ms;
    funcgen_render_state.amplitude_percent = funcgen.amplitude_percent;
    funcgen_render_state.pulse_width_percent = funcgen.pulse_width_percent;
    funcgen_render_state.sweep_enabled = funcgen.sweep_enabled;
    funcgen_render_state.generation++;
    if (reset) {
        funcgen_render_state.phase = 0U;
        funcgen_render_state.noise = 0x6d2b79f5U;
        funcgen_render_state.sweep_frames = 0U;
    }
    portEXIT_CRITICAL(&funcgen_render_lock);
}

static uint32_t funcgen_noise_next(uint32_t value)
{
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    return value != 0U ? value : 0x6d2b79f5U;
}

static int32_t funcgen_wave_sample(funcgen_waveform_t waveform, uint32_t phase,
                                   uint8_t pulse_width, uint32_t *noise)
{
    switch (waveform) {
    case FUNCGEN_WAVE_SINE:
        return (int32_t)lroundf(
            sinf((float)phase * (FUNCGEN_TWO_PI / 4294967296.0f)) * 32767.0f);
    case FUNCGEN_WAVE_SQUARE:
        return (phase & 0x80000000U) != 0U ? 32767 : -32767;
    case FUNCGEN_WAVE_TRIANGLE: {
        const uint32_t position = phase >> 16U;
        return position < 32768U ? (int32_t)(position * 2U) - 32767
                                 : 98303 - (int32_t)(position * 2U);
    }
    case FUNCGEN_WAVE_SAW:
        return (int32_t)(phase >> 16U) - 32768;
    case FUNCGEN_WAVE_PULSE: {
        const uint32_t threshold =
            (uint32_t)(((uint64_t)pulse_width * UINT32_MAX) / 100U);
        return phase <= threshold ? 32767 : -32767;
    }
    case FUNCGEN_WAVE_NOISE:
        *noise = funcgen_noise_next(*noise);
        return (int16_t)(*noise >> 16U);
    default:
        return 0;
    }
}

static void funcgen_render_pcm(int16_t *samples, size_t frames,
                               uint32_t sample_rate, void *user)
{
    (void)user;
    if (samples == NULL || sample_rate == 0U) {
        return;
    }

    funcgen_render_state_t state;
    portENTER_CRITICAL(&funcgen_render_lock);
    state = funcgen_render_state;
    portEXIT_CRITICAL(&funcgen_render_lock);

    uint32_t frequency = state.frequency_hz;
    if (state.sweep_enabled && state.sweep_time_ms > 0U) {
        const uint64_t duration_frames =
            ((uint64_t)sample_rate * state.sweep_time_ms) / 1000U;
        if (duration_frames > 0U) {
            const uint64_t position = state.sweep_frames % duration_frames;
            const int64_t difference =
                (int64_t)state.sweep_end_hz - (int64_t)state.frequency_hz;
            frequency = (uint32_t)((int64_t)state.frequency_hz +
                                   (difference * (int64_t)position) /
                                       (int64_t)duration_frames);
        }
    }
    const uint32_t nyquist = sample_rate / 2U;
    if (nyquist > 1U && frequency >= nyquist) {
        frequency = nyquist - 1U;
    }
    const uint32_t phase_step =
        (uint32_t)(((uint64_t)frequency << 32U) / sample_rate);

    for (size_t frame = 0U; frame < frames; frame++) {
        const int32_t raw =
            funcgen_wave_sample(state.waveform, state.phase,
                                state.pulse_width_percent, &state.noise);
        const int16_t sample =
            (int16_t)((raw * (int32_t)state.amplitude_percent) / 100);
        samples[frame * 2U] = sample;
        samples[frame * 2U + 1U] = sample;
        state.phase += phase_step;
    }
    state.sweep_frames += frames;

    portENTER_CRITICAL(&funcgen_render_lock);
    if (state.generation == funcgen_render_state.generation) {
        funcgen_render_state.phase = state.phase;
        funcgen_render_state.noise = state.noise;
        funcgen_render_state.sweep_frames = state.sweep_frames;
    }
    portEXIT_CRITICAL(&funcgen_render_lock);

    if (funcgen.scope != NULL) {
        (void)solar_os_oscilloscope_widget_submit_s16(funcgen.scope, samples,
                                                      frames, 2U);
    }
    funcgen.redraw = true;
}

static esp_err_t funcgen_set_enabled(bool enabled)
{
    if (enabled == funcgen.enabled) {
        return ESP_OK;
    }
    esp_err_t err;
    if (!enabled) {
        err = solar_os_synth_stop(FUNCGEN_OWNER);
        if (err == ESP_OK) {
            funcgen.enabled = false;
        }
    } else {
        funcgen_sync_render(true);
        const solar_os_synth_config_t config = {
            .owner = FUNCGEN_OWNER,
            .playback_stream = funcgen_output_stream(),
            .render = funcgen_render_pcm,
            .block_frames = FUNCGEN_BLOCK_FRAMES,
        };
        err = solar_os_synth_start(&config);
        funcgen.enabled = err == ESP_OK;
    }
    funcgen.last_error = err;
    funcgen.redraw = true;
    return err;
}

static esp_err_t funcgen_select_output(size_t index)
{
    if (index >= funcgen.output_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool restart = funcgen.enabled;
    if (restart) {
        const esp_err_t stop_err = funcgen_set_enabled(false);
        if (stop_err != ESP_OK) {
            return stop_err;
        }
    }
    funcgen.output_index = index;
    return restart ? funcgen_set_enabled(true) : ESP_OK;
}

static esp_err_t funcgen_parameter_get(void *user, float *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    switch ((funcgen_parameter_t)(uintptr_t)user) {
    case FUNCGEN_PARAMETER_WAVEFORM:
        *value = (float)funcgen.waveform;
        break;
    case FUNCGEN_PARAMETER_FREQUENCY:
        *value = (float)funcgen.frequency_hz;
        break;
    case FUNCGEN_PARAMETER_AMPLITUDE:
        *value = (float)funcgen.amplitude_percent;
        break;
    case FUNCGEN_PARAMETER_PULSE_WIDTH:
        *value = (float)funcgen.pulse_width_percent;
        break;
    case FUNCGEN_PARAMETER_SWEEP:
        *value = funcgen.sweep_enabled ? 1.0f : 0.0f;
        break;
    case FUNCGEN_PARAMETER_SWEEP_END:
        *value = (float)funcgen.sweep_end_hz;
        break;
    case FUNCGEN_PARAMETER_SWEEP_TIME:
        *value = (float)funcgen.sweep_time_ms;
        break;
    case FUNCGEN_PARAMETER_OUTPUT:
        *value = (float)funcgen.output_index;
        break;
    case FUNCGEN_PARAMETER_ENABLED:
        *value = funcgen.enabled ? 1.0f : 0.0f;
        break;
    default:
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t funcgen_parameter_set(void *user, float value)
{
    esp_err_t err = ESP_OK;
    switch ((funcgen_parameter_t)(uintptr_t)user) {
    case FUNCGEN_PARAMETER_WAVEFORM:
        funcgen.waveform = (funcgen_waveform_t)lroundf(value);
        break;
    case FUNCGEN_PARAMETER_FREQUENCY:
        funcgen.frequency_hz = (uint32_t)lroundf(value);
        break;
    case FUNCGEN_PARAMETER_AMPLITUDE:
        funcgen.amplitude_percent = (uint8_t)lroundf(value);
        break;
    case FUNCGEN_PARAMETER_PULSE_WIDTH:
        funcgen.pulse_width_percent = (uint8_t)lroundf(value);
        break;
    case FUNCGEN_PARAMETER_SWEEP:
        funcgen.sweep_enabled = value >= 0.5f;
        break;
    case FUNCGEN_PARAMETER_SWEEP_END:
        funcgen.sweep_end_hz = (uint32_t)lroundf(value);
        break;
    case FUNCGEN_PARAMETER_SWEEP_TIME:
        funcgen.sweep_time_ms = (uint32_t)lroundf(value);
        break;
    case FUNCGEN_PARAMETER_OUTPUT:
        err = funcgen_select_output((size_t)lroundf(value));
        break;
    case FUNCGEN_PARAMETER_ENABLED:
        err = funcgen_set_enabled(value >= 0.5f);
        break;
    default:
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK &&
        (funcgen_parameter_t)(uintptr_t)user != FUNCGEN_PARAMETER_OUTPUT &&
        (funcgen_parameter_t)(uintptr_t)user != FUNCGEN_PARAMETER_ENABLED) {
        funcgen_sync_render(false);
    }
    funcgen.redraw = true;
    return err;
}

#define FUNCGEN_PARAMETER(name_, label_, unit_, min_, max_, step_, curve_,     \
                          id_)                                                 \
    {                                                                          \
        .name = name_, .label = label_, .unit = unit_, .minimum = min_,        \
        .maximum = max_, .step = step_, .curve = curve_,                       \
        .get = funcgen_parameter_get, .set = funcgen_parameter_set,            \
        .user = (void *)(uintptr_t)(id_)                                       \
    }

static const solar_os_parameter_definition_t funcgen_parameters[] = {
    FUNCGEN_PARAMETER(
        "waveform", "Waveform", "#", 0.0f, (float)(FUNCGEN_WAVE_COUNT - 1U),
        1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR, FUNCGEN_PARAMETER_WAVEFORM),
    FUNCGEN_PARAMETER(
        "frequency", "Frequency", "Hz", (float)FUNCGEN_FREQUENCY_MIN_HZ,
        (float)FUNCGEN_FREQUENCY_MAX_HZ, 1.0f,
        SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC, FUNCGEN_PARAMETER_FREQUENCY),
    FUNCGEN_PARAMETER("amplitude", "Amplitude", "%", 0.0f, 100.0f, 1.0f,
                      SOLAR_OS_PARAMETER_CURVE_LINEAR,
                      FUNCGEN_PARAMETER_AMPLITUDE),
    FUNCGEN_PARAMETER("pulse.width", "Pulse width", "%", 1.0f, 99.0f, 1.0f,
                      SOLAR_OS_PARAMETER_CURVE_LINEAR,
                      FUNCGEN_PARAMETER_PULSE_WIDTH),
    FUNCGEN_PARAMETER("sweep.enabled", "Sweep", "", 0.0f, 1.0f, 1.0f,
                      SOLAR_OS_PARAMETER_CURVE_LINEAR, FUNCGEN_PARAMETER_SWEEP),
    FUNCGEN_PARAMETER(
        "sweep.end", "Sweep end", "Hz", (float)FUNCGEN_FREQUENCY_MIN_HZ,
        (float)FUNCGEN_FREQUENCY_MAX_HZ, 1.0f,
        SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC, FUNCGEN_PARAMETER_SWEEP_END),
    FUNCGEN_PARAMETER(
        "sweep.time", "Sweep time", "ms", (float)FUNCGEN_SWEEP_TIME_MIN_MS,
        (float)FUNCGEN_SWEEP_TIME_MAX_MS, 100.0f,
        SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC, FUNCGEN_PARAMETER_SWEEP_TIME),
    FUNCGEN_PARAMETER(
        "output", "Output stream", "#", 0.0f, (float)SOLAR_OS_AUDIO_DEVICE_MAX,
        1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR, FUNCGEN_PARAMETER_OUTPUT),
    FUNCGEN_PARAMETER("enabled", "Output enabled", "", 0.0f, 1.0f, 1.0f,
                      SOLAR_OS_PARAMETER_CURVE_LINEAR,
                      FUNCGEN_PARAMETER_ENABLED),
};

#undef FUNCGEN_PARAMETER

static void funcgen_parameters_register(void)
{
    funcgen.parameter_registration =
        (solar_os_parameter_registration_t)SOLAR_OS_PARAMETER_REGISTRATION_INIT;
    const esp_err_t err = solar_os_parameters_register(
        FUNCGEN_PARAMETER_OWNER, funcgen_parameters,
        sizeof(funcgen_parameters) / sizeof(funcgen_parameters[0]),
        &funcgen.parameter_registration);
    if (err != ESP_OK) {
        funcgen.last_error = err;
    }
}

static void funcgen_parameters_unregister(void)
{
    if (funcgen.parameter_registration.token != 0U) {
        (void)solar_os_parameters_unregister(&funcgen.parameter_registration);
    }
}

static void funcgen_enable_high_refresh(solar_os_context_t *ctx)
{
    if (funcgen.high_refresh_active) {
        return;
    }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL ||
        !solar_os_gfx_display_target_name(gfx, funcgen.display_target,
                                          sizeof(funcgen.display_target))) {
        return;
    }
    if (solar_os_display_set_high_refresh_override(
            funcgen.display_target, true, FUNCGEN_DISPLAY_HPM_HZ_TENTHS) ==
        ESP_OK) {
        funcgen.high_refresh_active = true;
    }
}

static void funcgen_disable_high_refresh(void)
{
    if (funcgen.high_refresh_active) {
        (void)solar_os_display_set_high_refresh_override(
            funcgen.display_target, false, FUNCGEN_DISPLAY_HPM_HZ_TENTHS);
        funcgen.high_refresh_active = false;
    }
}

static void funcgen_control_value(funcgen_control_t control, char *value,
                                  size_t value_len)
{
    switch (control) {
    case FUNCGEN_CONTROL_WAVEFORM:
        strlcpy(value, funcgen_wave_names[funcgen.waveform], value_len);
        break;
    case FUNCGEN_CONTROL_FREQUENCY:
        snprintf(value, value_len, "%" PRIu32 " Hz", funcgen.frequency_hz);
        break;
    case FUNCGEN_CONTROL_AMPLITUDE:
        snprintf(value, value_len, "%u%%", (unsigned)funcgen.amplitude_percent);
        break;
    case FUNCGEN_CONTROL_PULSE_WIDTH:
        snprintf(value, value_len, "%u%%",
                 (unsigned)funcgen.pulse_width_percent);
        break;
    case FUNCGEN_CONTROL_SWEEP:
        strlcpy(value, funcgen.sweep_enabled ? "On" : "Off", value_len);
        break;
    case FUNCGEN_CONTROL_SWEEP_END:
        snprintf(value, value_len, "%" PRIu32 " Hz", funcgen.sweep_end_hz);
        break;
    case FUNCGEN_CONTROL_SWEEP_TIME:
        snprintf(value, value_len, "%.1f s",
                 (double)funcgen.sweep_time_ms / 1000.0);
        break;
    case FUNCGEN_CONTROL_OUTPUT:
        strlcpy(value, funcgen_output_label(), value_len);
        break;
    default:
        value[0] = '\0';
        break;
    }
}

static const char *funcgen_control_label(funcgen_control_t control)
{
    static const char *const labels[] = {
        "Wave",  "Frequency", "Amplitude",  "Pulse width",
        "Sweep", "Sweep end", "Sweep time", "Output",
    };
    return labels[control];
}

static unsigned funcgen_control_position(funcgen_control_t control)
{
    switch (control) {
    case FUNCGEN_CONTROL_WAVEFORM:
        return (unsigned)funcgen.waveform * 10U / (FUNCGEN_WAVE_COUNT - 1U);
    case FUNCGEN_CONTROL_FREQUENCY:
        return (unsigned)(logf((float)funcgen.frequency_hz /
                               FUNCGEN_FREQUENCY_MIN_HZ) /
                          logf((float)FUNCGEN_FREQUENCY_MAX_HZ /
                               FUNCGEN_FREQUENCY_MIN_HZ) *
                          10.0f);
    case FUNCGEN_CONTROL_AMPLITUDE:
        return funcgen.amplitude_percent / 10U;
    case FUNCGEN_CONTROL_PULSE_WIDTH:
        return funcgen.pulse_width_percent / 10U;
    case FUNCGEN_CONTROL_SWEEP:
        return funcgen.sweep_enabled ? 10U : 0U;
    case FUNCGEN_CONTROL_SWEEP_END:
        return (unsigned)(logf((float)funcgen.sweep_end_hz /
                               FUNCGEN_FREQUENCY_MIN_HZ) /
                          logf((float)FUNCGEN_FREQUENCY_MAX_HZ /
                               FUNCGEN_FREQUENCY_MIN_HZ) *
                          10.0f);
    case FUNCGEN_CONTROL_SWEEP_TIME:
        return (unsigned)(logf((float)funcgen.sweep_time_ms /
                               FUNCGEN_SWEEP_TIME_MIN_MS) /
                          logf((float)FUNCGEN_SWEEP_TIME_MAX_MS /
                               FUNCGEN_SWEEP_TIME_MIN_MS) *
                          10.0f);
    case FUNCGEN_CONTROL_OUTPUT:
        return funcgen.output_count > 1U
                   ? (unsigned)(funcgen.output_index * 10U /
                                (funcgen.output_count - 1U))
                   : 0U;
    default:
        return 0U;
    }
}

static void funcgen_draw_knob(solar_os_gfx_t *gfx, int center_x, int center_y,
                              int radius, funcgen_control_t control)
{
    static const int8_t indicator_x[] = {-7, -10, -10, -8, -4, 0,
                                         4,  8,   10,  10, 7};
    static const int8_t indicator_y[] = {7, 4, 0, -5, -9, -10, -9, -5, 0, 4, 7};
    unsigned position = funcgen_control_position(control);
    if (position > 10U)
        position = 10U;
    solar_os_gfx_circle(gfx, center_x, center_y, radius);
    if (funcgen.selected == control) {
        solar_os_gfx_circle(gfx, center_x, center_y, radius + 3);
    }
    const int scale = radius > 18 ? radius - 7 : radius / 2;
    solar_os_gfx_line(gfx, center_x, center_y,
                      center_x + indicator_x[position] * scale / 10,
                      center_y + indicator_y[position] * scale / 10);
    char value[24];
    funcgen_control_value(control, value, sizeof(value));
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
    const char *label = funcgen_control_label(control);
    solar_os_gfx_text(gfx,
                      center_x - (int)solar_os_gfx_text_width(gfx, label) / 2,
                      center_y + radius + 14, label);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx,
                      center_x - (int)solar_os_gfx_text_width(gfx, value) / 2,
                      center_y + radius + 28, value);
}

static void funcgen_render_graphics(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || funcgen.suspended) {
        return;
    }
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_gfx_text(gfx, 6, 18, "FUNCTION GENERATOR");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    char status[64];
    if (funcgen.last_error != ESP_OK) {
        snprintf(status, sizeof(status), "ERROR %s",
                 esp_err_to_name(funcgen.last_error));
    } else {
        snprintf(status, sizeof(status), "%s  %s",
                 funcgen.enabled ? "OUTPUT ON" : "OUTPUT OFF",
                 funcgen_output_label());
    }
    const int status_x = width - 6 - (int)solar_os_gfx_text_width(gfx, status);
    solar_os_gfx_text(gfx, status_x > 175 ? status_x : 175, 18, status);

    const int scope_y = 28;
    const int scope_height = height >= 280 ? 78 : 58;
    solar_os_oscilloscope_widget_draw(funcgen.scope, gfx, 6, scope_y,
                                      width - 12, scope_height);

    const int controls_top = scope_y + scope_height + 4;
    const int footer_y = height - 6;
    const int available = footer_y - controls_top - 4;
    const int row_height = available / 2;
    const int cell_width = (width - 12) / 4;
    int radius = row_height / 2 - 22;
    if (radius > 18)
        radius = 18;
    if (radius < 10)
        radius = 10;
    for (size_t i = 0U; i < FUNCGEN_CONTROL_COUNT; i++) {
        const int row = (int)(i / 4U);
        const int column = (int)(i % 4U);
        const int center_x = 6 + column * cell_width + cell_width / 2;
        const int center_y = controls_top + row * row_height + radius + 2;
        funcgen_draw_knob(gfx, center_x, center_y, radius,
                          (funcgen_control_t)i);
    }
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx, 6, footer_y,
                      "Left/Right select  Up/Down tune  Space output");
    solar_os_gfx_present(gfx);
    funcgen.redraw = false;
}

static void funcgen_render_tui(void)
{
    if (funcgen.suspended) {
        return;
    }
    solar_os_tui_clear(&funcgen.tui);
    char line[128];
    solar_os_tui_draw_title(&funcgen.tui, "FUNCTION GENERATOR",
                            funcgen.enabled ? "OUTPUT ON" : "OUTPUT OFF");
    for (size_t i = 0U; i < FUNCGEN_CONTROL_COUNT; i++) {
        char value[48];
        funcgen_control_value((funcgen_control_t)i, value, sizeof(value));
        snprintf(line, sizeof(line), "%-13s %s",
                 funcgen_control_label((funcgen_control_t)i), value);
        (void)solar_os_tui_addstr(&funcgen.tui, i + 2U, 2U, line,
                                  funcgen.selected == (funcgen_control_t)i
                                      ? SOLAR_OS_TUI_ATTR_INVERSE
                                      : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    solar_os_synth_status_t status;
    solar_os_synth_get_status(&status);
    snprintf(line, sizeof(line), "Stream: %s  Rate: %" PRIu32 " Hz%s%s",
             status.running && status.playback_stream[0] != '\0'
                 ? status.playback_stream
                 : funcgen_output_label(),
             status.sample_rate,
             funcgen.last_error != ESP_OK ? "  Error: " : "",
             funcgen.last_error != ESP_OK ? esp_err_to_name(funcgen.last_error)
                                          : "");
    (void)solar_os_tui_addstr(&funcgen.tui, FUNCGEN_CONTROL_COUNT + 3U, 2U,
                              line, SOLAR_OS_TUI_ATTR_NORMAL);
    solar_os_tui_draw_help(&funcgen.tui,
                           "Arrows select/tune  Space output  Esc exit");
    solar_os_tui_refresh(&funcgen.tui);
    funcgen.redraw = false;
}

static void funcgen_render(solar_os_context_t *ctx)
{
    if (funcgen.mode == FUNCGEN_MODE_GRAPHICS) {
        funcgen_render_graphics(ctx);
    } else {
        funcgen_render_tui();
    }
}

static uint32_t funcgen_step_frequency(uint32_t value, int direction)
{
    uint32_t step = value < 100U ? 1U : (value < 1000U ? 10U : 100U);
    if (direction > 0) {
        return value > FUNCGEN_FREQUENCY_MAX_HZ - step
                   ? FUNCGEN_FREQUENCY_MAX_HZ
                   : value + step;
    }
    return value < FUNCGEN_FREQUENCY_MIN_HZ + step ? FUNCGEN_FREQUENCY_MIN_HZ
                                                   : value - step;
}

static void funcgen_adjust_selected(int direction)
{
    switch (funcgen.selected) {
    case FUNCGEN_CONTROL_WAVEFORM:
        funcgen.waveform =
            (funcgen_waveform_t)((funcgen.waveform + FUNCGEN_WAVE_COUNT +
                                  direction) %
                                 FUNCGEN_WAVE_COUNT);
        break;
    case FUNCGEN_CONTROL_FREQUENCY:
        funcgen.frequency_hz =
            funcgen_step_frequency(funcgen.frequency_hz, direction);
        break;
    case FUNCGEN_CONTROL_AMPLITUDE:
        if (direction > 0 && funcgen.amplitude_percent < 100U) {
            funcgen.amplitude_percent++;
        } else if (direction < 0 && funcgen.amplitude_percent > 0U) {
            funcgen.amplitude_percent--;
        }
        break;
    case FUNCGEN_CONTROL_PULSE_WIDTH:
        if (direction > 0 && funcgen.pulse_width_percent < 99U) {
            funcgen.pulse_width_percent++;
        } else if (direction < 0 && funcgen.pulse_width_percent > 1U) {
            funcgen.pulse_width_percent--;
        }
        break;
    case FUNCGEN_CONTROL_SWEEP:
        funcgen.sweep_enabled = !funcgen.sweep_enabled;
        break;
    case FUNCGEN_CONTROL_SWEEP_END:
        funcgen.sweep_end_hz =
            funcgen_step_frequency(funcgen.sweep_end_hz, direction);
        break;
    case FUNCGEN_CONTROL_SWEEP_TIME:
        if (direction > 0 &&
            funcgen.sweep_time_ms <= FUNCGEN_SWEEP_TIME_MAX_MS - 100U) {
            funcgen.sweep_time_ms += 100U;
        } else if (direction < 0 &&
                   funcgen.sweep_time_ms >= FUNCGEN_SWEEP_TIME_MIN_MS + 100U) {
            funcgen.sweep_time_ms -= 100U;
        }
        break;
    case FUNCGEN_CONTROL_OUTPUT: {
        size_t index = funcgen.output_index;
        if (direction > 0) {
            index = (index + 1U) % funcgen.output_count;
        } else {
            index = index == 0U ? funcgen.output_count - 1U : index - 1U;
        }
        (void)funcgen_select_output(index);
        break;
    }
    default:
        break;
    }
    if (funcgen.selected != FUNCGEN_CONTROL_OUTPUT) {
        funcgen_sync_render(false);
    }
    funcgen.redraw = true;
}

static bool funcgen_handle_key(solar_os_context_t *ctx, uint8_t key)
{
    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE ||
        key == 'q' || key == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    switch (key) {
    case SOLAR_OS_KEY_LEFT:
        funcgen.selected = funcgen.selected == 0U
                               ? FUNCGEN_CONTROL_COUNT - 1U
                               : (funcgen_control_t)(funcgen.selected - 1U);
        break;
    case SOLAR_OS_KEY_RIGHT:
    case '\t':
        funcgen.selected = (funcgen_control_t)((funcgen.selected + 1U) %
                                               FUNCGEN_CONTROL_COUNT);
        break;
    case SOLAR_OS_KEY_UP:
    case '+':
    case '=':
        funcgen_adjust_selected(1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case '-':
        funcgen_adjust_selected(-1);
        break;
    case ' ':
        (void)funcgen_set_enabled(!funcgen.enabled);
        break;
    case '\r':
    case '\n':
        if (funcgen.selected == FUNCGEN_CONTROL_SWEEP ||
            funcgen.selected == FUNCGEN_CONTROL_OUTPUT ||
            funcgen.selected == FUNCGEN_CONTROL_WAVEFORM) {
            funcgen_adjust_selected(1);
        }
        break;
    default:
        return false;
    }
    funcgen.redraw = true;
    funcgen_render(ctx);
    return true;
}

static esp_err_t funcgen_start(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    bool force_tui = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "--tui") == 0) {
            force_tui = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    const funcgen_mode_t launch_mode =
        !force_tui && funcgen_graphical_session(ctx) ?
            FUNCGEN_MODE_GRAPHICS : FUNCGEN_MODE_TUI;
    solar_os_context_set_app_class(
        ctx,
        launch_mode == FUNCGEN_MODE_GRAPHICS ?
            SOLAR_OS_APP_CLASS_GUI : SOLAR_OS_APP_CLASS_TUI);

    memset(&funcgen, 0, sizeof(funcgen));
    memset(&funcgen_render_state, 0, sizeof(funcgen_render_state));
    funcgen.waveform = FUNCGEN_WAVE_SINE;
    funcgen.frequency_hz = FUNCGEN_FREQUENCY_DEFAULT_HZ;
    funcgen.sweep_end_hz = 2000U;
    funcgen.sweep_time_ms = FUNCGEN_SWEEP_TIME_DEFAULT_MS;
    funcgen.amplitude_percent = FUNCGEN_AMPLITUDE_DEFAULT_PERCENT;
    funcgen.pulse_width_percent = FUNCGEN_PULSE_DEFAULT_PERCENT;
    funcgen.last_error = ESP_OK;
    funcgen_discover_outputs();
    funcgen_sync_render(true);
    funcgen.mode = launch_mode;

    esp_err_t err;
    if (funcgen.mode == FUNCGEN_MODE_TUI) {
        err = solar_os_tui_screen_begin(&funcgen.tui, ctx);
    } else {
        err = solar_os_oscilloscope_widget_create(FUNCGEN_SCOPE_SAMPLES,
                                                  &funcgen.scope);
        if (err == ESP_OK) {
            funcgen_enable_high_refresh(ctx);
            solar_os_context_set_graphics_active(ctx, true);
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    funcgen.ui_started = true;
    funcgen.redraw = true;
    funcgen_parameters_register();
    funcgen_render(ctx);
    return ESP_OK;
}

static void funcgen_stop(solar_os_context_t *ctx)
{
    funcgen_parameters_unregister();
    (void)funcgen_set_enabled(false);
    if (funcgen.mode == FUNCGEN_MODE_GRAPHICS) {
        funcgen_disable_high_refresh();
        solar_os_oscilloscope_widget_destroy(funcgen.scope);
        funcgen.scope = NULL;
        solar_os_context_set_graphics_active(ctx, false);
    } else if (funcgen.ui_started) {
        solar_os_tui_set_cursor_visible(&funcgen.tui, true);
        solar_os_tui_refresh(&funcgen.tui);
        solar_os_tui_end(&funcgen.tui);
    }
    funcgen.ui_started = false;
}

static void funcgen_suspend(solar_os_context_t *ctx)
{
    funcgen.suspended = true;
    if (funcgen.mode == FUNCGEN_MODE_GRAPHICS) {
        funcgen_disable_high_refresh();
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static void funcgen_resume(solar_os_context_t *ctx)
{
    funcgen.suspended = false;
    if (funcgen.mode == FUNCGEN_MODE_GRAPHICS) {
        funcgen_enable_high_refresh(ctx);
        solar_os_context_set_graphics_active(ctx, true);
    }
    funcgen.redraw = true;
    funcgen_render(ctx);
}

static bool funcgen_event(solar_os_context_t *ctx,
                          const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        funcgen_resume(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (funcgen.redraw ||
            (funcgen.enabled && funcgen.mode == FUNCGEN_MODE_GRAPHICS)) {
            funcgen_render(ctx);
        }
        return true;
    }
    return event->type == SOLAR_OS_EVENT_CHAR
               ? funcgen_handle_key(ctx, (uint8_t)event->data.ch)
               : false;
}

const solar_os_app_t solar_os_funcgen_app = {
    .name = "funcgen",
    .summary = "audio function generator",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = funcgen_start,
    .suspend = funcgen_suspend,
    .resume = funcgen_resume,
    .stop = funcgen_stop,
    .event = funcgen_event,
    .state_slot = &funcgen_state,
    .state_size = sizeof(funcgen_cold_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = FUNCGEN_REFRESH_MS,
    .tick_deadline_ms = FUNCGEN_TICK_DEADLINE_MS,
};
