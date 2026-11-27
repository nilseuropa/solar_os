#include "solar_os_signal_widgets.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_dsp.h"
#include "solar_os_memory.h"

#define SIGNAL_WIDGET_MAX_SPECTRUM_BARS 32U
#define SIGNAL_WIDGET_PI 3.14159265358979323846
#define SIGNAL_WIDGET_SPECTRUM_DECAY_Q8 48U

typedef struct {
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    size_t capacity;
    int16_t *capture[2];
    size_t capture_count[2];
    uint8_t active;
} signal_capture_t;

struct solar_os_signal_widget {
    signal_capture_t capture;
    void *storage_allocation;
    int16_t *snapshot;
    uint32_t kind;
};

typedef struct {
    solar_os_signal_widget_t base;
    int16_t *window;
    int16_t *windowed;
    solar_os_dsp_complex_s16_t *spectrum;
    solar_os_dsp_fft_t *fft;
    uint16_t levels_q8[SIGNAL_WIDGET_MAX_SPECTRUM_BARS];
    size_t level_count;
} signal_spectrum_t;

static void *signal_aligned_storage(size_t bytes, const char *tag,
                                    void **allocation)
{
    *allocation = solar_os_memory_calloc(
        1U, bytes + 15U, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, tag);
    if (*allocation == NULL) {
        return NULL;
    }
    return (void *)(((uintptr_t)*allocation + 15U) & ~(uintptr_t)0x0fU);
}

