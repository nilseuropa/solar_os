#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef unsigned TickType_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_SIZE 3
#define AUDIO_PWM_SAMPLE_RATE 16000U
#define AUDIO_PWM_WRITE_CHUNK 256U
#define pdMS_TO_TICKS(ms) ((ms) * TICK_HZ / 1000U)

static struct { bool active; void *samples; } audio_pwm;
static uint8_t queue[512], played[8192];
static unsigned produced, consumed, delays;
static bool fail_send;

static size_t xStreamBufferSpacesAvailable(void *stream)
{
    assert(stream == queue);
    return sizeof(queue) - (produced - consumed);
}

static size_t xStreamBufferSend(void *stream, const void *data, size_t count, unsigned wait)
{
    assert(wait == 0U && count <= xStreamBufferSpacesAvailable(stream));
    if (fail_send) return 0;
    const uint8_t *bytes = data;
    for (unsigned i = 0; i < count; i++) queue[produced++ % sizeof(queue)] = bytes[i];
    return count;
}

static void drain(unsigned count)
{
    assert(count <= produced - consumed);
    for (unsigned i = 0; i < count; i++) {
        assert(consumed < sizeof(played));
        played[consumed] = queue[consumed % sizeof(queue)];
        ++consumed;
    }
}

static void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0U);
    ++delays;
    drain(ticks * AUDIO_PWM_SAMPLE_RATE / TICK_HZ);
}

#include "audio_pwm_write.inc"

int main(void)
{
    static int16_t pcm[8192 * 2];
    for (unsigned i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++)
        pcm[i] = (int16_t)(i * 4937U);
    audio_pwm.active = true;
    audio_pwm.samples = queue;
    for (unsigned channels = 1; channels <= 2; channels++) {
        for (unsigned volume = 0; volume <= 100; volume += 25) {
            produced = consumed = delays = 0;
            assert(audio_pwm_write_s16(pcm, 8192, channels, volume) == ESP_OK);
            drain(produced - consumed);
            assert(consumed == 8192 && delays < 8192 / 64);
            for (unsigned i = 0; i < consumed; i++) {
                int32_t sample = pcm[i * channels];
                if (channels == 2) sample = (sample + pcm[i * channels + 1]) / 2;
                assert(played[i] == (uint8_t)((sample * (int)volume / 100 + 32768) >> 8));
            }
        }
    }
    produced = consumed = delays = 0;
    fail_send = true;
    assert(audio_pwm_write_s16(pcm, 1, 1, 100) == ESP_ERR_INVALID_SIZE);
    fail_send = false;
    assert(audio_pwm_write_s16(NULL, 1, 1, 100) == ESP_ERR_INVALID_ARG);
    assert(audio_pwm_write_s16(pcm, 1, 3, 100) == ESP_ERR_INVALID_ARG);
    audio_pwm.active = false;
    assert(audio_pwm_write_s16(pcm, 1, 1, 100) == ESP_ERR_INVALID_STATE);
    puts("PWM audio: paced blocks preserve PCM order without underruns");
}
