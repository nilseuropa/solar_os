#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_native_client.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_synth.h"
#include "solar_os_memory.h"
#include "solar_os_synth.h"
#include "solar_os_synth_voice.h"

#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
static const solar_os_native_voice_api_v1_t *voice;
static solar_os_native_handle_t voice_owner;
static solar_os_synth_config_t render_config;
static solar_os_synth_status_t audio;
static solar_os_synth_voice_status_t musical;
static int16_t wavetable[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
static unsigned allocations, callbacks, library_pins, driver_pins;
static bool start_timeout, stop_timeout, attaching_driver;

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && *tag);
    void *memory = malloc(size);
    if (memory) ++allocations;
    return memory;
}

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    void *memory = solar_os_memory_alloc(count * size, cls, tag);
    if (memory) memset(memory, 0, count * size);
    return memory;
}

void solar_os_memory_free(void *memory)
{
    if (memory) { assert(allocations); --allocations; free(memory); }
}

int solar_os_native_library_retain(uint32_t lease, uint32_t *out)
{
    if (lease != 1) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = 2;
    ++library_pins;
    return SOLAR_OS_NATIVE_OK;
}

int solar_os_native_library_release(uint32_t lease)
{
    assert(lease == 2 && library_pins);
    --library_pins;
    return SOLAR_OS_NATIVE_OK;
}

void *solar_os_native_driver_pin(void)
{
    if (!attaching_driver) return NULL;
    ++driver_pins;
    return &driver_pins;
}

void solar_os_native_driver_unpin(void *pin)
{
    assert(pin == &driver_pins && driver_pins);
    --driver_pins;
}

