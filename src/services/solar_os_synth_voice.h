#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_synth.h"

const char *solar_os_synth_waveform_name(solar_os_synth_waveform_t waveform);
bool solar_os_synth_parse_waveform(const char *name,
                                   solar_os_synth_waveform_t *waveform);

/* Configuration applies immediately to active voices and to future notes. */
esp_err_t solar_os_synth_voice_configure(
    const char *owner,
    const solar_os_synth_voice_config_t *config);

/* Performance configuration applies to the shared voice allocator. */
esp_err_t solar_os_synth_voice_configure_performance(
    const char *owner,
    const solar_os_synth_voice_performance_t *performance);

/* note_on starts and claims the native synth lazily for owner. */
esp_err_t solar_os_synth_voice_note_on(const char *owner,
                                       uint32_t frequency_hz,
                                       uint8_t velocity);
esp_err_t solar_os_synth_voice_note_off(const char *owner,
                                        uint32_t frequency_hz);
esp_err_t solar_os_synth_voice_all_notes_off(const char *owner);
esp_err_t solar_os_synth_voice_stop(const char *owner);

/* The service copies the complete table before this call returns. */
esp_err_t solar_os_synth_voice_set_wavetable(const char *owner,
                                             const int16_t *samples,
                                             size_t sample_count);
esp_err_t solar_os_synth_voice_get_wavetable(int16_t *samples,
                                             size_t sample_count);
void solar_os_synth_voice_get_status(solar_os_synth_voice_status_t *status);