static esp_err_t signal_capture_init(signal_capture_t *capture,
                                     size_t capacity,
                                     int16_t *first,
                                     int16_t *second)
{
    if (capture == NULL || capacity == 0U || first == NULL || second == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    capture->mutex = xSemaphoreCreateMutexStatic(&capture->mutex_storage);
    if (capture->mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    capture->capacity = capacity;
    capture->capture[0] = first;
    capture->capture[1] = second;
    return ESP_OK;
}

static int16_t signal_mono_sample(const int16_t *samples,
                                  size_t frame,
                                  uint8_t channels)
{
    int64_t sum = 0;
    for (uint8_t channel = 0U; channel < channels; channel++) {
        sum += samples[frame * channels + channel];
    }
    return (int16_t)(sum / channels);
}

static esp_err_t signal_capture_submit(signal_capture_t *capture,
                                       const int16_t *samples,
                                       size_t frames,
                                       uint8_t channels)
{
    if (capture == NULL || (frames > 0U && samples == NULL) || channels == 0U ||
        frames > SIZE_MAX / channels) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frames == 0U) {
        return ESP_OK;
    }

    xSemaphoreTake(capture->mutex, portMAX_DELAY);
    const uint8_t next = capture->active ^ 1U;
    const size_t count = frames < capture->capacity ? frames : capture->capacity;
    const size_t first = frames - count;
    for (size_t i = 0U; i < count; i++) {
        capture->capture[next][i] =
            signal_mono_sample(samples, first + i, channels);
    }
    capture->capture_count[next] = count;
    capture->active = next;
    xSemaphoreGive(capture->mutex);
    return ESP_OK;
}

static size_t signal_capture_snapshot(signal_capture_t *capture,
                                      int16_t *destination)
{
    if (capture == NULL || destination == NULL) {
        return 0U;
    }
    xSemaphoreTake(capture->mutex, portMAX_DELAY);
    const size_t count = capture->capture_count[capture->active];
    memcpy(destination,
           capture->capture[capture->active],
           count * sizeof(destination[0]));
    xSemaphoreGive(capture->mutex);
    return count;
}

static void signal_capture_reset(signal_capture_t *capture)
{
    if (capture == NULL || capture->mutex == NULL) {
        return;
    }
    xSemaphoreTake(capture->mutex, portMAX_DELAY);
    capture->capture_count[0] = 0U;
    capture->capture_count[1] = 0U;
    capture->active = 0U;
    xSemaphoreGive(capture->mutex);
}

static void signal_capture_destroy(signal_capture_t *capture)
{
    if (capture != NULL && capture->mutex != NULL) {
        vSemaphoreDelete(capture->mutex);
        capture->mutex = NULL;
    }
}

esp_err_t solar_os_signal_widget_create(uint32_t kind, size_t samples, solar_os_signal_widget_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    const bool spectrum = kind == SOLAR_OS_SIGNAL_SPECTRUM;
    const size_t buffers = spectrum ? 7U : 3U;
    if ((!spectrum && kind != SOLAR_OS_SIGNAL_WAVEFORM) || samples < 2U ||
        samples > (SIZE_MAX - 15U) / (buffers * sizeof(int16_t)) ||
        (spectrum && (samples < 64U || samples > SOLAR_OS_DSP_FFT_MAX_SIZE ||
                      (samples & (samples - 1U)) != 0U))) return ESP_ERR_INVALID_ARG;

    solar_os_signal_widget_t *widget = solar_os_memory_calloc(
        1U, spectrum ? sizeof(signal_spectrum_t) : sizeof(*widget),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, spectrum ? "widget.spectrum" : "widget.scope");
    if (!widget) return ESP_ERR_NO_MEM;
    widget->kind = kind;
    int16_t *storage = signal_aligned_storage(samples * buffers * sizeof(int16_t),
        spectrum ? "widget.spectrum.samples" : "widget.scope.samples", &widget->storage_allocation);
    if (!storage) {
        solar_os_signal_widget_destroy(widget);
        return ESP_ERR_NO_MEM;
    }
    widget->snapshot = storage + samples * 2U;
    esp_err_t error = signal_capture_init(&widget->capture, samples, storage, storage + samples);
    if (error == ESP_OK && spectrum) {
        signal_spectrum_t *fft = (signal_spectrum_t *)widget;
        fft->window = storage + samples * 3U;
        fft->windowed = storage + samples * 4U;
        fft->spectrum = (solar_os_dsp_complex_s16_t *)(storage + samples * 5U);
        error = solar_os_dsp_fft_create(samples, &fft->fft);
        if (error == ESP_OK) {
            for (size_t i = 0U; i < samples; i++) {
                const double phase = (2.0 * SIGNAL_WIDGET_PI * (double)i) / (double)(samples - 1U);
                fft->window[i] = (int16_t)lrint((0.5 - 0.5 * cos(phase)) * SOLAR_OS_DSP_Q15_ONE);
            }
        }
    }
    if (error != ESP_OK) {
        solar_os_signal_widget_destroy(widget);
        return error;
    }
    *out = widget;
    return ESP_OK;
}

void solar_os_signal_widget_destroy(solar_os_signal_widget_t *widget)
{
    if (!widget) return;
    if (widget->kind == SOLAR_OS_SIGNAL_SPECTRUM) {
        solar_os_dsp_fft_destroy(((signal_spectrum_t *)widget)->fft);
    }
    signal_capture_destroy(&widget->capture);
    solar_os_memory_free(widget->storage_allocation);
    solar_os_memory_free(widget);
}

void solar_os_signal_widget_reset(solar_os_signal_widget_t *widget)
{
    if (!widget) return;
    signal_capture_reset(&widget->capture);
    if (widget->kind == SOLAR_OS_SIGNAL_SPECTRUM) {
        signal_spectrum_t *fft = (signal_spectrum_t *)widget;
        memset(fft->levels_q8, 0, sizeof(fft->levels_q8));
        fft->level_count = 0U;
    }
}

esp_err_t solar_os_signal_widget_submit_s16(solar_os_signal_widget_t *widget,
    const int16_t *samples, size_t frames, uint8_t channels)
{
    if (!widget) return ESP_ERR_INVALID_ARG;
    return signal_capture_submit(&widget->capture, samples, frames, channels);
}

static void signal_draw_waveform(solar_os_signal_widget_t *widget,
    solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    const int middle = y + height / 2;
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DOTTED);
    solar_os_gfx_line(gfx, x + 2, middle, x + width - 3, middle);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);

    const size_t count = signal_capture_snapshot(
        &widget->capture, widget->snapshot);
    if (count > 1U) {
        uint32_t peak = 1U;
        for (size_t i = 0U; i < count; i++) {
            const int32_t sample = widget->snapshot[i];
            const uint32_t magnitude = sample < 0 ?
                (uint32_t)-sample : (uint32_t)sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        size_t trigger = 0U;
        for (size_t i = 1U; i < count / 2U; i++) {
            if (widget->snapshot[i - 1U] < 0 && widget->snapshot[i] >= 0) {
                trigger = i;
                break;
            }
        }
        const size_t visible = count - trigger;
        const int amplitude = height > 7 ? (height / 2) - 3 : 1;
        int previous_x = x + 2;
        int previous_y = middle -
            (int)(((int64_t)widget->snapshot[trigger] * amplitude) / peak);
        for (size_t i = 1U; i < visible; i++) {
            const int point_x = x + 2 +
                (int)(i * (size_t)(width - 5) / (visible - 1U));
            const int point_y = middle -
                (int)(((int64_t)widget->snapshot[trigger + i] * amplitude) / peak);
            solar_os_gfx_line(gfx, previous_x, previous_y, point_x, point_y);
            previous_x = point_x;
            previous_y = point_y;
        }
    }
}

