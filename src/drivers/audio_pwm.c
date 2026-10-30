#include "audio_pwm.h"

#include <stdbool.h>

#include "driver/gptimer.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "hal/ledc_ll.h"
#include "soc/soc_caps.h"

#define AUDIO_PWM_DUTY_RESOLUTION LEDC_TIMER_8_BIT
#define AUDIO_PWM_DUTY_MIDPOINT 128U
#define AUDIO_PWM_TIMER LEDC_TIMER_3
#define AUDIO_PWM_CHANNEL LEDC_CHANNEL_7
#if SOC_LEDC_SUPPORT_HS_MODE
#define AUDIO_PWM_MODE LEDC_HIGH_SPEED_MODE
#else
#define AUDIO_PWM_MODE LEDC_LOW_SPEED_MODE
#endif
#define AUDIO_PWM_GPTIMER_RESOLUTION_HZ 16000000U
#define AUDIO_PWM_GPTIMER_ALARM_TICKS \
    (AUDIO_PWM_GPTIMER_RESOLUTION_HZ / AUDIO_PWM_SAMPLE_RATE)
#define AUDIO_PWM_BUFFER_BYTES 512U
#define AUDIO_PWM_WRITE_CHUNK 256U
#define AUDIO_PWM_CLOSE_DRAIN_MS 40U

typedef struct {
    volatile bool active;
    gpio_num_t pin;
    gptimer_handle_t sample_timer;
    StreamBufferHandle_t samples;
    StaticStreamBuffer_t samples_storage;
    uint8_t samples_buffer[AUDIO_PWM_BUFFER_BYTES];
} audio_pwm_state_t;

static audio_pwm_state_t audio_pwm;

static void IRAM_ATTR audio_pwm_set_duty(uint8_t duty)
{
    ledc_dev_t *hw = LEDC_LL_GET_HW();
    if (hw->channel_group[AUDIO_PWM_MODE]
            .channel[AUDIO_PWM_CHANNEL]
            .conf1.duty_start) {
        return;
    }
    ledc_ll_set_duty_int_part(hw, AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL, duty);
    ledc_ll_set_duty_direction(hw,
                               AUDIO_PWM_MODE,
                               AUDIO_PWM_CHANNEL,
                               LEDC_DUTY_DIR_INCREASE);
    ledc_ll_set_duty_num(hw, AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL, 1U);
    ledc_ll_set_duty_cycle(hw, AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL, 1U);
    ledc_ll_set_duty_scale(hw, AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL, 0U);
    ledc_ll_set_duty_start(hw, AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL);
#if !SOC_LEDC_SUPPORT_HS_MODE
    ledc_ll_ls_channel_update(hw, AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL);
#endif
}

static bool IRAM_ATTR audio_pwm_on_sample(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *event,
    void *user)
{
    (void)timer;
    (void)event;
    audio_pwm_state_t *state = user;
    uint8_t duty = AUDIO_PWM_DUTY_MIDPOINT;
    BaseType_t task_woken = pdFALSE;
    if (state->active && state->samples != NULL) {
        (void)xStreamBufferReceiveFromISR(state->samples,
                                          &duty,
                                          sizeof(duty),
                                          &task_woken);
    }
    audio_pwm_set_duty(duty);
    return task_woken == pdTRUE;
}

static void audio_pwm_stop_timer(void)
{
    if (audio_pwm.sample_timer == NULL) {
        return;
    }
    (void)gptimer_stop(audio_pwm.sample_timer);
    (void)gptimer_disable(audio_pwm.sample_timer);
    (void)gptimer_del_timer(audio_pwm.sample_timer);
    audio_pwm.sample_timer = NULL;
}