esp_err_t solar_os_synth_start(const solar_os_synth_config_t *config)
{
    assert(config->block_frames == 64);
    if (audio.running) return ESP_ERR_INVALID_STATE;
    assert(config->render && (library_pins || driver_pins));
    strcpy(audio.owner, config->owner);
    if (config->playback_stream) strcpy(audio.playback_stream, config->playback_stream);
    audio.sample_rate = 16000;
    audio.running = true;
    render_config = *config;
    return start_timeout ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t solar_os_synth_stop(const char *owner)
{
    if (!audio.running) return ESP_OK;
    if (strcmp(audio.owner, owner)) return ESP_ERR_INVALID_STATE;
    if (stop_timeout) return ESP_ERR_TIMEOUT;
    int16_t output[128];
    render_config.render(output, 64, 16000, render_config.user);
    audio = (solar_os_synth_status_t){0};
    return ESP_OK;
}

void solar_os_synth_get_status(solar_os_synth_status_t *status) { *status = audio; }

static void render(int16_t *samples, size_t frames, uint32_t rate, void *user)
{
    assert(user == &callbacks && (library_pins || driver_pins));
    assert(frames == 64 && rate == 16000);
    samples[0] = 200;
    ++callbacks;
}

esp_err_t solar_os_synth_voice_stop(const char *owner)
{
    if (!musical.running) return ESP_OK;
    if (strcmp(owner, musical.owner)) return ESP_ERR_INVALID_STATE;
    if (stop_timeout) return ESP_ERR_TIMEOUT;
    musical.running = false;
    musical.owner[0] = 0;
    return ESP_OK;
}

void solar_os_synth_voice_get_status(solar_os_synth_voice_status_t *status) { *status = musical; }

esp_err_t solar_os_synth_voice_configure(const char *owner, const solar_os_synth_voice_config_t *config)
{
    if (musical.running && strcmp(owner, musical.owner)) return ESP_ERR_INVALID_STATE;
    assert(voice->close(voice_owner) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(voice->note_on(voice_owner, 440, 90) == SOLAR_OS_NATIVE_ERROR_BUSY);
    musical.config = *config;
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_configure_performance(const char *owner, const solar_os_synth_voice_performance_t *config)
{
    if (musical.running && strcmp(owner, musical.owner)) return ESP_ERR_INVALID_STATE;
    musical.performance = *config;
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_note_on(const char *owner, uint32_t frequency, uint8_t velocity)
{
    assert(frequency == 440 && velocity == 90);
    if (musical.running && strcmp(owner, musical.owner)) return ESP_ERR_INVALID_STATE;
    strcpy(musical.owner, owner);
    musical.running = true;
    musical.active_voices = 1;
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_note_off(const char *owner, uint32_t frequency)
{
    assert(frequency == 440 && !strcmp(owner, musical.owner));
    musical.active_voices = 0;
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_all_notes_off(const char *owner)
{
    return solar_os_synth_voice_note_off(owner, 440);
}

esp_err_t solar_os_synth_voice_set_wavetable(const char *owner, const int16_t *samples, size_t count)
{
    assert(!strcmp(owner, musical.owner) && count == sizeof(wavetable) / sizeof(wavetable[0]));
    memcpy(wavetable, samples, sizeof(wavetable));
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_get_wavetable(int16_t *samples, size_t count)
{
    assert(count == sizeof(wavetable) / sizeof(wavetable[0]));
    memcpy(samples, wavetable, sizeof(wavetable));
    return ESP_OK;
}

const char *solar_os_synth_waveform_name(solar_os_synth_waveform_t waveform)
{
    assert(waveform == SOLAR_OS_SYNTH_WAVE_SINE);
    return "sine";
}

bool solar_os_synth_parse_waveform(const char *name, solar_os_synth_waveform_t *waveform)
{
    if (strcmp(name, "sine")) return false;
    *waveform = SOLAR_OS_SYNTH_WAVE_SINE;
    return true;
}
#endif

int main(void)
{
    const solar_os_native_synth_api_v1_t *synth =
        solar_os_native_synth_get(solar_os_native_synth_get_service, sizeof(*synth));
#if SOLAR_OS_PACKAGE_SERVICE_SYNTH
    voice = solar_os_native_voice_get(solar_os_native_synth_get_service, sizeof(*voice));
    assert(synth && voice);
    solar_os_native_synth_config_v1_t config = {.playback_stream = "audio.fake",
        .render = render, .user = &callbacks, .block_frames = 64, .library_lease = 1};
    solar_os_native_handle_t handle, other;
    assert(!synth->open(&config, &handle) && handle && library_pins == 1);
    assert(synth->open(&config, &other) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!other && library_pins == 1 && audio.running);
    solar_os_synth_status_t status;
    assert(!synth->status(&status) && status.sample_rate == 16000 && !strcmp(status.playback_stream, "audio.fake"));
    assert(!synth->close(handle) && !library_pins && callbacks == 1);

    start_timeout = stop_timeout = true;
    assert(synth->open(&config, &handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && handle && library_pins == 1);
    assert(synth->close(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && library_pins == 1);
    start_timeout = stop_timeout = false;
    assert(!synth->close(handle) && !library_pins && callbacks == 2);
    config.library_lease = 0;
    assert(synth->open(&config, &handle) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !handle);
    attaching_driver = true;
    assert(!synth->open(&config, &handle) && driver_pins == 1);
    attaching_driver = false;
    assert(!synth->close(handle) && !driver_pins && callbacks == 3);

    assert(!voice->open(&voice_owner) && !voice->open(&other));
    assert(!voice->configure(voice_owner, &(solar_os_synth_voice_config_t){.sustain_percent = 60}));
    assert(!voice->configure_performance(voice_owner, &(solar_os_synth_voice_performance_t){.mono = true}));
    assert(!voice->note_on(voice_owner, 440, 90));
    assert(!voice->close(other) && musical.running);
    assert(synth->close(voice_owner) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    int16_t samples[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES] = {42};
    assert(!voice->set_wavetable(voice_owner, samples, SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES));
    samples[0] = 0;
    assert(!voice->get_wavetable(samples, SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES) && samples[0] == 42);
    solar_os_synth_waveform_t waveform;
    assert(voice->parse_waveform("sine", &waveform) && !strcmp(voice->waveform_name(waveform), "sine"));
    assert(!voice->note_off(voice_owner, 440) && !voice->all_notes_off(voice_owner));
    solar_os_synth_voice_status_t voices;
    assert(!voice->status(&voices) && voices.running && voices.performance.mono && !voices.active_voices);
    stop_timeout = true;
    assert(voice->close(voice_owner) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && musical.running);
    stop_timeout = false;
    assert(!voice->close(voice_owner) && !musical.running && !allocations && !solar_os_native_handle_count());
#else
    assert(!synth);
    assert(!solar_os_native_voice_get(solar_os_native_synth_get_service, sizeof(solar_os_native_voice_api_v1_t)));
#endif
    return 0;
}