static uint32_t signal_spectrum_magnitude(
    const solar_os_dsp_complex_s16_t *value)
{
    const int32_t real = value->real;
    const int32_t imaginary = value->imag;
    const uint32_t abs_real = real < 0 ? (uint32_t)-real : (uint32_t)real;
    const uint32_t abs_imaginary = imaginary < 0 ?
        (uint32_t)-imaginary : (uint32_t)imaginary;
    const uint32_t maximum = abs_real > abs_imaginary ? abs_real : abs_imaginary;
    const uint32_t minimum = abs_real > abs_imaginary ? abs_imaginary : abs_real;
    return maximum + minimum / 2U;
}

static uint16_t signal_spectrum_level_q8(uint32_t magnitude)
{
    if (magnitude == 0U) {
        return 0U;
    }
    unsigned exponent = 0U;
    uint32_t value = magnitude;
    while (value > 1U) {
        value >>= 1U;
        exponent++;
    }
    const uint32_t base = 1U << exponent;
    const uint32_t fraction = ((magnitude - base) << 8U) / base;
    const uint32_t level = exponent * 256U + fraction;
    return level > UINT16_MAX ? UINT16_MAX : (uint16_t)level;
}

static void signal_draw_spectrum(signal_spectrum_t *widget,
    solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    const size_t fft_size = solar_os_dsp_fft_size(widget->fft);
    const size_t count = signal_capture_snapshot(
        &widget->base.capture, widget->base.snapshot);
    if (count > 0U) {
        if (count < fft_size) {
            memmove(widget->base.snapshot + fft_size - count,
                    widget->base.snapshot,
                    count * sizeof(widget->base.snapshot[0]));
            memset(widget->base.snapshot,
                   0,
                   (fft_size - count) * sizeof(widget->base.snapshot[0]));
        }
        int64_t sum = 0;
        for (size_t i = 0U; i < fft_size; i++) {
            sum += widget->base.snapshot[i];
        }
        const int32_t mean = (int32_t)(sum / (int64_t)fft_size);
        for (size_t i = 0U; i < fft_size; i++) {
            int32_t value = (int32_t)widget->base.snapshot[i] - mean;
            if (value > INT16_MAX) {
                value = INT16_MAX;
            } else if (value < INT16_MIN) {
                value = INT16_MIN;
            }
            widget->base.snapshot[i] = (int16_t)value;
        }
        uint8_t exponent = 0U;
        if (solar_os_dsp_window_q15(widget->windowed,
                                    widget->base.snapshot,
                                    widget->window,
                                    fft_size) == ESP_OK &&
            solar_os_dsp_fft_execute(widget->fft,
                                     widget->spectrum,
                                     widget->windowed,
                                     &exponent) == ESP_OK) {
            (void)exponent;
            size_t bars = (size_t)(width - 5) / 5U;
            const size_t half = fft_size / 2U;
            if (bars > SIGNAL_WIDGET_MAX_SPECTRUM_BARS) {
                bars = SIGNAL_WIDGET_MAX_SPECTRUM_BARS;
            }
            if (bars > half - 1U) {
                bars = half - 1U;
            }
            if (bars != widget->level_count) {
                memset(widget->levels_q8, 0, sizeof(widget->levels_q8));
                widget->level_count = bars;
            }
            const size_t denominator = bars * bars;
            for (size_t bar = 0U; bar < bars; bar++) {
                size_t first = 1U +
                    ((half - 1U) * bar * bar) / denominator;
                size_t last = 1U +
                    ((half - 1U) * (bar + 1U) * (bar + 1U)) / denominator;
                if (last <= first) {
                    last = first + 1U;
                }
                if (last > half) {
                    last = half;
                }
                uint32_t magnitude = 0U;
                for (size_t bin = first; bin < last; bin++) {
                    const uint32_t candidate =
                        signal_spectrum_magnitude(&widget->spectrum[bin]);
                    if (candidate > magnitude) {
                        magnitude = candidate;
                    }
                }
                const uint16_t next = signal_spectrum_level_q8(magnitude);
                uint16_t shown = widget->levels_q8[bar];
                if (next >= shown) {
                    shown = next;
                } else if (shown > SIGNAL_WIDGET_SPECTRUM_DECAY_Q8) {
                    shown -= SIGNAL_WIDGET_SPECTRUM_DECAY_Q8;
                } else {
                    shown = 0U;
                }
                widget->levels_q8[bar] = shown;
            }
        }
    }

    const size_t bars = widget->level_count;
    const int plot_height = height - 5;
    const uint32_t full_scale_q8 = 15U * 256U;
    if (bars > 0U) {
        const int plot_width = width - 4;
        for (size_t bar = 0U; bar < bars; bar++) {
            int bar_height = (int)(((uint32_t)widget->levels_q8[bar] *
                                    (uint32_t)plot_height) / full_scale_q8);
            if (bar_height > plot_height) {
                bar_height = plot_height;
            }
            if (bar_height > 0) {
                const int left = x + 2 +
                    (int)((bar * (size_t)plot_width) / bars);
                const int right = x + 2 +
                    (int)(((bar + 1U) * (size_t)plot_width) / bars);
                const int bar_width = right - left;
                solar_os_gfx_fill_rect(gfx,
                                       left,
                                       y + height - 2 - bar_height,
                                       bar_width > 1 ? bar_width - 1 : 1,
                                       bar_height);
            }
        }
    }
}

void solar_os_signal_widget_draw(solar_os_signal_widget_t *widget,
    solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    if (!widget || !gfx) return;
    const bool spectrum = widget->kind == SOLAR_OS_SIGNAL_SPECTRUM;
    const int minimum = spectrum ? 8 : 5;
    if (width < minimum || height < minimum) return;

    const solar_os_gfx_color_t saved_color = solar_os_gfx_color(gfx);
    const solar_os_gfx_line_style_t saved_style = solar_os_gfx_line_style(gfx);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, height);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
    solar_os_gfx_rect(gfx, x, y, width, height);
    if (spectrum) {
        signal_draw_spectrum((signal_spectrum_t *)widget, gfx, x, y, width, height);
    } else {
        signal_draw_waveform(widget, gfx, x, y, width, height);
    }
    solar_os_gfx_set_line_style(gfx, saved_style);
    solar_os_gfx_set_color(gfx, saved_color);
}