esp_err_t audio_pwm_open(gpio_num_t pin)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (audio_pwm.active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_pwm.samples == NULL) {
        audio_pwm.samples = xStreamBufferCreateStatic(
            sizeof(audio_pwm.samples_buffer),
            1U,
            audio_pwm.samples_buffer,
            &audio_pwm.samples_storage);
        if (audio_pwm.samples == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    (void)xStreamBufferReset(audio_pwm.samples);

    const ledc_timer_config_t ledc_timer = {
        .speed_mode = AUDIO_PWM_MODE,
        .duty_resolution = AUDIO_PWM_DUTY_RESOLUTION,
        .timer_num = AUDIO_PWM_TIMER,
        .freq_hz = AUDIO_PWM_CARRIER_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer),
                        "audio_pwm",
                        "LEDC timer config failed");

    /*
     * ESP-IDF configures a channel's initial duty before binding the timer
     * requested by ledc_channel_config().  On classic ESP32 the channel still
     * points at unconfigured timer 0 here, so the duty-start handshake never
     * completes and ledc_update_duty() holds the spinlock until the interrupt
     * watchdog fires.  Bind our running carrier timer first.
     */
    ESP_RETURN_ON_ERROR(ledc_bind_channel_timer(AUDIO_PWM_MODE,
                                                AUDIO_PWM_CHANNEL,
                                                AUDIO_PWM_TIMER),
                        "audio_pwm",
                        "LEDC channel timer bind failed");

    const ledc_channel_config_t ledc_channel = {
        .gpio_num = pin,
        .speed_mode = AUDIO_PWM_MODE,
        .channel = AUDIO_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = AUDIO_PWM_TIMER,
        .duty = AUDIO_PWM_DUTY_MIDPOINT,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    };
    esp_err_t err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        return err;
    }

    const gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = AUDIO_PWM_GPTIMER_RESOLUTION_HZ,
    };
    err = gptimer_new_timer(&timer_config, &audio_pwm.sample_timer);
    if (err != ESP_OK) {
        (void)ledc_stop(AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL, 0U);
        return err;
    }
    const gptimer_event_callbacks_t callbacks = {
        .on_alarm = audio_pwm_on_sample,
    };
    err = gptimer_register_event_callbacks(audio_pwm.sample_timer,
                                           &callbacks,
                                           &audio_pwm);
    if (err == ESP_OK) {
        const gptimer_alarm_config_t alarm = {
            .alarm_count = AUDIO_PWM_GPTIMER_ALARM_TICKS,
            .reload_count = 0U,
            .flags.auto_reload_on_alarm = true,
        };
        err = gptimer_set_alarm_action(audio_pwm.sample_timer, &alarm);
    }
    if (err == ESP_OK) {
        err = gptimer_enable(audio_pwm.sample_timer);
    }
    if (err == ESP_OK) {
        audio_pwm.pin = pin;
        audio_pwm.active = true;
        err = gptimer_start(audio_pwm.sample_timer);
    }
    if (err != ESP_OK) {
        audio_pwm.active = false;
        audio_pwm_stop_timer();
        (void)ledc_stop(AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL, 0U);
        return err;
    }
    return ESP_OK;
}

esp_err_t audio_pwm_write_s16(const int16_t *samples,
                              size_t frames,
                              uint8_t channels,
                              uint8_t volume)
{
    if (samples == NULL || frames == 0U || channels == 0U || channels > 2U ||
        volume > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_pwm.active || audio_pwm.samples == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t duties[AUDIO_PWM_WRITE_CHUNK];
    size_t consumed = 0U;
    while (consumed < frames) {
        const size_t count = frames - consumed < AUDIO_PWM_WRITE_CHUNK ?
            frames - consumed : AUDIO_PWM_WRITE_CHUNK;
        for (size_t i = 0; i < count; i++) {
            const size_t frame = consumed + i;
            int32_t sample = samples[frame * channels];
            if (channels == 2U) {
                sample = (sample + samples[(frame * channels) + 1U]) / 2;
            }
            sample = (sample * volume) / 100;
            duties[i] = (uint8_t)((sample + 32768) >> 8);
        }
        /* Blocking sends wake on every sample interrupt. */
        size_t available;
        while ((available = xStreamBufferSpacesAvailable(audio_pwm.samples)) < count) {
            const TickType_t ticks = pdMS_TO_TICKS(
                (count - available) * 1000U / AUDIO_PWM_SAMPLE_RATE);
            vTaskDelay(ticks ? ticks : 1U);
        }
        const size_t sent = xStreamBufferSend(audio_pwm.samples,
                                              duties,
                                              count,
                                              0U);
        if (sent != count) {
            return ESP_ERR_INVALID_SIZE;
        }
        consumed += count;
    }
    return ESP_OK;
}

void audio_pwm_close(void)
{
    if (!audio_pwm.active && audio_pwm.sample_timer == NULL) {
        return;
    }
    if (audio_pwm.active && audio_pwm.samples != NULL) {
        TickType_t drain_ticks = pdMS_TO_TICKS(AUDIO_PWM_CLOSE_DRAIN_MS);
        if (drain_ticks == 0U) {
            drain_ticks = 1U;
        }
        for (TickType_t waited = 0U;
             waited < drain_ticks &&
             xStreamBufferBytesAvailable(audio_pwm.samples) != 0U;
             waited++) {
            vTaskDelay(1U);
        }
    }
    audio_pwm.active = false;
    audio_pwm_stop_timer();
    (void)ledc_set_duty(AUDIO_PWM_MODE,
                        AUDIO_PWM_CHANNEL,
                        AUDIO_PWM_DUTY_MIDPOINT);
    (void)ledc_update_duty(AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL);
    (void)ledc_stop(AUDIO_PWM_MODE, AUDIO_PWM_CHANNEL, 0U);
    if (audio_pwm.samples != NULL) {
        (void)xStreamBufferReset(audio_pwm.samples);
    }
}
