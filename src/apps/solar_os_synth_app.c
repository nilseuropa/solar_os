#include "solar_os_synth_app.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_midi.h"
#include "solar_os_parameters.h"
#include "solar_os_storage.h"
#include "solar_os_synth_voice.h"

#define SYNTH_APP_OWNER "app:synth"
#define SYNTH_APP_PULSE_MS 220U
#define SYNTH_APP_STATUS_POLL_MS 250U
#define SYNTH_APP_COMPACT_VISUAL_QUIET_MS 250U
#define SYNTH_APP_HELD_MAX 16U
#define SYNTH_APP_OCTAVE_MIN 2
#define SYNTH_APP_OCTAVE_MAX 6
#define SYNTH_APP_VELOCITY_STEP 5
#define SYNTH_APP_VOLUME_STEP 5
#define SYNTH_APP_WAVE_STEP 1024
#define SYNTH_APP_WAVE_STEP_LARGE 4096
#define SYNTH_APP_BRUSH_MAX 16U
#define SYNTH_APP_DEFAULT_WAVE_STEPS 16U
#define SYNTH_APP_TWO_PI 6.28318530717958647692f
#define SYNTH_APP_PIANO_HEIGHT 63
#define SYNTH_APP_PIANO_BOTTOM_OFFSET 90
#define SYNTH_APP_FOOTER_HEIGHT 21
#define SYNTH_APP_FACTORY_PRESET_COUNT 8U
#define SYNTH_APP_USER_PRESET_COUNT 8U
#define SYNTH_APP_PRESET_WAVE_STEPS_MAX 64U
#define SYNTH_APP_PRESET_MAGIC 0x53595052UL
#define SYNTH_APP_PRESET_VERSION 1U
#define SYNTH_APP_PRESET_DIRECTORY ".solar/synth/presets"
#define SYNTH_APP_PRESET_MONO_FLAG 0x8000U
#define SYNTH_APP_PRESET_GLIDE_MASK 0x7fffU

typedef enum {
  SYNTH_TAB_PLAY = 0,
  SYNTH_TAB_FILTER,
  SYNTH_TAB_WAVE,
  SYNTH_TAB_OSCILLATOR2,
  SYNTH_TAB_MODE,
  SYNTH_TAB_PRESET,
  SYNTH_TAB_COUNT,
} synth_tab_t;

typedef enum {
  SYNTH_BASE_SQUARE = 0,
  SYNTH_BASE_TRIANGLE,
  SYNTH_BASE_SAW,
  SYNTH_BASE_SUPERSAW,
  SYNTH_BASE_SINE,
  SYNTH_BASE_FLAT,
  SYNTH_BASE_COUNT,
} synth_wave_baseline_t;

typedef enum {
  SYNTH_CONTROL_WAVE = 0,
  SYNTH_CONTROL_VOLUME,
  SYNTH_CONTROL_ATTACK,
  SYNTH_CONTROL_DECAY,
  SYNTH_CONTROL_SUSTAIN,
  SYNTH_CONTROL_RELEASE,
  SYNTH_CONTROL_COUNT,
} synth_control_t;

typedef enum {
  SYNTH_FILTER_CONTROL_CUTOFF = 0,
  SYNTH_FILTER_CONTROL_RESONANCE,
  SYNTH_FILTER_CONTROL_AMOUNT,
  SYNTH_FILTER_CONTROL_ATTACK,
  SYNTH_FILTER_CONTROL_DECAY,
  SYNTH_FILTER_CONTROL_SUSTAIN,
  SYNTH_FILTER_CONTROL_RELEASE,
  SYNTH_FILTER_CONTROL_COUNT,
} synth_filter_control_t;

typedef enum {
  SYNTH_OSCILLATOR2_CONTROL_WAVE = 0,
  SYNTH_OSCILLATOR2_CONTROL_OCTAVE,
  SYNTH_OSCILLATOR2_CONTROL_DETUNE,
  SYNTH_OSCILLATOR2_CONTROL_MIX,
  SYNTH_OSCILLATOR2_CONTROL_COUNT,
} synth_oscillator2_control_t;

typedef enum {
  SYNTH_MODE_CONTROL_VOICES = 0,
  SYNTH_MODE_CONTROL_HOLD,
  SYNTH_MODE_CONTROL_GLIDE,
  SYNTH_MODE_CONTROL_COUNT,
} synth_mode_control_t;

typedef enum {
  SYNTH_PRESET_SLOT_EMPTY = 0,
  SYNTH_PRESET_SLOT_SAVED,
  SYNTH_PRESET_SLOT_INVALID,
} synth_preset_slot_state_t;

typedef enum {
  SYNTH_PARAMETER_VOLUME = 0,
  SYNTH_PARAMETER_ATTACK,
  SYNTH_PARAMETER_DECAY,
  SYNTH_PARAMETER_SUSTAIN,
  SYNTH_PARAMETER_RELEASE,
  SYNTH_PARAMETER_FILTER_CUTOFF,
  SYNTH_PARAMETER_FILTER_RESONANCE,
  SYNTH_PARAMETER_FILTER_AMOUNT,
  SYNTH_PARAMETER_FILTER_ATTACK,
  SYNTH_PARAMETER_FILTER_DECAY,
  SYNTH_PARAMETER_FILTER_SUSTAIN,
  SYNTH_PARAMETER_FILTER_RELEASE,
  SYNTH_PARAMETER_OSC2_OCTAVE,
  SYNTH_PARAMETER_OSC2_DETUNE,
  SYNTH_PARAMETER_OSC2_MIX,
  SYNTH_PARAMETER_GLIDE,
  SYNTH_PARAMETER_COUNT,
} synth_parameter_t;

typedef struct {
  bool active;
  bool midi;
  bool release_pending;
  solar_os_input_source_t source;
  uint16_t physical_key;
  uint16_t usage;
  uint32_t frequency_hz;
  uint32_t release_at_ms;
  uint8_t semitone;
  uint8_t midi_channel;
  uint8_t midi_note;
} synth_held_note_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  solar_os_synth_voice_config_t config;
  uint8_t baseline;
  uint8_t wave_steps;
  uint16_t performance_flags;
  int16_t wavetable[SYNTH_APP_PRESET_WAVE_STEPS_MAX];
  uint32_t crc32;
} synth_preset_file_t;

typedef struct {
  solar_os_synth_voice_config_t config;
  solar_os_synth_voice_performance_t performance;
  synth_control_t selected;
  synth_filter_control_t filter_selected;
  synth_oscillator2_control_t oscillator2_selected;
  synth_mode_control_t mode_selected;
  synth_held_note_t held[SYNTH_APP_HELD_MAX];
  solar_os_midi_subscription_t midi_subscription;
  solar_os_parameter_registration_t parameter_registration;
  bool midi_subscribed;
  bool midi_sustain[16];
  int octave;
  uint8_t velocity;
  uint8_t volume;
  synth_tab_t tab;
  synth_wave_baseline_t baseline;
  synth_wave_baseline_t baseline_undo;
  size_t wave_cursor;
  uint8_t wave_brush;
  size_t wave_steps;
  size_t wave_steps_undo;
  int16_t wavetable[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
  int16_t wavetable_undo[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
  bool wavetable_undo_valid;
  size_t preset_selected;
  synth_preset_slot_state_t preset_slots[SYNTH_APP_USER_PRESET_COUNT];
  char preset_message[40];
  esp_err_t last_error;
  size_t last_active_voices;
  uint32_t last_deadline_misses;
  uint32_t last_pcm_generation;
  uint32_t last_status_poll_ms;
  bool last_running;
  bool keyboard_visible;
  bool hold_mode;
  bool headless;
  bool suspended;
  bool parameter_dirty;
  bool visual_dirty;
  uint32_t last_performance_ms;
  synth_parameter_t compact_parameter;
  bool compact_parameter_valid;
} synth_app_state_t;

static void *synth_app_state;
#define synth_app (*(synth_app_state_t *)synth_app_state)

static void synth_release_all(bool stop);

static esp_err_t synth_parameter_get(void *user, float *value) {
  if (value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  switch ((synth_parameter_t)(uintptr_t)user) {
  case SYNTH_PARAMETER_VOLUME:
    *value = (float)synth_app.volume;
    break;
  case SYNTH_PARAMETER_ATTACK:
    *value = (float)synth_app.config.attack_ms;
    break;
  case SYNTH_PARAMETER_DECAY:
    *value = (float)synth_app.config.decay_ms;
    break;
  case SYNTH_PARAMETER_SUSTAIN:
    *value = (float)synth_app.config.sustain_percent;
    break;
  case SYNTH_PARAMETER_RELEASE:
    *value = (float)synth_app.config.release_ms;
    break;
  case SYNTH_PARAMETER_FILTER_CUTOFF:
    *value = (float)synth_app.config.filter.cutoff_hz;
    break;
  case SYNTH_PARAMETER_FILTER_RESONANCE:
    *value = (float)synth_app.config.filter.resonance_percent;
    break;
  case SYNTH_PARAMETER_FILTER_AMOUNT:
    *value = (float)synth_app.config.filter.envelope_amount_percent;
    break;
  case SYNTH_PARAMETER_FILTER_ATTACK:
    *value = (float)synth_app.config.filter.attack_ms;
    break;
  case SYNTH_PARAMETER_FILTER_DECAY:
    *value = (float)synth_app.config.filter.decay_ms;
    break;
  case SYNTH_PARAMETER_FILTER_SUSTAIN:
    *value = (float)synth_app.config.filter.sustain_percent;
    break;
  case SYNTH_PARAMETER_FILTER_RELEASE:
    *value = (float)synth_app.config.filter.release_ms;
    break;
  case SYNTH_PARAMETER_OSC2_OCTAVE:
    *value = (float)synth_app.config.oscillator2.octave;
    break;
  case SYNTH_PARAMETER_OSC2_DETUNE:
    *value = (float)synth_app.config.oscillator2.detune_cents;
    break;
  case SYNTH_PARAMETER_OSC2_MIX:
    *value = (float)synth_app.config.oscillator2.mix_percent;
    break;
  case SYNTH_PARAMETER_GLIDE:
    *value = (float)synth_app.performance.glide_ms;
    break;
  default:
    return ESP_ERR_NOT_FOUND;
  }
  return ESP_OK;
}

static esp_err_t synth_parameter_set(void *user, float value) {
  const synth_parameter_t parameter = (synth_parameter_t)(uintptr_t)user;
  bool performance = false;
  switch (parameter) {
  case SYNTH_PARAMETER_VOLUME:
    synth_app.last_error = solar_os_audio_set_volume((uint8_t)lroundf(value));
    if (synth_app.last_error == ESP_OK) {
      synth_app.volume = (uint8_t)lroundf(value);
    }
    break;
  case SYNTH_PARAMETER_ATTACK:
    synth_app.config.attack_ms = (uint32_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_DECAY:
    synth_app.config.decay_ms = (uint32_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_SUSTAIN:
    synth_app.config.sustain_percent = (uint8_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_RELEASE:
    synth_app.config.release_ms = (uint32_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_FILTER_CUTOFF:
    synth_app.config.filter.cutoff_hz = (uint32_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_FILTER_RESONANCE:
    synth_app.config.filter.resonance_percent = (uint8_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_FILTER_AMOUNT:
    synth_app.config.filter.envelope_amount_percent = (uint8_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_FILTER_ATTACK:
    synth_app.config.filter.attack_ms = (uint32_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_FILTER_DECAY:
    synth_app.config.filter.decay_ms = (uint32_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_FILTER_SUSTAIN:
    synth_app.config.filter.sustain_percent = (uint8_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_FILTER_RELEASE:
    synth_app.config.filter.release_ms = (uint32_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_OSC2_OCTAVE:
    synth_app.config.oscillator2.octave = (int8_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_OSC2_DETUNE:
    synth_app.config.oscillator2.detune_cents = (int16_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_OSC2_MIX:
    synth_app.config.oscillator2.mix_percent = (uint8_t)lroundf(value);
    break;
  case SYNTH_PARAMETER_GLIDE:
    synth_app.performance.glide_ms = (uint16_t)lroundf(value);
    performance = true;
    break;
  default:
    return ESP_ERR_NOT_FOUND;
  }
  if (parameter != SYNTH_PARAMETER_VOLUME) {
    synth_app.last_error = performance ?
        solar_os_synth_voice_configure_performance(SYNTH_APP_OWNER,
                                                   &synth_app.performance) :
        solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  }
  if (synth_app.last_error == ESP_OK) {
    synth_app.compact_parameter = parameter;
    synth_app.compact_parameter_valid = true;
    synth_app.parameter_dirty = true;
  }
  return synth_app.last_error;
}

#define SYNTH_PARAMETER(name_, label_, unit_, min_, max_, step_, curve_, id_) \
  {                                                                            \
    .name = name_, .label = label_, .unit = unit_, .minimum = min_,            \
    .maximum = max_, .step = step_, .curve = curve_,                           \
    .get = synth_parameter_get, .set = synth_parameter_set,                    \
    .user = (void *)(uintptr_t)(id_),                                           \
  }

static const solar_os_parameter_definition_t synth_parameters[] = {
    SYNTH_PARAMETER("volume", "Volume", "%", 0.0f, 100.0f, 1.0f,
                    SOLAR_OS_PARAMETER_CURVE_LINEAR, SYNTH_PARAMETER_VOLUME),
    SYNTH_PARAMETER("envelope.attack", "Amp attack", "ms", 0.0f, 10000.0f,
                    1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_ATTACK),
    SYNTH_PARAMETER("envelope.decay", "Amp decay", "ms", 0.0f, 10000.0f,
                    1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_DECAY),
    SYNTH_PARAMETER("envelope.sustain", "Amp sustain", "%", 0.0f, 100.0f,
                    1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_SUSTAIN),
    SYNTH_PARAMETER("envelope.release", "Amp release", "ms", 0.0f, 10000.0f,
                    1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_RELEASE),
    SYNTH_PARAMETER("filter.cutoff", "Filter cutoff", "Hz", 40.0f, 18000.0f,
                    1.0f, SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC,
                    SYNTH_PARAMETER_FILTER_CUTOFF),
    SYNTH_PARAMETER("filter.resonance", "Filter resonance", "%", 0.0f,
                    100.0f, 1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_FILTER_RESONANCE),
    SYNTH_PARAMETER("filter.envelope.amount", "Filter envelope", "%", 0.0f,
                    100.0f, 1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_FILTER_AMOUNT),
    SYNTH_PARAMETER("filter.envelope.attack", "Filter attack", "ms", 0.0f,
                    10000.0f, 1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_FILTER_ATTACK),
    SYNTH_PARAMETER("filter.envelope.decay", "Filter decay", "ms", 0.0f,
                    10000.0f, 1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_FILTER_DECAY),
    SYNTH_PARAMETER("filter.envelope.sustain", "Filter sustain", "%", 0.0f,
                    100.0f, 1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_FILTER_SUSTAIN),
    SYNTH_PARAMETER("filter.envelope.release", "Filter release", "ms", 0.0f,
                    10000.0f, 1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_FILTER_RELEASE),
    SYNTH_PARAMETER("osc2.octave", "Oscillator 2 octave", "oct", -2.0f, 2.0f,
                    1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_OSC2_OCTAVE),
    SYNTH_PARAMETER("osc2.detune", "Oscillator 2 detune", "cent", -100.0f,
                    100.0f, 1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_OSC2_DETUNE),
    SYNTH_PARAMETER("osc2.mix", "Oscillator 2 mix", "%", 0.0f, 100.0f,
                    1.0f, SOLAR_OS_PARAMETER_CURVE_LINEAR,
                    SYNTH_PARAMETER_OSC2_MIX),
    SYNTH_PARAMETER("glide", "Glide", "ms", 0.0f, 2500.0f, 1.0f,
                    SOLAR_OS_PARAMETER_CURVE_LINEAR, SYNTH_PARAMETER_GLIDE),
};

#undef SYNTH_PARAMETER

static void synth_parameters_register(void) {
  synth_app.parameter_registration =
      (solar_os_parameter_registration_t)SOLAR_OS_PARAMETER_REGISTRATION_INIT;
  const esp_err_t err = solar_os_parameters_register(
      "synth", synth_parameters,
      sizeof(synth_parameters) / sizeof(synth_parameters[0]),
      &synth_app.parameter_registration);
  if (err != ESP_OK && synth_app.last_error == ESP_OK) {
    synth_app.last_error = err;
  }
}

static void synth_parameters_unregister(void) {
  if (synth_app.parameter_registration.token != 0U) {
    (void)solar_os_parameters_unregister(&synth_app.parameter_registration);
  }
}

static const uint16_t synth_envelope_values[] = {
    0,   5,   10,   20,   40,   80,   120,  180,  250,   350,
    500, 750, 1000, 1500, 2000, 3000, 5000, 7500, 10000,
};

static const uint16_t synth_filter_cutoff_values[] = {
    40,   60,   80,   120,  180,  250,  350,   500,   700,
    1000, 1500, 2200, 3200, 4800, 7000, 10000, 14000, 18000,
};

static const uint16_t synth_glide_values[] = {
    0, 20, 40, 80, 120, 180, 250, 350, 500, 750, 1000, 1500, 2000, 2500,
};

static const uint16_t synth_note_frequencies_octave_4[] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494, 523,
};

static const uint16_t synth_note_usages[] = {
    0x04U, /* A */
    0x1aU, /* W */
    0x16U, /* S */
    0x08U, /* E */
    0x07U, /* D */
    0x09U, /* F */
    0x17U, /* T */
    0x0aU, /* G */
    0x1cU, /* Y physical position; Z on a German keyboard */
    0x0bU, /* H */
    0x18U, /* U */
    0x0dU, /* J */
    0x0eU, /* K */
};

static const uint16_t synth_wavetable_step_counts[] = {16U, 32U, 64U};

static const char *const synth_factory_preset_names[] = {
    "ANALOG BASS",  "POLY BRASS", "SYNTH STRINGS", "DIGI E.PIANO",
    "DIGITAL BELL", "MONO LEAD",  "SEQ PLUCK",     "DREAM PAD",
};

static solar_os_synth_voice_config_t synth_default_config(void) {
  return (solar_os_synth_voice_config_t){
      .waveform = SOLAR_OS_SYNTH_WAVE_SQUARE,
      .attack_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_ATTACK_MS,
      .decay_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_DECAY_MS,
      .sustain_percent = SOLAR_OS_SYNTH_VOICE_DEFAULT_SUSTAIN_PERCENT,
      .release_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_RELEASE_MS,
      .oscillator2 =
          {
              .waveform = SOLAR_OS_SYNTH_WAVE_SQUARE,
              .octave = SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_OCTAVE,
              .detune_cents =
                  SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_DETUNE_CENTS,
              .mix_percent =
                  SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_MIX_PERCENT,
          },
      .filter =
          {
              .cutoff_hz = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_CUTOFF_HZ,
              .resonance_percent =
                  SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RESONANCE_PERCENT,
              .envelope_amount_percent =
                  SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ENVELOPE_AMOUNT_PERCENT,
              .attack_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ATTACK_MS,
              .decay_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_DECAY_MS,
              .sustain_percent =
                  SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_SUSTAIN_PERCENT,
              .release_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RELEASE_MS,
          },
  };
}

static solar_os_synth_voice_performance_t synth_default_performance(void) {
  return (solar_os_synth_voice_performance_t){
      .mono = SOLAR_OS_SYNTH_VOICE_DEFAULT_MONO,
      .glide_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_GLIDE_MS,
  };
}

static solar_os_synth_voice_config_t
synth_factory_preset_config(size_t index, synth_wave_baseline_t *baseline,
                            size_t *wave_steps,
                            solar_os_synth_voice_performance_t *performance) {
  solar_os_synth_voice_config_t config = synth_default_config();
  *performance = synth_default_performance();
  *baseline = SYNTH_BASE_SQUARE;
  *wave_steps = SYNTH_APP_DEFAULT_WAVE_STEPS;
  switch (index) {
  case 0:
    performance->mono = true;
    performance->glide_ms = 40U;
    config.waveform = SOLAR_OS_SYNTH_WAVE_SAW;
    config.attack_ms = 5U;
    config.decay_ms = 120U;
    config.sustain_percent = 65U;
    config.release_ms = 180U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    config.oscillator2.octave = -1;
    config.oscillator2.mix_percent = 40U;
    config.filter.cutoff_hz = 500U;
    config.filter.resonance_percent = 20U;
    config.filter.envelope_amount_percent = 70U;
    config.filter.attack_ms = 5U;
    config.filter.decay_ms = 250U;
    config.filter.sustain_percent = 15U;
    config.filter.release_ms = 180U;
    *baseline = SYNTH_BASE_SAW;
    break;
  case 1:
    config.waveform = SOLAR_OS_SYNTH_WAVE_SAW;
    config.attack_ms = 20U;
    config.decay_ms = 350U;
    config.sustain_percent = 75U;
    config.release_ms = 350U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    config.oscillator2.detune_cents = 7;
    config.oscillator2.mix_percent = 30U;
    config.filter.cutoff_hz = 1200U;
    config.filter.resonance_percent = 15U;
    config.filter.envelope_amount_percent = 70U;
    config.filter.attack_ms = 20U;
    config.filter.decay_ms = 500U;
    config.filter.sustain_percent = 50U;
    config.filter.release_ms = 350U;
    *baseline = SYNTH_BASE_SAW;
    break;
  case 2:
    config.waveform = SOLAR_OS_SYNTH_WAVE_SAW;
    config.attack_ms = 250U;
    config.decay_ms = 750U;
    config.sustain_percent = 85U;
    config.release_ms = 1500U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_SAW;
    config.oscillator2.detune_cents = 12;
    config.oscillator2.mix_percent = 45U;
    config.filter.cutoff_hz = 3200U;
    config.filter.resonance_percent = 10U;
    config.filter.envelope_amount_percent = 20U;
    config.filter.attack_ms = 500U;
    config.filter.decay_ms = 1000U;
    config.filter.sustain_percent = 70U;
    config.filter.release_ms = 1500U;
    *baseline = SYNTH_BASE_SAW;
    break;
  case 3:
    config.waveform = SOLAR_OS_SYNTH_WAVE_TRIANGLE;
    config.attack_ms = 5U;
    config.decay_ms = 750U;
    config.sustain_percent = 25U;
    config.release_ms = 500U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_SINE;
    config.oscillator2.octave = 1;
    config.oscillator2.detune_cents = 5;
    config.oscillator2.mix_percent = 30U;
    config.filter.cutoff_hz = 4800U;
    config.filter.resonance_percent = 10U;
    config.filter.envelope_amount_percent = 25U;
    config.filter.attack_ms = 5U;
    config.filter.decay_ms = 500U;
    config.filter.sustain_percent = 10U;
    config.filter.release_ms = 500U;
    *baseline = SYNTH_BASE_TRIANGLE;
    break;
  case 4:
    config.waveform = SOLAR_OS_SYNTH_WAVE_SINE;
    config.attack_ms = 0U;
    config.decay_ms = 1500U;
    config.sustain_percent = 0U;
    config.release_ms = 2000U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_TRIANGLE;
    config.oscillator2.octave = 2;
    config.oscillator2.detune_cents = 7;
    config.oscillator2.mix_percent = 45U;
    config.filter.cutoff_hz = 10000U;
    config.filter.resonance_percent = 25U;
    config.filter.envelope_amount_percent = 10U;
    config.filter.attack_ms = 0U;
    config.filter.decay_ms = 1000U;
    config.filter.sustain_percent = 0U;
    config.filter.release_ms = 2000U;
    *baseline = SYNTH_BASE_SINE;
    break;
  case 5:
    performance->mono = true;
    performance->glide_ms = 80U;
    config.waveform = SOLAR_OS_SYNTH_WAVE_SAW;
    config.attack_ms = 5U;
    config.decay_ms = 120U;
    config.sustain_percent = 70U;
    config.release_ms = 180U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    config.oscillator2.detune_cents = -12;
    config.oscillator2.mix_percent = 35U;
    config.filter.cutoff_hz = 2200U;
    config.filter.resonance_percent = 35U;
    config.filter.envelope_amount_percent = 40U;
    config.filter.attack_ms = 5U;
    config.filter.decay_ms = 250U;
    config.filter.sustain_percent = 30U;
    config.filter.release_ms = 180U;
    *baseline = SYNTH_BASE_SAW;
    break;
  case 6:
    config.waveform = SOLAR_OS_SYNTH_WAVE_SAW;
    config.attack_ms = 0U;
    config.decay_ms = 120U;
    config.sustain_percent = 0U;
    config.release_ms = 80U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    config.oscillator2.octave = 1;
    config.oscillator2.mix_percent = 20U;
    config.filter.cutoff_hz = 1500U;
    config.filter.resonance_percent = 25U;
    config.filter.envelope_amount_percent = 85U;
    config.filter.attack_ms = 0U;
    config.filter.decay_ms = 180U;
    config.filter.sustain_percent = 0U;
    config.filter.release_ms = 80U;
    *baseline = SYNTH_BASE_SAW;
    break;
  case 7:
    config.waveform = SOLAR_OS_SYNTH_WAVE_SINE;
    config.attack_ms = 1000U;
    config.decay_ms = 1500U;
    config.sustain_percent = 80U;
    config.release_ms = 3000U;
    config.oscillator2.waveform = SOLAR_OS_SYNTH_WAVE_TRIANGLE;
    config.oscillator2.detune_cents = 7;
    config.oscillator2.mix_percent = 45U;
    config.filter.cutoff_hz = 2200U;
    config.filter.resonance_percent = 15U;
    config.filter.envelope_amount_percent = 30U;
    config.filter.attack_ms = 1000U;
    config.filter.decay_ms = 2000U;
    config.filter.sustain_percent = 65U;
    config.filter.release_ms = 3000U;
    *baseline = SYNTH_BASE_SINE;
    break;
  default:
    break;
  }
  return config;
}

static uint32_t synth_preset_crc32(const void *data, size_t length) {
  const uint8_t *bytes = data;
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0; i < length; i++) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8U; bit++) {
      crc = (crc & 1U) != 0U ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
    }
  }
  return ~crc;
}

static bool synth_wave_steps_valid(size_t wave_steps) {
  for (size_t i = 0; i < sizeof(synth_wavetable_step_counts) /
                             sizeof(synth_wavetable_step_counts[0]);
       i++) {
    if (wave_steps == synth_wavetable_step_counts[i]) {
      return true;
    }
  }
  return false;
}

static bool
synth_preset_config_valid(const solar_os_synth_voice_config_t *config) {
  return config != NULL && config->waveform >= SOLAR_OS_SYNTH_WAVE_SQUARE &&
         config->waveform <= SOLAR_OS_SYNTH_WAVE_CUSTOM &&
         config->attack_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
         config->decay_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
         config->sustain_percent <= 100U &&
         config->release_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
         config->oscillator2.waveform >= SOLAR_OS_SYNTH_WAVE_SQUARE &&
         config->oscillator2.waveform <= SOLAR_OS_SYNTH_WAVE_CUSTOM &&
         config->oscillator2.octave >=
             SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN &&
         config->oscillator2.octave <=
             SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX &&
         config->oscillator2.detune_cents >=
             SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS &&
         config->oscillator2.detune_cents <=
             SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS &&
         config->oscillator2.mix_percent <= 100U &&
         config->filter.cutoff_hz >=
             SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ &&
         config->filter.cutoff_hz <=
             SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MAX_HZ &&
         config->filter.resonance_percent <= 100U &&
         config->filter.envelope_amount_percent <= 100U &&
         config->filter.attack_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
         config->filter.decay_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
         config->filter.sustain_percent <= 100U &&
         config->filter.release_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS;
}

static uint16_t synth_preset_pack_performance(
    const solar_os_synth_voice_performance_t *performance) {
  return (performance->mono ? SYNTH_APP_PRESET_MONO_FLAG : 0U) |
         (performance->glide_ms & SYNTH_APP_PRESET_GLIDE_MASK);
}

static solar_os_synth_voice_performance_t
synth_preset_unpack_performance(uint16_t flags) {
  return (solar_os_synth_voice_performance_t){
      .mono = (flags & SYNTH_APP_PRESET_MONO_FLAG) != 0U,
      .glide_ms = flags & SYNTH_APP_PRESET_GLIDE_MASK,
  };
}

static bool synth_preset_performance_valid(uint16_t flags) {
  return (flags & SYNTH_APP_PRESET_GLIDE_MASK) <=
         SOLAR_OS_SYNTH_VOICE_GLIDE_MAX_MS;
}

static bool synth_preset_record_valid(const synth_preset_file_t *preset) {
  return preset != NULL && preset->magic == SYNTH_APP_PRESET_MAGIC &&
         preset->version == SYNTH_APP_PRESET_VERSION &&
         preset->size == sizeof(*preset) &&
         preset->baseline < SYNTH_BASE_COUNT &&
         synth_wave_steps_valid(preset->wave_steps) &&
         synth_preset_config_valid(&preset->config) &&
         synth_preset_performance_valid(preset->performance_flags) &&
         preset->crc32 ==
             synth_preset_crc32(preset, offsetof(synth_preset_file_t, crc32));
}

static esp_err_t synth_preset_slot_path(size_t slot, const char *suffix,
                                        char *path, size_t path_len) {
  if (slot >= SYNTH_APP_USER_PRESET_COUNT || suffix == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  char relative[SOLAR_OS_STORAGE_PATH_MAX];
  const int written =
      snprintf(relative, sizeof(relative), "%s/slot%02u.syp%s",
               SYNTH_APP_PRESET_DIRECTORY, (unsigned)(slot + 1U), suffix);
  if (written < 0 || (size_t)written >= sizeof(relative)) {
    return ESP_ERR_INVALID_SIZE;
  }
  return solar_os_storage_default_path(relative, path, path_len);
}

static esp_err_t synth_preset_read_slot(size_t slot,
                                        synth_preset_file_t *preset) {
  if (preset == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  char path[SOLAR_OS_STORAGE_PATH_MAX];
  esp_err_t err = synth_preset_slot_path(slot, "", path, sizeof(path));
  if (err != ESP_OK) {
    return err;
  }
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
  }
  const bool valid_size = fread(preset, sizeof(*preset), 1, file) == 1 &&
                          fgetc(file) == EOF && !ferror(file);
  if (fclose(file) != 0 || !valid_size) {
    return ESP_ERR_INVALID_SIZE;
  }
  return synth_preset_record_valid(preset) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void synth_preset_scan_slots(void) {
  for (size_t slot = 0; slot < SYNTH_APP_USER_PRESET_COUNT; slot++) {
    synth_preset_file_t preset;
    const esp_err_t err = synth_preset_read_slot(slot, &preset);
    synth_app.preset_slots[slot] = err == ESP_OK ? SYNTH_PRESET_SLOT_SAVED
                                   : err == ESP_ERR_NOT_FOUND
                                       ? SYNTH_PRESET_SLOT_EMPTY
                                       : SYNTH_PRESET_SLOT_INVALID;
  }
}

static esp_err_t synth_preset_ensure_directory(void) {
  static const char *const directories[] = {
      ".solar",
      ".solar/synth",
      SYNTH_APP_PRESET_DIRECTORY,
  };
  char path[SOLAR_OS_STORAGE_PATH_MAX];
  for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
    esp_err_t err =
        solar_os_storage_default_path(directories[i], path, sizeof(path));
    if (err != ESP_OK) {
      return err;
    }
    if (solar_os_storage_mkdir(path) != ESP_OK && errno != EEXIST) {
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

static uint32_t synth_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *synth_baseline_name(synth_wave_baseline_t baseline) {
  switch (baseline) {
  case SYNTH_BASE_SQUARE:
    return "SQUARE";
  case SYNTH_BASE_TRIANGLE:
    return "TRIANGLE";
  case SYNTH_BASE_SAW:
    return "SAW";
  case SYNTH_BASE_SUPERSAW:
    return "SUPERSAW";
  case SYNTH_BASE_SINE:
    return "SINE";
  case SYNTH_BASE_FLAT:
    return "FLAT";
  default:
    return "?";
  }
}

static const char *synth_wave_short_name(solar_os_synth_waveform_t waveform) {
  switch (waveform) {
  case SOLAR_OS_SYNTH_WAVE_SQUARE:
    return "SQR";
  case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
    return "TRI";
  case SOLAR_OS_SYNTH_WAVE_SAW:
    return "SAW";
  case SOLAR_OS_SYNTH_WAVE_SINE:
    return "SINE";
  case SOLAR_OS_SYNTH_WAVE_NOISE:
    return "NOISE";
  case SOLAR_OS_SYNTH_WAVE_CUSTOM:
    return "USER";
  default:
    return "?";
  }
}

static int16_t synth_baseline_sample(synth_wave_baseline_t baseline,
                                     size_t index, size_t sample_count) {
  const uint32_t phase = (uint32_t)(((uint64_t)index << 32) / sample_count);
  switch (baseline) {
  case SYNTH_BASE_SQUARE:
    return index < sample_count / 2U ? -32767 : 32767;
  case SYNTH_BASE_TRIANGLE:
    return phase < 0x80000000U
               ? (int16_t)(-32767 + (int32_t)(phase >> 15))
               : (int16_t)(32767 - (int32_t)((phase - 0x80000000U) >> 15));
  case SYNTH_BASE_SAW:
    return (int16_t)((int32_t)(phase >> 16) - 32768);
  case SYNTH_BASE_SUPERSAW: {
    static const int32_t phase_offsets[] = {
        -0x0c000000, -0x08000000, -0x04000000, 0,
        0x04000000,  0x08000000,  0x0c000000,
    };
    int32_t accumulated = 0;
    for (size_t i = 0; i < sizeof(phase_offsets) / sizeof(phase_offsets[0]);
         i++) {
      const uint32_t shifted = phase + (uint32_t)phase_offsets[i];
      accumulated += (int32_t)(shifted >> 16) - 32768;
    }
    return (int16_t)(accumulated / (int32_t)(sizeof(phase_offsets) /
                                             sizeof(phase_offsets[0])));
  }
  case SYNTH_BASE_SINE:
    return (
        int16_t)(sinf(SYNTH_APP_TWO_PI * (float)index / (float)sample_count) *
                 32767.0f);
  case SYNTH_BASE_FLAT:
  default:
    return 0;
  }
}

static int16_t synth_wavetable_interpolate(const int16_t *table,
                                           size_t sample_count,
                                           size_t phase_numerator,
                                           size_t phase_denominator) {
  const size_t scaled = phase_numerator;
  const size_t index = (scaled / phase_denominator) % sample_count;
  const size_t next = (index + 1U) % sample_count;
  const int32_t fraction = (int32_t)(scaled % phase_denominator);
  const int32_t denominator = (int32_t)phase_denominator;
  return (int16_t)(((int32_t)table[index] * (denominator - fraction) +
                    (int32_t)table[next] * fraction) /
                   denominator);
}

static uint8_t synth_wavetable_brush_max(void) {
  size_t maximum = (synth_app.wave_steps - 1U) / 2U;
  if (maximum > SYNTH_APP_BRUSH_MAX) {
    maximum = SYNTH_APP_BRUSH_MAX;
  }
  return (uint8_t)maximum;
}

static void synth_wavetable_snapshot(void) {
  memcpy(synth_app.wavetable_undo, synth_app.wavetable,
         sizeof(synth_app.wavetable));
  synth_app.baseline_undo = synth_app.baseline;
  synth_app.wave_steps_undo = synth_app.wave_steps;
  synth_app.wavetable_undo_valid = true;
}

static esp_err_t synth_wavetable_upload_values(const int16_t *wavetable,
                                               size_t wave_steps) {
  int16_t playback_table[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
  for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES; i++) {
    playback_table[i] =
        synth_wavetable_interpolate(wavetable, wave_steps, i * wave_steps,
                                    SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES);
  }
  return solar_os_synth_voice_set_wavetable(
      SYNTH_APP_OWNER, playback_table, SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES);
}

static esp_err_t synth_wavetable_upload(void) {
  return synth_wavetable_upload_values(synth_app.wavetable,
                                       synth_app.wave_steps);
}

static void synth_wavetable_commit(void) {
  esp_err_t err = synth_wavetable_upload();
  if (err == ESP_OK) {
    synth_app.config.waveform = SOLAR_OS_SYNTH_WAVE_CUSTOM;
    err = solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  }
  synth_app.last_error = err;
}

static void synth_wavetable_fill_values(int16_t *wavetable, size_t wave_steps,
                                        synth_wave_baseline_t baseline) {
  memset(wavetable, 0, sizeof(*wavetable) * SYNTH_APP_PRESET_WAVE_STEPS_MAX);
  int32_t peak = 0;
  for (size_t i = 0; i < wave_steps; i++) {
    wavetable[i] = synth_baseline_sample(baseline, i, wave_steps);
    int32_t magnitude = wavetable[i];
    if (magnitude < 0) {
      magnitude = -magnitude;
    }
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  if (baseline == SYNTH_BASE_SUPERSAW && peak > 0 && peak < 32767) {
    for (size_t i = 0; i < wave_steps; i++) {
      wavetable[i] = (int16_t)(((int32_t)wavetable[i] * 32767) / peak);
    }
  }
}

static void synth_wavetable_fill(synth_wave_baseline_t baseline) {
  synth_app.baseline = baseline;
  synth_wavetable_fill_values(synth_app.wavetable, synth_app.wave_steps,
                              baseline);
}

static void synth_wavetable_seed(synth_wave_baseline_t baseline,
                                 bool remember) {
  if (remember) {
    synth_wavetable_snapshot();
  }
  synth_wavetable_fill(baseline);
  synth_wavetable_commit();
}

static esp_err_t
synth_preset_apply(const solar_os_synth_voice_config_t *config,
                   const solar_os_synth_voice_performance_t *performance,
                   synth_wave_baseline_t baseline, size_t wave_steps,
                   const int16_t *wavetable) {
  if (!synth_preset_config_valid(config) || performance == NULL ||
      performance->glide_ms > SOLAR_OS_SYNTH_VOICE_GLIDE_MAX_MS ||
      baseline >= SYNTH_BASE_COUNT || !synth_wave_steps_valid(wave_steps) ||
      wavetable == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (synth_app.performance.mono != performance->mono) {
    synth_release_all(false);
  }
  esp_err_t err = synth_wavetable_upload_values(wavetable, wave_steps);
  if (err == ESP_OK) {
    err = solar_os_synth_voice_configure(SYNTH_APP_OWNER, config);
  }
  if (err == ESP_OK) {
    err = solar_os_synth_voice_configure_performance(SYNTH_APP_OWNER,
                                                     performance);
  }
  if (err != ESP_OK) {
    return err;
  }
  synth_app.config = *config;
  synth_app.performance = *performance;
  synth_app.baseline = baseline;
  synth_app.wave_steps = wave_steps;
  memset(synth_app.wavetable, 0, sizeof(synth_app.wavetable));
  memcpy(synth_app.wavetable, wavetable,
         wave_steps * sizeof(synth_app.wavetable[0]));
  synth_app.wave_cursor = wave_steps / 4U;
  if (synth_app.wave_brush > synth_wavetable_brush_max()) {
    synth_app.wave_brush = synth_wavetable_brush_max();
  }
  synth_app.wavetable_undo_valid = false;
  synth_app.last_error = ESP_OK;
  return ESP_OK;
}

static esp_err_t synth_preset_load_factory(size_t index) {
  if (index >= SYNTH_APP_FACTORY_PRESET_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }
  synth_wave_baseline_t baseline;
  size_t wave_steps;
  solar_os_synth_voice_performance_t performance;
  const solar_os_synth_voice_config_t config =
      synth_factory_preset_config(index, &baseline, &wave_steps, &performance);
  int16_t wavetable[SYNTH_APP_PRESET_WAVE_STEPS_MAX];
  synth_wavetable_fill_values(wavetable, wave_steps, baseline);
  const esp_err_t err = synth_preset_apply(&config, &performance, baseline,
                                           wave_steps, wavetable);
  snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
           err == ESP_OK ? "loaded %s" : "load failed: %s",
           err == ESP_OK ? synth_factory_preset_names[index]
                         : esp_err_to_name(err));
  return err;
}

static esp_err_t synth_preset_load_user(size_t slot) {
  synth_preset_file_t preset;
  esp_err_t err = synth_preset_read_slot(slot, &preset);
  if (err == ESP_OK) {
    const solar_os_synth_voice_performance_t performance =
        synth_preset_unpack_performance(preset.performance_flags);
    err = synth_preset_apply(&preset.config, &performance,
                             (synth_wave_baseline_t)preset.baseline,
                             preset.wave_steps, preset.wavetable);
  }
  if (err == ESP_ERR_NOT_FOUND) {
    snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
             "user %u is empty", (unsigned)(slot + 1U));
  } else if (err == ESP_OK) {
    snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
             "loaded user %u", (unsigned)(slot + 1U));
  } else {
    snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
             "load failed: %s", esp_err_to_name(err));
  }
  synth_app.preset_slots[slot] = err == ESP_OK ? SYNTH_PRESET_SLOT_SAVED
                                 : err == ESP_ERR_NOT_FOUND
                                     ? SYNTH_PRESET_SLOT_EMPTY
                                     : SYNTH_PRESET_SLOT_INVALID;
  return err;
}

static esp_err_t synth_preset_write_user(size_t slot) {
  if (slot >= SYNTH_APP_USER_PRESET_COUNT) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = synth_preset_ensure_directory();
  if (err != ESP_OK) {
    return err;
  }
  synth_preset_file_t preset;
  memset(&preset, 0, sizeof(preset));
  preset.magic = SYNTH_APP_PRESET_MAGIC;
  preset.version = SYNTH_APP_PRESET_VERSION;
  preset.size = sizeof(preset);
  preset.config = synth_app.config;
  preset.baseline = (uint8_t)synth_app.baseline;
  preset.wave_steps = (uint8_t)synth_app.wave_steps;
  preset.performance_flags =
      synth_preset_pack_performance(&synth_app.performance);
  memcpy(preset.wavetable, synth_app.wavetable,
         synth_app.wave_steps * sizeof(preset.wavetable[0]));
  preset.crc32 =
      synth_preset_crc32(&preset, offsetof(synth_preset_file_t, crc32));

  char path[SOLAR_OS_STORAGE_PATH_MAX];
  char temporary[SOLAR_OS_STORAGE_PATH_MAX];
  char backup[SOLAR_OS_STORAGE_PATH_MAX];
  if (synth_preset_slot_path(slot, "", path, sizeof(path)) != ESP_OK ||
      synth_preset_slot_path(slot, ".tmp", temporary, sizeof(temporary)) !=
          ESP_OK ||
      synth_preset_slot_path(slot, ".bak", backup, sizeof(backup)) != ESP_OK) {
    return ESP_ERR_INVALID_SIZE;
  }
  (void)solar_os_storage_remove(temporary);
  FILE *file = fopen(temporary, "wb");
  if (file == NULL) {
    return ESP_FAIL;
  }
  const bool write_failed = fwrite(&preset, sizeof(preset), 1, file) != 1 ||
                            fflush(file) != 0 || fsync(fileno(file)) != 0;
  if (fclose(file) != 0 || write_failed) {
    (void)solar_os_storage_remove(temporary);
    return ESP_FAIL;
  }

  (void)solar_os_storage_remove(backup);
  bool had_previous = false;
  if (solar_os_storage_rename(path, backup) == ESP_OK) {
    had_previous = true;
  } else if (errno != ENOENT) {
    (void)solar_os_storage_remove(temporary);
    return ESP_FAIL;
  }
  err = solar_os_storage_rename(temporary, path);
  if (err != ESP_OK) {
    if (had_previous) {
      (void)solar_os_storage_rename(backup, path);
    }
    (void)solar_os_storage_remove(temporary);
    return err;
  }
  (void)solar_os_storage_remove(backup);
  return ESP_OK;
}

static void synth_preset_load_selected(void) {
  if (synth_app.preset_selected < SYNTH_APP_FACTORY_PRESET_COUNT) {
    (void)synth_preset_load_factory(synth_app.preset_selected);
  } else {
    (void)synth_preset_load_user(synth_app.preset_selected -
                                 SYNTH_APP_FACTORY_PRESET_COUNT);
  }
}

static void synth_preset_save_selected(void) {
  if (synth_app.preset_selected < SYNTH_APP_FACTORY_PRESET_COUNT) {
    snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
             "factory presets are read-only");
    return;
  }
  const size_t slot =
      synth_app.preset_selected - SYNTH_APP_FACTORY_PRESET_COUNT;
  const esp_err_t err = synth_preset_write_user(slot);
  if (err == ESP_OK) {
    synth_app.preset_slots[slot] = SYNTH_PRESET_SLOT_SAVED;
    snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
             "saved user %u", (unsigned)(slot + 1U));
  } else {
    synth_preset_file_t existing;
    const esp_err_t read_error = synth_preset_read_slot(slot, &existing);
    synth_app.preset_slots[slot] =
        read_error == ESP_OK              ? SYNTH_PRESET_SLOT_SAVED
        : read_error == ESP_ERR_NOT_FOUND ? SYNTH_PRESET_SLOT_EMPTY
                                          : SYNTH_PRESET_SLOT_INVALID;
    snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
             "save failed: %s", esp_err_to_name(err));
  }
}

static void synth_wavetable_draw(int direction, bool large_step) {
  synth_wavetable_snapshot();
  const int32_t delta = direction * (large_step ? SYNTH_APP_WAVE_STEP_LARGE
                                                : SYNTH_APP_WAVE_STEP);
  const int radius = synth_app.wave_brush;
  const int divisor = radius + 1;
  for (int offset = -radius; offset <= radius; offset++) {
    int index = (int)synth_app.wave_cursor + offset;
    while (index < 0) {
      index += (int)synth_app.wave_steps;
    }
    index %= (int)synth_app.wave_steps;
    const int weight = divisor - (offset < 0 ? -offset : offset);
    int32_t sample = synth_app.wavetable[index] + delta * weight / divisor;
    if (sample > 32767) {
      sample = 32767;
    } else if (sample < -32767) {
      sample = -32767;
    }
    synth_app.wavetable[index] = (int16_t)sample;
  }
  synth_wavetable_commit();
}

static void synth_wavetable_smooth(void) {
  synth_wavetable_snapshot();
  for (size_t i = 0; i < synth_app.wave_steps; i++) {
    const size_t previous =
        (i + synth_app.wave_steps - 1U) % synth_app.wave_steps;
    const size_t next = (i + 1U) % synth_app.wave_steps;
    synth_app.wavetable[i] =
        (int16_t)(((int32_t)synth_app.wavetable_undo[previous] +
                   synth_app.wavetable_undo[i] +
                   synth_app.wavetable_undo[next]) /
                  3);
  }
  synth_wavetable_commit();
}

static void synth_wavetable_normalize(void) {
  int32_t peak = 0;
  for (size_t i = 0; i < synth_app.wave_steps; i++) {
    int32_t magnitude = synth_app.wavetable[i];
    if (magnitude < 0) {
      magnitude = -magnitude;
    }
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  if (peak == 0 || peak == 32767) {
    return;
  }
  synth_wavetable_snapshot();
  for (size_t i = 0; i < synth_app.wave_steps; i++) {
    synth_app.wavetable[i] =
        (int16_t)(((int32_t)synth_app.wavetable[i] * 32767) / peak);
  }
  synth_wavetable_commit();
}

static void synth_wavetable_undo(void) {
  if (!synth_app.wavetable_undo_valid) {
    return;
  }
  for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES; i++) {
    const int16_t sample = synth_app.wavetable[i];
    synth_app.wavetable[i] = synth_app.wavetable_undo[i];
    synth_app.wavetable_undo[i] = sample;
  }
  const synth_wave_baseline_t baseline = synth_app.baseline;
  synth_app.baseline = synth_app.baseline_undo;
  synth_app.baseline_undo = baseline;
  const size_t old_steps = synth_app.wave_steps;
  synth_app.wave_steps = synth_app.wave_steps_undo;
  synth_app.wave_steps_undo = old_steps;
  synth_app.wave_cursor =
      synth_app.wave_cursor * synth_app.wave_steps / old_steps;
  if (synth_app.wave_cursor >= synth_app.wave_steps) {
    synth_app.wave_cursor = synth_app.wave_steps - 1U;
  }
  const uint8_t brush_max = synth_wavetable_brush_max();
  if (synth_app.wave_brush > brush_max) {
    synth_app.wave_brush = brush_max;
  }
  synth_wavetable_commit();
}

static void synth_wavetable_cycle_steps(void) {
  synth_wavetable_snapshot();
  const size_t old_steps = synth_app.wave_steps_undo;
  const size_t step_count = sizeof(synth_wavetable_step_counts) /
                            sizeof(synth_wavetable_step_counts[0]);
  size_t selected = 0;
  while (selected < step_count &&
         synth_wavetable_step_counts[selected] != old_steps) {
    selected++;
  }
  if (selected == step_count) {
    selected = step_count - 1U;
  }
  const size_t new_steps =
      synth_wavetable_step_counts[(selected + 1U) % step_count];
  memset(synth_app.wavetable, 0, sizeof(synth_app.wavetable));
  for (size_t i = 0; i < new_steps; i++) {
    synth_app.wavetable[i] = synth_wavetable_interpolate(
        synth_app.wavetable_undo, old_steps, i * old_steps, new_steps);
  }
  synth_app.wave_steps = new_steps;
  synth_app.wave_cursor = synth_app.wave_cursor * new_steps / old_steps;
  if (synth_app.wave_cursor >= new_steps) {
    synth_app.wave_cursor = new_steps - 1U;
  }
  const uint8_t brush_max = synth_wavetable_brush_max();
  if (synth_app.wave_brush > brush_max) {
    synth_app.wave_brush = brush_max;
  }
  synth_wavetable_commit();
}

static uint32_t synth_note_frequency(uint8_t semitone) {
  uint32_t frequency = synth_note_frequencies_octave_4[semitone];
  if (synth_app.octave > 4) {
    frequency <<= (unsigned)(synth_app.octave - 4);
  } else if (synth_app.octave < 4) {
    frequency >>= (unsigned)(4 - synth_app.octave);
  }
  return frequency;
}

static int synth_semitone_for_usage(uint16_t usage) {
  for (size_t i = 0;
       i < sizeof(synth_note_usages) / sizeof(synth_note_usages[0]); i++) {
    if (synth_note_usages[i] == usage) {
      return (int)i;
    }
  }
  return -1;
}

static int synth_semitone_for_char(uint8_t key) {
  if (key >= 'A' && key <= 'Z') {
    key = (uint8_t)(key - 'A' + 'a');
  }
  switch (key) {
  case 'a':
    return 0;
  case 'w':
    return 1;
  case 's':
    return 2;
  case 'e':
    return 3;
  case 'd':
    return 4;
  case 'f':
    return 5;
  case 't':
    return 6;
  case 'g':
    return 7;
  case 'y':
  case 'z':
    return 8;
  case 'h':
    return 9;
  case 'u':
    return 10;
  case 'j':
    return 11;
  case 'k':
    return 12;
  default:
    return -1;
  }
}

static int synth_semitone_for_key(const solar_os_input_key_event_t *key) {
  const int by_usage = synth_semitone_for_usage(key->usage);
  return by_usage >= 0 ? by_usage : synth_semitone_for_char(key->key);
}

static bool synth_semitone_held(uint8_t semitone) {
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    if (synth_app.held[i].active && synth_app.held[i].semitone == semitone) {
      return true;
    }
  }
  return false;
}

static void synth_draw_wave_icon(solar_os_gfx_t *gfx, int x, int y, int width,
                                 int height, solar_os_synth_waveform_t waveform,
                                 const solar_os_synth_voice_status_t *status) {
  const int left = x;
  const int right = x + width;
  const int top = y;
  const int middle = y + height / 2;
  const int bottom = y + height;

  if (status != NULL && status->pcm_sample_count > 1U &&
      status->pcm_waveform == waveform) {
    int32_t scope_peak = (int32_t)status->pcm_peak;
    if (scope_peak < 1) {
      scope_peak = 1;
    }
    const int scope_amplitude = height > 3 ? (height / 2) - 1 : 1;
    for (size_t i = 1; i < status->pcm_sample_count; i++) {
      const int x0 = left + (int)((i - 1U) * (size_t)width /
                                  (status->pcm_sample_count - 1U));
      const int x1 =
          left + (int)(i * (size_t)width / (status->pcm_sample_count - 1U));
      const int y0 =
          middle -
          ((int32_t)status->pcm_samples[i - 1U] * scope_amplitude) / scope_peak;
      const int y1 =
          middle -
          ((int32_t)status->pcm_samples[i] * scope_amplitude) / scope_peak;
      solar_os_gfx_line(gfx, x0, y0, x1, y1);
    }
    return;
  }

  switch (waveform) {
  case SOLAR_OS_SYNTH_WAVE_SQUARE:
    solar_os_gfx_line(gfx, left, bottom, left + width / 4, bottom);
    solar_os_gfx_line(gfx, left + width / 4, bottom, left + width / 4, top);
    solar_os_gfx_line(gfx, left + width / 4, top, left + (3 * width) / 4, top);
    solar_os_gfx_line(gfx, left + (3 * width) / 4, top, left + (3 * width) / 4,
                      bottom);
    solar_os_gfx_line(gfx, left + (3 * width) / 4, bottom, right, bottom);
    break;
  case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
    solar_os_gfx_line(gfx, left, middle, left + width / 4, top);
    solar_os_gfx_line(gfx, left + width / 4, top, left + (3 * width) / 4,
                      bottom);
    solar_os_gfx_line(gfx, left + (3 * width) / 4, bottom, right, middle);
    break;
  case SOLAR_OS_SYNTH_WAVE_SAW:
    solar_os_gfx_line(gfx, left, bottom, right - 1, top);
    solar_os_gfx_line(gfx, right - 1, top, right - 1, bottom);
    break;
  case SOLAR_OS_SYNTH_WAVE_SINE: {
    int previous_x = left;
    int previous_y = middle;
    for (int column = 1; column <= width; column++) {
      const int point_x = left + column;
      const int point_y =
          middle - (int)(sinf(SYNTH_APP_TWO_PI * (float)column / (float)width) *
                         (float)(height / 2));
      solar_os_gfx_line(gfx, previous_x, previous_y, point_x, point_y);
      previous_x = point_x;
      previous_y = point_y;
    }
    break;
  }
  case SOLAR_OS_SYNTH_WAVE_CUSTOM:
    for (int column = 1; column <= width; column++) {
      const int16_t previous = synth_wavetable_interpolate(
          synth_app.wavetable, synth_app.wave_steps,
          (size_t)(column - 1) * synth_app.wave_steps, (size_t)width);
      const int16_t current = synth_wavetable_interpolate(
          synth_app.wavetable, synth_app.wave_steps,
          (size_t)column * synth_app.wave_steps, (size_t)width);
      const int y0 = middle - (int32_t)previous * height / (2 * 32767);
      const int y1 = middle - (int32_t)current * height / (2 * 32767);
      solar_os_gfx_line(gfx, left + column - 1, y0, left + column, y1);
    }
    break;
  case SOLAR_OS_SYNTH_WAVE_NOISE: {
    const int points[] = {middle,  top,        bottom, middle - 4, bottom,
                          top + 3, middle + 2, top,    bottom};
    const size_t count = sizeof(points) / sizeof(points[0]);
    for (size_t i = 1; i < count; i++) {
      const int x0 = left + (int)((i - 1) * (size_t)width / (count - 1));
      const int x1 = left + (int)(i * (size_t)width / (count - 1));
      solar_os_gfx_line(gfx, x0, points[i - 1], x1, points[i]);
    }
    break;
  }
  default:
    break;
  }
}

static size_t synth_envelope_value_index(uint32_t value) {
  size_t best = 0;
  uint32_t best_distance = UINT32_MAX;
  for (size_t i = 0;
       i < sizeof(synth_envelope_values) / sizeof(synth_envelope_values[0]);
       i++) {
    const uint32_t candidate = synth_envelope_values[i];
    const uint32_t distance =
        candidate > value ? candidate - value : value - candidate;
    if (distance < best_distance) {
      best = i;
      best_distance = distance;
    }
  }
  return best;
}

static size_t synth_filter_cutoff_value_index(uint32_t value) {
  size_t best = 0;
  uint32_t best_distance = UINT32_MAX;
  for (size_t i = 0; i < sizeof(synth_filter_cutoff_values) /
                             sizeof(synth_filter_cutoff_values[0]);
       i++) {
    const uint32_t candidate = synth_filter_cutoff_values[i];
    const uint32_t distance =
        candidate > value ? candidate - value : value - candidate;
    if (distance < best_distance) {
      best = i;
      best_distance = distance;
    }
  }
  return best;
}

static void synth_draw_knob(solar_os_gfx_t *gfx, int center_x, int center_y,
                            int radius, const char *label, const char *value,
                            unsigned position, bool selected) {
  static const int8_t indicator_x[] = {-7, -10, -10, -8, -4, 0,
                                       4,  8,   10,  10, 7};
  static const int8_t indicator_y[] = {7, 4, 0, -5, -9, -10, -9, -5, 0, 4, 7};
  if (position > 10U) {
    position = 10U;
  }

  solar_os_gfx_circle(gfx, center_x, center_y, radius);
  if (selected) {
    solar_os_gfx_circle(gfx, center_x, center_y, radius + 3);
  }
  const int scale = radius > 18 ? radius - 7 : radius / 2;
  solar_os_gfx_line(gfx, center_x, center_y,
                    center_x + indicator_x[position] * scale / 10,
                    center_y + indicator_y[position] * scale / 10);

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  const int label_x = center_x - (int)solar_os_gfx_text_width(gfx, label) / 2;
  solar_os_gfx_text(gfx, label_x, center_y + radius + 15, label);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  const int value_x = center_x - (int)solar_os_gfx_text_width(gfx, value) / 2;
  solar_os_gfx_text(gfx, value_x, center_y + radius + 29, value);
}

static void synth_draw_envelope(solar_os_gfx_t *gfx, int x, int y, int width,
                                int height, const char *title,
                                uint32_t attack_ms, uint32_t decay_ms,
                                uint8_t sustain_percent, uint32_t release_ms) {
  const int left = x + 10;
  const int right = x + width - 10;
  const int top = y + 23;
  const int bottom = y + height - 8;
  const int graph_width = right - left;
  const int graph_height = bottom - top;
  const uint32_t attack_weight =
      (uint32_t)synth_envelope_value_index(attack_ms) + 1U;
  const uint32_t decay_weight =
      (uint32_t)synth_envelope_value_index(decay_ms) + 1U;
  const uint32_t sustain_weight = 8U;
  const uint32_t release_weight =
      (uint32_t)synth_envelope_value_index(release_ms) + 1U;
  const uint32_t total_weight =
      attack_weight + decay_weight + sustain_weight + release_weight;
  const int attack_x =
      left + (int)((uint32_t)graph_width * attack_weight / total_weight);
  const int decay_x =
      left + (int)((uint32_t)graph_width * (attack_weight + decay_weight) /
                   total_weight);
  const int sustain_x =
      left +
      (int)((uint32_t)graph_width *
            (attack_weight + decay_weight + sustain_weight) / total_weight);
  const int sustain_y = bottom - graph_height * (int)sustain_percent / 100;

  solar_os_gfx_rect(gfx, x, y, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, x + 8, y + 16, title);
  solar_os_gfx_line(gfx, left, bottom, attack_x, top);
  solar_os_gfx_line(gfx, attack_x, top, decay_x, sustain_y);
  solar_os_gfx_line(gfx, decay_x, sustain_y, sustain_x, sustain_y);
  solar_os_gfx_line(gfx, sustain_x, sustain_y, right, bottom);
}

static void synth_draw_volume_button(solar_os_gfx_t *gfx, int x, int y,
                                     int width, int height, bool selected) {
  char value[12];
  snprintf(value, sizeof(value), "%u%%", (unsigned)synth_app.volume);
  solar_os_gfx_rect(gfx, x, y, width, height);
  if (selected) {
    solar_os_gfx_rect(gfx, x + 3, y + 3, width - 6, height - 6);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  int text_x = x + (width - (int)solar_os_gfx_text_width(gfx, "VOLUME")) / 2;
  solar_os_gfx_text(gfx, text_x, y + height / 2 - 6, "VOLUME");
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  text_x = x + (width - (int)solar_os_gfx_text_width(gfx, value)) / 2;
  solar_os_gfx_text(gfx, text_x, y + height / 2 + 12, value);
}

static void synth_draw_piano(solar_os_gfx_t *gfx, int x, int y, int width,
                             int height) {
  static const uint8_t white_semitones[] = {0, 2, 4, 5, 7, 9, 11, 12};
  static const char *const white_labels[] = {"A", "S", "D", "F",
                                             "G", "H", "J", "K"};
  static const int8_t black_after_white[] = {0, 1, -1, 2, 3, 4, -1};
  static const uint8_t black_semitones[] = {1, 3, 6, 8, 10};
  static const char *const black_labels[] = {"W", "E", "T", "Y/Z", "U"};
  const int white_width = width / 8;

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  for (size_t i = 0; i < 8; i++) {
    const int key_x = x + (int)i * white_width;
    const int key_width = i == 7 ? x + width - key_x : white_width;
    if (synth_semitone_held(white_semitones[i])) {
      solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
      solar_os_gfx_fill_rect(gfx, key_x + 1, y + 1, key_width - 1, height - 1);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, key_x, y, key_width, height);
  }

  for (size_t i = 0; i < sizeof(black_after_white); i++) {
    if (black_after_white[i] < 0) {
      continue;
    }
    const int black_index = black_after_white[i];
    const int key_x = x + ((int)i + 1) * white_width - white_width / 4;
    const int key_width = white_width / 2;
    const int key_height = (height * 3) / 5;
    const bool held = synth_semitone_held(black_semitones[black_index]);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, key_x, y, key_width, key_height);
    if (held) {
      solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
      solar_os_gfx_fill_rect(gfx, key_x + 3, y + 3, key_width - 6,
                             key_height - 6);
      solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
      solar_os_gfx_rect(gfx, key_x + 3, y + 3, key_width - 6, key_height - 6);
    }
    solar_os_gfx_set_color(gfx, held ? SOLAR_OS_GFX_COLOR_BLACK
                                     : SOLAR_OS_GFX_COLOR_WHITE);
    const char *label = black_labels[black_index];
    const int label_x =
        key_x + (key_width - (int)solar_os_gfx_text_width(gfx, label)) / 2;
    solar_os_gfx_text(gfx, label_x, y + key_height - 5, label);
  }

  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  for (size_t i = 0; i < 8; i++) {
    const int key_x = x + (int)i * white_width;
    const int key_width = i == 7 ? x + width - key_x : white_width;
    const char *label = white_labels[i];
    const int label_x =
        key_x + (key_width - (int)solar_os_gfx_text_width(gfx, label)) / 2;
    solar_os_gfx_text(gfx, label_x, y + height - 5, label);
  }
}

static int synth_editor_bottom(int height) {
  return height - (synth_app.keyboard_visible
                       ? SYNTH_APP_PIANO_BOTTOM_OFFSET
                       : SYNTH_APP_FOOTER_HEIGHT);
}

static int synth_visualizer_extra_height(void) {
  return synth_app.keyboard_visible
             ? 0
             : (SYNTH_APP_PIANO_BOTTOM_OFFSET - SYNTH_APP_FOOTER_HEIGHT) / 2;
}

static void synth_draw_keyboard(solar_os_gfx_t *gfx, int width, int height) {
  if (synth_app.keyboard_visible) {
    synth_draw_piano(gfx, 6, height - SYNTH_APP_PIANO_BOTTOM_OFFSET, width - 12,
                     SYNTH_APP_PIANO_HEIGHT);
  }
}

static void synth_draw_header(solar_os_gfx_t *gfx,
                              const solar_os_synth_voice_status_t *status) {
  const int width = (int)solar_os_gfx_width(gfx);
  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
  solar_os_gfx_text(gfx, 6, 18, "SYNTH");

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  const bool compact = width < 380;
  const char *tabs = "1P 2F 3W 4O 5G 6R";
  if (synth_app.tab == SYNTH_TAB_PLAY) {
    tabs = "[1P] 2F 3W 4O 5G 6R";
  } else if (synth_app.tab == SYNTH_TAB_FILTER) {
    tabs = "1P [2F] 3W 4O 5G 6R";
  } else if (synth_app.tab == SYNTH_TAB_WAVE) {
    tabs = "1P 2F [3W] 4O 5G 6R";
  } else if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
    tabs = "1P 2F 3W [4O] 5G 6R";
  } else if (synth_app.tab == SYNTH_TAB_MODE) {
    tabs = "1P 2F 3W 4O [5G] 6R";
  } else if (synth_app.tab == SYNTH_TAB_PRESET) {
    tabs = "1P 2F 3W 4O 5G [6R]";
  }
  solar_os_gfx_text(gfx, 62, 17, tabs);

  char header[64];
  const esp_err_t display_error = synth_app.last_error != ESP_OK
                                      ? synth_app.last_error
                                      : status->last_error;
  if (display_error != ESP_OK) {
    snprintf(header, sizeof(header), "audio: %s",
             esp_err_to_name(display_error));
  } else if (compact) {
    snprintf(header, sizeof(header), "o%d v%u %uv", synth_app.octave,
             (unsigned)synth_app.velocity, (unsigned)status->active_voices);
  } else {
    snprintf(header, sizeof(header), "o%d v%u %uv %uHz m%u", synth_app.octave,
             (unsigned)synth_app.velocity, (unsigned)status->active_voices,
             (unsigned)status->sample_rate,
             (unsigned)status->render_deadline_misses);
  }
  solar_os_gfx_text(gfx, compact ? 194 : 202, 17, header);
}

static void synth_draw_wave_editor(solar_os_gfx_t *gfx, int width, int height) {
  const int graph_x = 6;
  const int graph_y = 34;
  const int graph_width = width - 12;
  const int editor_bottom = synth_editor_bottom(height);
  int graph_height = editor_bottom - graph_y - 22;
  if (graph_height < 80) {
    graph_height = 80;
  }
  const int left = graph_x + 8;
  const int right = graph_x + graph_width - 8;
  const int top = graph_y + 27;
  const int bottom = graph_y + graph_height - 10;
  const int middle = (top + bottom) / 2;
  const int amplitude = (bottom - top) / 2;

  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  solar_os_gfx_rect(gfx, graph_x, graph_y, graph_width, graph_height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, graph_x + 8, graph_y + 17, "WAVETABLE");
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  char graph_status[48];
  snprintf(graph_status, sizeof(graph_status), "%s b%u [ENTER %u]",
           synth_baseline_name(synth_app.baseline),
           (unsigned)synth_app.wave_brush, (unsigned)synth_app.wave_steps);
  const int status_x = graph_x + graph_width - 8 -
                       (int)solar_os_gfx_text_width(gfx, graph_status);
  solar_os_gfx_text(gfx, status_x, graph_y + 17, graph_status);
  solar_os_gfx_line(gfx, left, middle, right, middle);

  int previous_x = left;
  int previous_y = middle - (int32_t)synth_app.wavetable[0] * amplitude / 32767;
  for (size_t i = 1; i <= synth_app.wave_steps; i++) {
    const int x =
        left + (int)(i * (size_t)(right - left) / synth_app.wave_steps);
    const int y =
        middle - (int32_t)synth_app.wavetable[i % synth_app.wave_steps] *
                     amplitude / 32767;
    solar_os_gfx_line(gfx, previous_x, previous_y, x, y);
    previous_x = x;
    previous_y = y;
  }
  if (synth_app.wave_steps <= 64U) {
    for (size_t i = 0; i < synth_app.wave_steps; i++) {
      const int x =
          left + (int)(i * (size_t)(right - left) / synth_app.wave_steps);
      const int y =
          middle - (int32_t)synth_app.wavetable[i] * amplitude / 32767;
      solar_os_gfx_fill_rect(gfx, x - 1, y - 1, 3, 3);
    }
  }

  const int cursor_x =
      left + (int)(synth_app.wave_cursor * (size_t)(right - left) /
                   synth_app.wave_steps);
  const int cursor_y =
      middle -
      (int32_t)synth_app.wavetable[synth_app.wave_cursor] * amplitude / 32767;
  solar_os_gfx_line(gfx, cursor_x, top, cursor_x, bottom);
  solar_os_gfx_fill_rect(gfx, cursor_x - 2, cursor_y - 2, 5, 5);

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  char editor_status[64];
  snprintf(editor_status, sizeof(editor_status),
           "L/R move U/D draw  %03u/%u %+6d b%u",
           (unsigned)synth_app.wave_cursor, (unsigned)synth_app.wave_steps,
           (int)synth_app.wavetable[synth_app.wave_cursor],
           (unsigned)synth_app.wave_brush);
  solar_os_gfx_text(gfx, 6, editor_bottom - 8, editor_status);

  synth_draw_keyboard(gfx, width, height);
  solar_os_gfx_text(gfx, 6, height - 6,
                    width >= 380
                        ? "X keys  Enter steps B base R reset M smooth N norm"
                        : "X keys  Enter steps B base R reset M/N norm");
}

static void synth_draw_filter_response(solar_os_gfx_t *gfx, int x, int y,
                                       int width, int height) {
  const int left = x + 8;
  const int right = x + width - 8;
  const int top = y + 23;
  const int bottom = y + height - 8;
  const float cutoff = (float)synth_app.config.filter.cutoff_hz;
  const float resonance =
      (float)synth_app.config.filter.resonance_percent / 100.0f;
  const float quality = 0.5f + 9.5f * resonance * resonance;
  const float minimum_log =
      log10f((float)SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ);
  const float maximum_log =
      log10f((float)SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MAX_HZ);

  solar_os_gfx_rect(gfx, x, y, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, x + 8, y + 16, "LOW-PASS");
  solar_os_gfx_line(gfx, left, bottom, right, bottom);

  int previous_x = left;
  int previous_y = top;
  for (int column = 0; column <= right - left; column++) {
    const float position = (float)column / (float)(right - left);
    const float frequency =
        powf(10.0f, minimum_log + position * (maximum_log - minimum_log));
    const float ratio = frequency / cutoff;
    const float squared = ratio * ratio;
    const float denominator = sqrtf((1.0f - squared) * (1.0f - squared) +
                                    squared / (quality * quality));
    const float magnitude = denominator > 0.0001f ? 1.0f / denominator : 10.0f;
    float db = 20.0f * log10f(magnitude);
    if (db > 12.0f) {
      db = 12.0f;
    } else if (db < -48.0f) {
      db = -48.0f;
    }
    const int point_x = left + column;
    const int point_y =
        top + (int)((12.0f - db) * (float)(bottom - top) / 60.0f);
    if (column > 0) {
      solar_os_gfx_line(gfx, previous_x, previous_y, point_x, point_y);
    }
    previous_x = point_x;
    previous_y = point_y;
  }
}

static void synth_format_frequency(char *value, size_t value_size,
                                   uint32_t frequency_hz) {
  if (frequency_hz >= 1000U && frequency_hz % 1000U == 0U) {
    snprintf(value, value_size, "%uk", (unsigned)(frequency_hz / 1000U));
  } else if (frequency_hz >= 1000U) {
    snprintf(value, value_size, "%.1fk", (double)frequency_hz / 1000.0);
  } else {
    snprintf(value, value_size, "%u", (unsigned)frequency_hz);
  }
}

static void synth_draw_filter_editor(solar_os_gfx_t *gfx, int width,
                                     int height) {
  const int graphs_top = 35;
  const bool compact = height < 280;
  const int graphs_height =
      (compact ? 56 : 72) + synth_visualizer_extra_height();
  const int gap = 6;
  const int graph_width = (width - 18) / 2;
  synth_draw_filter_response(gfx, 6, graphs_top, graph_width, graphs_height);
  synth_draw_envelope(
      gfx, 12 + graph_width, graphs_top, width - graph_width - 18,
      graphs_height, "FILTER ENV", synth_app.config.filter.attack_ms,
      synth_app.config.filter.decay_ms, synth_app.config.filter.sustain_percent,
      synth_app.config.filter.release_ms);

  const int editor_bottom = synth_editor_bottom(height);
  const int controls_top = graphs_top + graphs_height + gap;
  const int knob_cell = (width - 12) / SYNTH_FILTER_CONTROL_COUNT;
  int knob_radius = knob_cell / 3;
  const int maximum_radius = synth_app.keyboard_visible
                                 ? (compact ? 10 : 16)
                                 : knob_cell / 3;
  if (knob_radius > maximum_radius) {
    knob_radius = maximum_radius;
  } else if (knob_radius < 10) {
    knob_radius = 10;
  }
  const int knob_y = synth_app.keyboard_visible
                         ? controls_top + knob_radius
                         : controls_top + (editor_bottom - controls_top - 29) / 2;
  const char *const labels[] = {"CUT", "RES", "ENV", "A", "D", "S", "R"};
  const uint32_t values[] = {
      synth_app.config.filter.cutoff_hz,
      synth_app.config.filter.resonance_percent,
      synth_app.config.filter.envelope_amount_percent,
      synth_app.config.filter.attack_ms,
      synth_app.config.filter.decay_ms,
      synth_app.config.filter.sustain_percent,
      synth_app.config.filter.release_ms,
  };
  for (size_t i = 0; i < SYNTH_FILTER_CONTROL_COUNT; i++) {
    char value[12];
    unsigned position;
    if (i == SYNTH_FILTER_CONTROL_CUTOFF) {
      synth_format_frequency(value, sizeof(value), values[i]);
      const size_t index = synth_filter_cutoff_value_index(values[i]);
      position = (unsigned)(index * 10U /
                            ((sizeof(synth_filter_cutoff_values) /
                              sizeof(synth_filter_cutoff_values[0])) -
                             1U));
    } else if (i == SYNTH_FILTER_CONTROL_RESONANCE ||
               i == SYNTH_FILTER_CONTROL_AMOUNT ||
               i == SYNTH_FILTER_CONTROL_SUSTAIN) {
      snprintf(value, sizeof(value), "%u%%", (unsigned)values[i]);
      position = (unsigned)values[i] / 10U;
    } else {
      snprintf(value, sizeof(value), "%ums", (unsigned)values[i]);
      const size_t index = synth_envelope_value_index(values[i]);
      position = (unsigned)(index * 10U /
                            ((sizeof(synth_envelope_values) /
                              sizeof(synth_envelope_values[0])) -
                             1U));
    }
    synth_draw_knob(gfx, 6 + (int)i * knob_cell + knob_cell / 2, knob_y,
                    knob_radius, labels[i], value, position,
                    synth_app.filter_selected == (synth_filter_control_t)i);
  }

  synth_draw_keyboard(gfx, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    "X keys  Arrows select/tune PgUp/Dn octave +/- velocity");
}

static void synth_draw_oscillator_panel(solar_os_gfx_t *gfx, int x, int y,
                                        int width, int height,
                                        const char *title,
                                        solar_os_synth_waveform_t waveform,
                                        bool selected) {
  solar_os_gfx_rect(gfx, x, y, width, height);
  if (selected) {
    solar_os_gfx_rect(gfx, x + 3, y + 3, width - 6, height - 6);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, x + 8, y + 16, title);
  synth_draw_wave_icon(gfx, x + 10, y + 24, width - 20, height - 43, waveform,
                       NULL);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, x + 8, y + height - 6,
                    synth_wave_short_name(waveform));
}

static void synth_draw_oscillator2_editor(solar_os_gfx_t *gfx, int width,
                                          int height) {
  const bool compact = height < 280;
  const int graphs_top = 35;
  const int graphs_height =
      (compact ? 56 : 70) + synth_visualizer_extra_height();
  const int graph_gap = 6;
  const int graph_width = (width - 18) / 2;
  synth_draw_oscillator_panel(gfx, 6, graphs_top, graph_width, graphs_height,
                              "OSC1", synth_app.config.waveform, false);
  synth_draw_oscillator_panel(
      gfx, 12 + graph_width, graphs_top, width - graph_width - 18,
      graphs_height, "OSC2", synth_app.config.oscillator2.waveform,
      synth_app.oscillator2_selected == SYNTH_OSCILLATOR2_CONTROL_WAVE);

  const int editor_bottom = synth_editor_bottom(height);
  const int controls_top = graphs_top + graphs_height + graph_gap;
  const int knob_cell = (width - 12) / SYNTH_OSCILLATOR2_CONTROL_COUNT;
  int knob_radius = knob_cell / 4;
  const int maximum_radius = synth_app.keyboard_visible
                                 ? (compact ? 10 : 20)
                                 : knob_cell / 3;
  const int minimum_radius = compact ? 10 : 11;
  if (knob_radius > maximum_radius) {
    knob_radius = maximum_radius;
  } else if (knob_radius < minimum_radius) {
    knob_radius = minimum_radius;
  }
  const int knob_y = synth_app.keyboard_visible
                         ? controls_top + knob_radius
                         : controls_top + (editor_bottom - controls_top - 29) / 2;
  const char *const labels[] = {"WAVE", "OCT", "FINE", "MIX"};
  char values[SYNTH_OSCILLATOR2_CONTROL_COUNT][12];
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_WAVE],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_WAVE]), "%s",
           synth_wave_short_name(synth_app.config.oscillator2.waveform));
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_OCTAVE],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_OCTAVE]), "%+d",
           (int)synth_app.config.oscillator2.octave);
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_DETUNE],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_DETUNE]), "%+dc",
           (int)synth_app.config.oscillator2.detune_cents);
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_MIX],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_MIX]), "%u%%",
           (unsigned)synth_app.config.oscillator2.mix_percent);
  const unsigned positions[] = {
      (unsigned)synth_app.config.oscillator2.waveform * 10U /
          SOLAR_OS_SYNTH_WAVE_CUSTOM,
      (unsigned)(synth_app.config.oscillator2.octave -
                 SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN) *
          10U /
          (SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX -
           SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN),
      (unsigned)(synth_app.config.oscillator2.detune_cents -
                 SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS) *
          10U /
          (SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS -
           SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS),
      synth_app.config.oscillator2.mix_percent / 10U,
  };
  for (size_t i = 0; i < SYNTH_OSCILLATOR2_CONTROL_COUNT; i++) {
    synth_draw_knob(gfx, 6 + (int)i * knob_cell + knob_cell / 2, knob_y,
                    knob_radius, labels[i], values[i], positions[i],
                    synth_app.oscillator2_selected ==
                        (synth_oscillator2_control_t)i);
  }

  synth_draw_keyboard(gfx, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    "X keys  Arrows select/tune PgUp/Dn octave +/- velocity");
}

static void synth_draw_preset_editor(solar_os_gfx_t *gfx, int width,
                                     int height) {
  const bool compact = height < 280;
  const int editor_bottom = synth_editor_bottom(height);
  const int gap = 6;
  const int cell_width = (width - 18) / 2;
  const int factory_x = 6;
  const int user_x = factory_x + cell_width + gap;
  const int rows_top = compact ? 50 : 54;
  int row_height = compact ? 12 : 17;
  if (!synth_app.keyboard_visible) {
    const int expanded_row_height =
        (editor_bottom - rows_top - 22) / SYNTH_APP_FACTORY_PRESET_COUNT;
    if (expanded_row_height > row_height) {
      row_height = expanded_row_height;
    }
  }

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, factory_x + 4, rows_top - 5, "FACTORY");
  solar_os_gfx_text(gfx, user_x + 4, rows_top - 5, "USER");
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  for (size_t row = 0; row < SYNTH_APP_FACTORY_PRESET_COUNT; row++) {
    const int y = rows_top + (int)row * row_height;
    char factory_label[24];
    snprintf(factory_label, sizeof(factory_label), "F%u  %s",
             (unsigned)(row + 1U), synth_factory_preset_names[row]);
    if (synth_app.preset_selected == row) {
      solar_os_gfx_rect(gfx, factory_x, y, cell_width, row_height - 1);
    }
    solar_os_gfx_text(gfx, factory_x + 4, y + row_height - 3, factory_label);

    const size_t user_index = SYNTH_APP_FACTORY_PRESET_COUNT + row;
    const char *state =
        synth_app.preset_slots[row] == SYNTH_PRESET_SLOT_SAVED     ? "SAVED"
        : synth_app.preset_slots[row] == SYNTH_PRESET_SLOT_INVALID ? "INVALID"
                                                                   : "EMPTY";
    char user_label[24];
    snprintf(user_label, sizeof(user_label), "U%u  %s", (unsigned)(row + 1U),
             state);
    if (synth_app.preset_selected == user_index) {
      solar_os_gfx_rect(gfx, user_x, y, cell_width, row_height - 1);
    }
    solar_os_gfx_text(gfx, user_x + 4, y + row_height - 3, user_label);
  }

  if (!compact || !synth_app.keyboard_visible) {
    solar_os_gfx_text(gfx, 6, editor_bottom - 8, synth_app.preset_message);
  }
  synth_draw_keyboard(gfx, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    compact && synth_app.keyboard_visible &&
                            synth_app.preset_message[0] != '\0'
                        ? synth_app.preset_message
                        : "X keys  Arrows select  Enter load  V save");
}

static void synth_draw_mode_editor(solar_os_gfx_t *gfx, int width, int height) {
  const int editor_bottom = synth_editor_bottom(height);
  const int panel_x = 6;
  const int panel_y = 42;
  const int panel_width = width - 12;
  const int panel_height = editor_bottom - panel_y - 14;
  const int center_y = panel_y + panel_height / 2;
  const int control_width = panel_width / 4;

  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  solar_os_gfx_rect(gfx, panel_x, panel_y, panel_width, panel_height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, panel_x + 10, panel_y + 18, "PERFORMANCE MODE");

  if (synth_app.mode_selected == SYNTH_MODE_CONTROL_VOICES) {
    solar_os_gfx_rect(gfx, panel_x + 10, center_y - 25,
                      control_width - 20, 48);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  const char *voices_label = "VOICES";
  solar_os_gfx_text(
      gfx,
      panel_x + control_width / 2 -
          (int)solar_os_gfx_text_width(gfx, voices_label) / 2,
      center_y - 5, voices_label);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
  const char *voices = synth_app.performance.mono ? "MONO" : "POLY";
  solar_os_gfx_text(
      gfx,
      panel_x + control_width / 2 -
          (int)solar_os_gfx_text_width(gfx, voices) / 2,
      center_y + 14, voices);

  const int hold_x = panel_x + control_width;
  if (synth_app.mode_selected == SYNTH_MODE_CONTROL_HOLD) {
    solar_os_gfx_rect(gfx, hold_x + 5, center_y - 25,
                      control_width - 10, 48);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  const char *hold_label = "HOLD";
  solar_os_gfx_text(
      gfx,
      hold_x + control_width / 2 -
          (int)solar_os_gfx_text_width(gfx, hold_label) / 2,
      center_y - 5, hold_label);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
  const char *hold = synth_app.hold_mode ? "ON" : "OFF";
  solar_os_gfx_text(
      gfx,
      hold_x + control_width / 2 -
          (int)solar_os_gfx_text_width(gfx, hold) / 2,
      center_y + 14, hold);

  const int graph_x = panel_x + control_width * 2 + 10;
  const int graph_width = panel_width - control_width * 2 - 22;
  const int graph_top = panel_y + 32;
  const int graph_bottom = panel_y + panel_height - 20;
  if (synth_app.mode_selected == SYNTH_MODE_CONTROL_GLIDE) {
    solar_os_gfx_rect(gfx, graph_x - 5, graph_top - 9, graph_width + 10,
                      graph_bottom - graph_top + 24);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  char glide_label[24];
  snprintf(glide_label, sizeof(glide_label), "GLIDE %ums",
           (unsigned)synth_app.performance.glide_ms);
  solar_os_gfx_text(gfx, graph_x, panel_y + 18, glide_label);
  solar_os_gfx_line(gfx, graph_x, graph_bottom, graph_x + graph_width,
                    graph_bottom);
  solar_os_gfx_line(gfx, graph_x, graph_top, graph_x + graph_width, graph_top);
  if (synth_app.performance.glide_ms == 0U) {
    solar_os_gfx_line(gfx, graph_x, graph_bottom, graph_x + 2, graph_top);
    solar_os_gfx_line(gfx, graph_x + 2, graph_top, graph_x + graph_width,
                      graph_top);
  } else {
    const int maximum_ramp = graph_width * 3 / 4;
    int ramp = (int)(((uint32_t)maximum_ramp * synth_app.performance.glide_ms) /
                     SOLAR_OS_SYNTH_VOICE_GLIDE_MAX_MS);
    if (ramp < 8) {
      ramp = 8;
    }
    solar_os_gfx_line(gfx, graph_x, graph_bottom, graph_x + ramp, graph_top);
    solar_os_gfx_line(gfx, graph_x + ramp, graph_top, graph_x + graph_width,
                      graph_top);
  }

  synth_draw_keyboard(gfx, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    "X keys  Left/Right select  Up/Down set  Enter toggle");
}

static bool synth_use_compact_layout(int width, int height) {
  return width < 240 || height < 200;
}

static bool synth_use_micro_layout(int width, int height) {
  return width < 112 || height < 56;
}

static int synth_centered_text_x(solar_os_gfx_t *gfx, int width,
                                 const char *text) {
  const int text_width = (int)solar_os_gfx_text_width(gfx, text);
  return text_width < width ? (width - text_width) / 2 : 0;
}

static void
synth_draw_compact_header(solar_os_gfx_t *gfx, int width, const char *group,
                          unsigned selected, unsigned count,
                          const solar_os_synth_voice_status_t *status) {
  char heading[20];
  snprintf(heading, sizeof(heading), "%s %u/%u", group, selected, count);
  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 2, 10, heading);
  if (width >= 112) {
    char voices[8];
    snprintf(voices, sizeof(voices), "%uv", (unsigned)status->active_voices);
    solar_os_gfx_text(
        gfx, width - 2 - (int)solar_os_gfx_text_width(gfx, voices), 10, voices);
  }
}

static unsigned synth_compact_parameter_position(synth_parameter_t parameter,
                                                 float value) {
  const solar_os_parameter_definition_t *definition =
      &synth_parameters[(size_t)parameter];
  float position;
  if (definition->curve == SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC &&
      definition->minimum > 0.0f && value > 0.0f) {
    position = logf(value / definition->minimum) /
               logf(definition->maximum / definition->minimum);
  } else {
    position = (value - definition->minimum) /
               (definition->maximum - definition->minimum);
  }
  if (position < 0.0f) {
    position = 0.0f;
  } else if (position > 1.0f) {
    position = 1.0f;
  }
  return (unsigned)lroundf(position * 1000.0f);
}

static void synth_format_compact_parameter(synth_parameter_t parameter,
                                           float raw_value, char *value,
                                           size_t value_size) {
  const solar_os_parameter_definition_t *definition =
      &synth_parameters[(size_t)parameter];
  const long rounded = lroundf(raw_value);
  if (strcmp(definition->unit, "%") == 0) {
    snprintf(value, value_size, "%ld%%", rounded);
  } else if (strcmp(definition->unit, "Hz") == 0) {
    char frequency[12];
    synth_format_frequency(frequency, sizeof(frequency), (uint32_t)rounded);
    snprintf(value, value_size, "%sHz", frequency);
  } else if (strcmp(definition->unit, "ms") == 0 && rounded >= 1000) {
    snprintf(value, value_size, "%.1fs", (double)raw_value / 1000.0);
  } else if (strcmp(definition->unit, "oct") == 0) {
    snprintf(value, value_size, "%+ld oct", rounded);
  } else if (strcmp(definition->unit, "cent") == 0) {
    snprintf(value, value_size, "%+ld cent", rounded);
  } else {
    snprintf(value, value_size, "%ld%s", rounded, definition->unit);
  }
}

static void synth_compact_parameter_location(synth_parameter_t parameter,
                                             const char **group,
                                             unsigned *selected,
                                             unsigned *count) {
  if (parameter <= SYNTH_PARAMETER_RELEASE) {
    *group = "PLAY";
    *selected = (unsigned)parameter + 2U;
    *count = SYNTH_CONTROL_COUNT;
  } else if (parameter <= SYNTH_PARAMETER_FILTER_RELEASE) {
    *group = "FILT";
    *selected = (unsigned)(parameter - SYNTH_PARAMETER_FILTER_CUTOFF) + 1U;
    *count = SYNTH_FILTER_CONTROL_COUNT;
  } else if (parameter <= SYNTH_PARAMETER_OSC2_MIX) {
    *group = "OSC2";
    *selected = (unsigned)(parameter - SYNTH_PARAMETER_OSC2_OCTAVE) + 2U;
    *count = SYNTH_OSCILLATOR2_CONTROL_COUNT;
  } else {
    *group = "MODE";
    *selected = 3U;
    *count = SYNTH_MODE_CONTROL_COUNT;
  }
}

static void synth_draw_compact_meter(
    solar_os_gfx_t *gfx, int width, int height, const char *group,
    unsigned selected, unsigned count, const char *label, const char *value,
    unsigned position, const solar_os_synth_voice_status_t *status) {
  const bool micro = synth_use_micro_layout(width, height);
  synth_draw_compact_header(gfx, width, group, selected, count, status);

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, synth_centered_text_x(gfx, width, label),
                    micro ? 21 : 24, label);
  solar_os_gfx_set_font(gfx, micro ? SOLAR_OS_GFX_FONT_BOLD_14
                                   : SOLAR_OS_GFX_FONT_BOLD_16);
  solar_os_gfx_text(gfx, synth_centered_text_x(gfx, width, value),
                    micro ? 36 : 43, value);

  const int bar_x = 4;
  const int bar_y = height - (micro ? 7 : 17);
  const int bar_width = width - 8;
  solar_os_gfx_rect(gfx, bar_x, bar_y, bar_width, 5);
  const int fill_width = (int)((uint32_t)(bar_width - 2) * position / 1000U);
  if (fill_width > 0) {
    solar_os_gfx_fill_rect(gfx, bar_x + 1, bar_y + 1, fill_width, 3);
  }

  if (!micro) {
    char footer[24];
    snprintf(footer, sizeof(footer), "O%d  V%u", synth_app.octave,
             (unsigned)synth_app.velocity);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx, synth_centered_text_x(gfx, width, footer),
                      height - 4, footer);
  }
}

static void
synth_draw_compact_parameter(solar_os_gfx_t *gfx, int width, int height,
                             synth_parameter_t parameter,
                             const solar_os_synth_voice_status_t *status) {
  static const char *const labels[SYNTH_PARAMETER_COUNT] = {
      "VOLUME",   "ATTACK",    "DECAY",     "SUSTAIN",
      "RELEASE",  "CUTOFF",    "RESONANCE", "FILTER ENV",
      "F ATTACK", "F DECAY",   "F SUSTAIN", "F RELEASE",
      "OSC2 OCT", "OSC2 FINE", "OSC2 MIX",  "GLIDE",
  };
  float raw_value = 0.0f;
  (void)synth_parameter_get((void *)(uintptr_t)parameter, &raw_value);
  char value[24];
  synth_format_compact_parameter(parameter, raw_value, value, sizeof(value));
  const char *group;
  unsigned selected;
  unsigned count;
  synth_compact_parameter_location(parameter, &group, &selected, &count);
  synth_draw_compact_meter(
      gfx, width, height, group, selected, count, labels[(size_t)parameter],
      value, synth_compact_parameter_position(parameter, raw_value), status);
}

static void synth_draw_compact_waveform(
    solar_os_gfx_t *gfx, int width, int height, const char *group,
    unsigned selected, unsigned count, const char *label,
    solar_os_synth_waveform_t waveform,
    const solar_os_synth_voice_status_t *status, bool live) {
  const bool micro = synth_use_micro_layout(width, height);
  synth_draw_compact_header(gfx, width, group, selected, count, status);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, 4, 23, label);
  const int graph_y = micro ? 25 : 27;
  const int graph_height = height - graph_y - (micro ? 3 : 15);
  synth_draw_wave_icon(gfx, 4, graph_y, width - 8, graph_height, waveform,
                       live ? status : NULL);
  if (!micro) {
    const char *name = synth_wave_short_name(waveform);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx, synth_centered_text_x(gfx, width, name), height - 4,
                      name);
  }
}

static void
synth_draw_compact_wave_editor(solar_os_gfx_t *gfx, int width, int height,
                               const solar_os_synth_voice_status_t *status) {
  const bool micro = synth_use_micro_layout(width, height);
  synth_draw_compact_header(gfx, width, "WAVE", 1U, 1U, status);
  const int left = 3;
  const int right = width - 4;
  const int top = 14;
  const int bottom = height - (micro ? 3 : 15);
  const int middle = (top + bottom) / 2;
  const int amplitude = (bottom - top) / 2;
  solar_os_gfx_line(gfx, left, middle, right, middle);
  int previous_x = left;
  int previous_y = middle - (int32_t)synth_app.wavetable[0] * amplitude / 32767;
  for (size_t i = 1; i <= synth_app.wave_steps; i++) {
    const int x =
        left + (int)(i * (size_t)(right - left) / synth_app.wave_steps);
    const int y =
        middle - (int32_t)synth_app.wavetable[i % synth_app.wave_steps] *
                     amplitude / 32767;
    solar_os_gfx_line(gfx, previous_x, previous_y, x, y);
    previous_x = x;
    previous_y = y;
  }
  const int cursor_x =
      left + (int)(synth_app.wave_cursor * (size_t)(right - left) /
                   synth_app.wave_steps);
  solar_os_gfx_line(gfx, cursor_x, top, cursor_x, bottom);
  if (!micro) {
    char footer[24];
    snprintf(footer, sizeof(footer), "%02u/%u %+d b%u",
             (unsigned)synth_app.wave_cursor, (unsigned)synth_app.wave_steps,
             (int)synth_app.wavetable[synth_app.wave_cursor],
             (unsigned)synth_app.wave_brush);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_gfx_text(gfx, synth_centered_text_x(gfx, width, footer),
                      height - 4, footer);
  }
}

static void
synth_draw_compact_preset(solar_os_gfx_t *gfx, int width, int height,
                          const solar_os_synth_voice_status_t *status) {
  const bool factory =
      synth_app.preset_selected < SYNTH_APP_FACTORY_PRESET_COUNT;
  const size_t row = synth_app.preset_selected % SYNTH_APP_FACTORY_PRESET_COUNT;
  const char *value;
  if (factory) {
    value = synth_factory_preset_names[row];
  } else {
    value = synth_app.preset_slots[row] == SYNTH_PRESET_SLOT_SAVED ? "SAVED"
            : synth_app.preset_slots[row] == SYNTH_PRESET_SLOT_INVALID
                ? "INVALID"
                : "EMPTY";
  }
  char label[16];
  snprintf(label, sizeof(label), "%s %u", factory ? "FACTORY" : "USER",
           (unsigned)(row + 1U));
  synth_draw_compact_meter(
      gfx, width, height, "PRE", (unsigned)(row + 1U),
      SYNTH_APP_FACTORY_PRESET_COUNT, label, value,
      (unsigned)(row * 1000U / (SYNTH_APP_FACTORY_PRESET_COUNT - 1U)), status);
}

static void synth_draw_compact(solar_os_gfx_t *gfx, int width, int height,
                               const solar_os_synth_voice_status_t *status) {
  const esp_err_t display_error = synth_app.last_error != ESP_OK
                                      ? synth_app.last_error
                                      : status->last_error;
  if (display_error != ESP_OK) {
    synth_draw_compact_meter(gfx, width, height, "SYNTH", 1U, 1U, "AUDIO",
                             esp_err_to_name(display_error), 0U, status);
    return;
  }
  if (synth_app.compact_parameter_valid) {
    synth_draw_compact_parameter(gfx, width, height,
                                 synth_app.compact_parameter, status);
    return;
  }
  if (synth_app.tab == SYNTH_TAB_WAVE) {
    synth_draw_compact_wave_editor(gfx, width, height, status);
  } else if (synth_app.tab == SYNTH_TAB_PRESET) {
    synth_draw_compact_preset(gfx, width, height, status);
  } else if (synth_app.tab == SYNTH_TAB_MODE &&
             synth_app.mode_selected == SYNTH_MODE_CONTROL_VOICES) {
    synth_draw_compact_meter(gfx, width, height, "MODE", 1U,
                             SYNTH_MODE_CONTROL_COUNT, "VOICES",
                             synth_app.performance.mono ? "MONO" : "POLY",
                             synth_app.performance.mono ? 0U : 1000U, status);
  } else if (synth_app.tab == SYNTH_TAB_MODE &&
             synth_app.mode_selected == SYNTH_MODE_CONTROL_HOLD) {
    synth_draw_compact_meter(gfx, width, height, "MODE", 2U,
                             SYNTH_MODE_CONTROL_COUNT, "HOLD",
                             synth_app.hold_mode ? "ON" : "OFF",
                             synth_app.hold_mode ? 1000U : 0U, status);
  } else if (synth_app.tab == SYNTH_TAB_MODE) {
    synth_draw_compact_parameter(gfx, width, height, SYNTH_PARAMETER_GLIDE,
                                 status);
  } else if (synth_app.tab == SYNTH_TAB_FILTER) {
    synth_draw_compact_parameter(
        gfx, width, height,
        (synth_parameter_t)(SYNTH_PARAMETER_FILTER_CUTOFF +
                            synth_app.filter_selected),
        status);
  } else if (synth_app.tab == SYNTH_TAB_OSCILLATOR2 &&
             synth_app.oscillator2_selected == SYNTH_OSCILLATOR2_CONTROL_WAVE) {
    synth_draw_compact_waveform(
        gfx, width, height, "OSC2", 1U, SYNTH_OSCILLATOR2_CONTROL_COUNT,
        "OSC2 WAVE", synth_app.config.oscillator2.waveform, status, false);
  } else if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
    synth_draw_compact_parameter(
        gfx, width, height,
        (synth_parameter_t)(SYNTH_PARAMETER_OSC2_OCTAVE +
                            synth_app.oscillator2_selected - 1),
        status);
  } else if (synth_app.selected == SYNTH_CONTROL_WAVE) {
    synth_draw_compact_waveform(gfx, width, height, "PLAY", 1U,
                                SYNTH_CONTROL_COUNT, "WAVE",
                                synth_app.config.waveform, status, true);
  } else {
    synth_draw_compact_parameter(
        gfx, width, height,
        (synth_parameter_t)(synth_app.selected - SYNTH_CONTROL_VOLUME), status);
  }
}

static bool synth_scope_visible(solar_os_context_t *ctx) {
  solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
  if (synth_app.headless || gfx == NULL || synth_app.suspended ||
      synth_app.tab != SYNTH_TAB_PLAY) {
    return false;
  }
  const int width = (int)solar_os_gfx_width(gfx);
  const int height = (int)solar_os_gfx_height(gfx);
  return !synth_use_compact_layout(width, height) ||
         (!synth_app.compact_parameter_valid &&
          synth_app.selected == SYNTH_CONTROL_WAVE);
}

static void synth_remember_rendered_status(
    const solar_os_synth_voice_status_t *status) {
  synth_app.last_active_voices = status->active_voices;
  synth_app.last_deadline_misses = status->render_deadline_misses;
  synth_app.last_pcm_generation = status->pcm_generation;
  synth_app.last_running = status->running;
  synth_app.last_status_poll_ms = synth_now_ms();
}

static void synth_render(solar_os_context_t *ctx) {
  solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
  if (synth_app.headless || gfx == NULL || synth_app.suspended) {
    return;
  }

  const int width = (int)solar_os_gfx_width(gfx);
  const int height = (int)solar_os_gfx_height(gfx);
  synth_app.visual_dirty = false;
  solar_os_synth_voice_status_t status;
  solar_os_synth_voice_get_status(&status);
  solar_os_audio_status_t audio_status;
  solar_os_audio_get_status(&audio_status);
  synth_app.volume = audio_status.volume;

  solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
  if (synth_use_compact_layout(width, height)) {
    synth_draw_compact(gfx, width, height, &status);
    solar_os_gfx_present(gfx);
    synth_remember_rendered_status(&status);
    return;
  }
  synth_draw_header(gfx, &status);

  if (synth_app.tab == SYNTH_TAB_WAVE) {
    synth_draw_wave_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_remember_rendered_status(&status);
    return;
  }

  if (synth_app.tab == SYNTH_TAB_FILTER) {
    synth_draw_filter_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_remember_rendered_status(&status);
    return;
  }

  if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
    synth_draw_oscillator2_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_remember_rendered_status(&status);
    return;
  }

  if (synth_app.tab == SYNTH_TAB_PRESET) {
    synth_draw_preset_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_remember_rendered_status(&status);
    return;
  }

  if (synth_app.tab == SYNTH_TAB_MODE) {
    synth_draw_mode_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_remember_rendered_status(&status);
    return;
  }

  const int graphs_top = 35;
  const int graphs_height = 70 + synth_visualizer_extra_height();
  const int editor_bottom = synth_editor_bottom(height);
  const int wave_width = width / 4;
  const int wave_x = 6;
  const int wave_panel_width = wave_width - 10;
  solar_os_gfx_rect(gfx, wave_x, graphs_top, wave_panel_width, graphs_height);
  if (synth_app.selected == SYNTH_CONTROL_WAVE) {
    solar_os_gfx_rect(gfx, wave_x + 3, graphs_top + 3, wave_panel_width - 6,
                      graphs_height - 6);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, wave_x + 8, graphs_top + 16, "WAVE");
  synth_draw_wave_icon(gfx, wave_x + 10, graphs_top + 25, wave_panel_width - 20,
                       graphs_height - 43, synth_app.config.waveform, &status);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  char wave_status[24];
  const char *wave_name = synth_wave_short_name(synth_app.config.waveform);
  if (status.pcm_generation > 0U &&
      status.pcm_waveform == synth_app.config.waveform) {
    snprintf(wave_status, sizeof(wave_status), "%s %04lx", wave_name,
             (unsigned long)(status.pcm_hash & 0xffffU));
  } else {
    snprintf(wave_status, sizeof(wave_status), "%s", wave_name);
  }
  solar_os_gfx_text(gfx, wave_x + 8, graphs_top + graphs_height - 7,
                    wave_status);

  const int knob_area_x = wave_width;
  synth_draw_envelope(
      gfx, knob_area_x, graphs_top, width - knob_area_x - 6, graphs_height,
      "ENVELOPE", synth_app.config.attack_ms, synth_app.config.decay_ms,
      synth_app.config.sustain_percent, synth_app.config.release_ms);

  const int controls_top = graphs_top + graphs_height + 8;
  const int control_bottom = editor_bottom - 13;
  const int volume_y = controls_top + 7;
  const int volume_height = synth_app.keyboard_visible
                                ? 48
                                : editor_bottom - volume_y - 7;
  synth_draw_volume_button(gfx, wave_x, volume_y, wave_panel_width, volume_height,
                           synth_app.selected == SYNTH_CONTROL_VOLUME);

  const int knob_cell = (width - knob_area_x) / 4;
  int knob_radius = knob_cell / 3;
  const int maximum_radius = synth_app.keyboard_visible ? 22 : 30;
  if (knob_radius > maximum_radius) {
    knob_radius = maximum_radius;
  } else if (knob_radius < 13) {
    knob_radius = 13;
  }
  const int knob_y = synth_app.keyboard_visible
                         ? controls_top + knob_radius + 1
                         : controls_top + (editor_bottom - controls_top - 29) / 2;
  const uint32_t knob_values[] = {
      synth_app.config.attack_ms,
      synth_app.config.decay_ms,
      synth_app.config.sustain_percent,
      synth_app.config.release_ms,
  };
  const char *const knob_labels[] = {"A", "D", "S", "R"};
  for (size_t i = 0; i < 4; i++) {
    char value[16];
    unsigned position;
    if (i == 2) {
      snprintf(value, sizeof(value), "%u%%", (unsigned)knob_values[i]);
      position = (unsigned)knob_values[i] / 10U;
    } else {
      snprintf(value, sizeof(value), "%ums", (unsigned)knob_values[i]);
      const size_t index = synth_envelope_value_index(knob_values[i]);
      position =
          (unsigned)((index * 10U) / ((sizeof(synth_envelope_values) /
                                       sizeof(synth_envelope_values[0])) -
                                      1U));
    }
    synth_draw_knob(gfx, knob_area_x + (int)i * knob_cell + knob_cell / 2,
                    knob_y, knob_radius, knob_labels[i], value, position,
                    synth_app.selected ==
                        (synth_control_t)(i + SYNTH_CONTROL_ATTACK));
  }

  if (editor_bottom > control_bottom) {
    synth_draw_keyboard(gfx, width, height);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    "X keys  Arrows select/tune PgUp/Dn octave +/- velocity");
  solar_os_gfx_present(gfx);

  synth_remember_rendered_status(&status);
}

static void synth_render_changed(solar_os_context_t *ctx, bool changed,
                                 bool performance_activity,
                                 uint32_t now_ms) {
  if (performance_activity) {
    synth_app.last_performance_ms = now_ms;
  }
  if (!changed && !synth_app.visual_dirty) {
    return;
  }

  solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
  if (synth_app.headless || gfx == NULL || synth_app.suspended) {
    return;
  }
  const bool compact = synth_use_compact_layout(
      (int)solar_os_gfx_width(gfx), (int)solar_os_gfx_height(gfx));
  if (compact &&
      (uint32_t)(now_ms - synth_app.last_performance_ms) <
          SYNTH_APP_COMPACT_VISUAL_QUIET_MS) {
    synth_app.visual_dirty = true;
    return;
  }
  synth_render(ctx);
}

static synth_held_note_t *synth_find_held(const solar_os_input_key_event_t *key,
                                          int semitone) {
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    synth_held_note_t *held = &synth_app.held[i];
    if (!held->active) {
      continue;
    }
    if (held->midi) {
      continue;
    }
    if (key->physical_key != SOLAR_OS_INPUT_PHYSICAL_NONE) {
      if (held->source == key->source &&
          held->physical_key == key->physical_key) {
        return held;
      }
    } else if (held->physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE &&
               held->semitone == semitone) {
      return held;
    }
  }
  return NULL;
}

static synth_held_note_t *synth_allocate_held(void) {
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    if (!synth_app.held[i].active) {
      return &synth_app.held[i];
    }
  }
  return NULL;
}

static bool synth_note_on(const solar_os_input_key_event_t *key, int semitone) {
  synth_held_note_t *held = synth_find_held(key, semitone);
  if (held != NULL && key->physical_key != SOLAR_OS_INPUT_PHYSICAL_NONE) {
    return false;
  }
  if (held == NULL) {
    held = synth_allocate_held();
  }
  if (held == NULL) {
    return false;
  }

  const uint32_t frequency = synth_note_frequency((uint8_t)semitone);
  const esp_err_t err = solar_os_synth_voice_note_on(SYNTH_APP_OWNER, frequency,
                                                     synth_app.velocity);
  synth_app.last_error = err;
  if (err != ESP_OK) {
    return true;
  }

  *held = (synth_held_note_t){
      .active = true,
      .source = key->source,
      .physical_key = key->physical_key,
      .usage = key->usage,
      .frequency_hz = frequency,
      .release_at_ms = !synth_app.hold_mode &&
                               key->physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE
                           ? synth_now_ms() + SYNTH_APP_PULSE_MS
                           : 0U,
      .semitone = (uint8_t)semitone,
  };
  return true;
}

static bool synth_release_held(synth_held_note_t *held) {
  if (held == NULL || !held->active) {
    return false;
  }
  const uint32_t frequency = held->frequency_hz;
  memset(held, 0, sizeof(*held));
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    if (synth_app.held[i].active &&
        synth_app.held[i].frequency_hz == frequency) {
      return true;
    }
  }
  const esp_err_t err =
      solar_os_synth_voice_note_off(SYNTH_APP_OWNER, frequency);
  if (err != ESP_OK) {
    synth_app.last_error = err;
  }
  return true;
}

static bool synth_note_off(const solar_os_input_key_event_t *key,
                           int semitone) {
  return synth_release_held(synth_find_held(key, semitone));
}

static bool synth_toggle_note(const solar_os_input_key_event_t *key,
                              int semitone) {
  synth_held_note_t *held = synth_find_held(key, semitone);
  return held != NULL ? synth_release_held(held)
                      : synth_note_on(key, semitone);
}

static bool synth_set_hold_mode(bool enabled) {
  if (synth_app.hold_mode == enabled) {
    return false;
  }
  synth_app.hold_mode = enabled;
  bool changed = true;
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    synth_held_note_t *held = &synth_app.held[i];
    if (!held->active || held->midi) {
      continue;
    }
    if (enabled) {
      held->release_at_ms = 0U;
    } else {
      changed |= synth_release_held(held);
    }
  }
  return changed;
}

static synth_held_note_t *synth_find_midi_held(uint8_t channel, uint8_t note) {
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    synth_held_note_t *held = &synth_app.held[i];
    if (held->active && held->midi && held->midi_channel == channel &&
        held->midi_note == note) {
      return held;
    }
  }
  return NULL;
}

static uint32_t synth_midi_frequency(uint8_t note) {
  const float frequency = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
  if (frequency < (float)SOLAR_OS_SYNTH_VOICE_FREQUENCY_MIN_HZ ||
      frequency > (float)SOLAR_OS_SYNTH_VOICE_FREQUENCY_MAX_HZ) {
    return 0U;
  }
  return (uint32_t)(frequency + 0.5f);
}

static bool synth_midi_note_on(uint8_t channel, uint8_t note,
                               uint8_t velocity) {
  if (velocity == 0U) {
    synth_held_note_t *held = synth_find_midi_held(channel, note);
    if (held == NULL) {
      return false;
    }
    if (synth_app.midi_sustain[channel]) {
      held->release_pending = true;
      return true;
    }
    return synth_release_held(held);
  }
  synth_held_note_t *held = synth_find_midi_held(channel, note);
  if (held != NULL) {
    held->release_pending = false;
    const esp_err_t error = solar_os_synth_voice_note_on(
        SYNTH_APP_OWNER, held->frequency_hz, velocity);
    synth_app.last_error = error;
    return true;
  }
  held = synth_allocate_held();
  const uint32_t frequency = synth_midi_frequency(note);
  if (held == NULL || frequency == 0U) {
    return false;
  }
  const esp_err_t error = solar_os_synth_voice_note_on(SYNTH_APP_OWNER,
                                                       frequency,
                                                       velocity);
  synth_app.last_error = error;
  if (error != ESP_OK) {
    return true;
  }
  *held = (synth_held_note_t) {
      .active = true,
      .midi = true,
      .source = SOLAR_OS_INPUT_SOURCE_INVALID,
      .physical_key = SOLAR_OS_INPUT_PHYSICAL_NONE,
      .frequency_hz = frequency,
      .semitone = note % 12U,
      .midi_channel = channel,
      .midi_note = note,
  };
  return true;
}

static bool synth_midi_note_off(uint8_t channel, uint8_t note) {
  synth_held_note_t *held = synth_find_midi_held(channel, note);
  if (held == NULL) {
    return false;
  }
  if (synth_app.midi_sustain[channel]) {
    held->release_pending = true;
    return true;
  }
  return synth_release_held(held);
}

static bool synth_midi_release_channel(uint8_t channel, bool pending_only) {
  bool changed = false;
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    synth_held_note_t *held = &synth_app.held[i];
    if (held->active && held->midi && held->midi_channel == channel &&
        (!pending_only || held->release_pending)) {
      changed |= synth_release_held(held);
    }
  }
  return changed;
}

static bool synth_handle_midi_message(const solar_os_midi_message_t *message) {
  if (message == NULL || message->status < 0x80U || message->status >= 0xf0U) {
    return false;
  }
  const uint8_t kind = message->status & 0xf0U;
  const uint8_t channel = message->status & 0x0fU;
  if (kind == 0x90U) {
    return synth_midi_note_on(channel, message->data1, message->data2);
  }
  if (kind == 0x80U) {
    return synth_midi_note_off(channel, message->data1);
  }
  if (kind == 0xb0U && message->data1 == 64U) {
    const bool sustain = message->data2 >= 64U;
    if (synth_app.midi_sustain[channel] == sustain) {
      return false;
    }
    synth_app.midi_sustain[channel] = sustain;
    return sustain ? true : synth_midi_release_channel(channel, true);
  }
  if (kind == 0xb0U && (message->data1 == 120U || message->data1 == 123U)) {
    synth_app.midi_sustain[channel] = false;
    return synth_midi_release_channel(channel, false);
  }
  return false;
}

static void synth_midi_subscribe(void) {
  if (synth_app.midi_subscribed) {
    return;
  }
  synth_app.midi_subscription =
      (solar_os_midi_subscription_t)SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
  synth_app.midi_subscribed =
      solar_os_midi_subscribe(SYNTH_APP_OWNER, &synth_app.midi_subscription) ==
      ESP_OK;
}

static void synth_midi_unsubscribe(void) {
  if (synth_app.midi_subscribed) {
    (void)solar_os_midi_unsubscribe(&synth_app.midi_subscription);
  }
  synth_app.midi_subscribed = false;
  synth_app.midi_subscription =
      (solar_os_midi_subscription_t)SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
  memset(synth_app.midi_sustain, 0, sizeof(synth_app.midi_sustain));
}

static void synth_release_all(bool stop) {
  if (stop) {
    (void)solar_os_synth_voice_stop(SYNTH_APP_OWNER);
  } else {
    (void)solar_os_synth_voice_all_notes_off(SYNTH_APP_OWNER);
  }
  memset(synth_app.held, 0, sizeof(synth_app.held));
}

static uint32_t *synth_selected_envelope_value(void) {
  switch (synth_app.selected) {
  case SYNTH_CONTROL_ATTACK:
    return &synth_app.config.attack_ms;
  case SYNTH_CONTROL_DECAY:
    return &synth_app.config.decay_ms;
  case SYNTH_CONTROL_RELEASE:
    return &synth_app.config.release_ms;
  default:
    return NULL;
  }
}

static void synth_adjust_selected(int direction) {
  if (synth_app.selected == SYNTH_CONTROL_WAVE) {
    int waveform = (int)synth_app.config.waveform + direction;
    if (waveform < SOLAR_OS_SYNTH_WAVE_SQUARE) {
      waveform = SOLAR_OS_SYNTH_WAVE_CUSTOM;
    } else if (waveform > SOLAR_OS_SYNTH_WAVE_CUSTOM) {
      waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    }
    synth_app.config.waveform = (solar_os_synth_waveform_t)waveform;
  } else if (synth_app.selected == SYNTH_CONTROL_VOLUME) {
    int volume = (int)synth_app.volume + direction * SYNTH_APP_VOLUME_STEP;
    if (volume < 0) {
      volume = 0;
    } else if (volume > 100) {
      volume = 100;
    }
    synth_app.last_error = solar_os_audio_set_volume((uint8_t)volume);
    if (synth_app.last_error == ESP_OK) {
      synth_app.volume = (uint8_t)volume;
    }
    return;
  } else if (synth_app.selected == SYNTH_CONTROL_SUSTAIN) {
    int sustain = (int)synth_app.config.sustain_percent + direction * 5;
    if (sustain < 0) {
      sustain = 0;
    } else if (sustain > 100) {
      sustain = 100;
    }
    synth_app.config.sustain_percent = (uint8_t)sustain;
  } else {
    uint32_t *value = synth_selected_envelope_value();
    if (value != NULL) {
      size_t index = synth_envelope_value_index(*value);
      const size_t count =
          sizeof(synth_envelope_values) / sizeof(synth_envelope_values[0]);
      if (direction > 0 && index + 1 < count) {
        index++;
      } else if (direction < 0 && index > 0) {
        index--;
      }
      *value = synth_envelope_values[index];
    }
  }
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
}

static uint32_t *synth_selected_filter_envelope_value(void) {
  switch (synth_app.filter_selected) {
  case SYNTH_FILTER_CONTROL_ATTACK:
    return &synth_app.config.filter.attack_ms;
  case SYNTH_FILTER_CONTROL_DECAY:
    return &synth_app.config.filter.decay_ms;
  case SYNTH_FILTER_CONTROL_RELEASE:
    return &synth_app.config.filter.release_ms;
  default:
    return NULL;
  }
}

static void synth_adjust_filter_selected(int direction) {
  if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_CUTOFF) {
    size_t index =
        synth_filter_cutoff_value_index(synth_app.config.filter.cutoff_hz);
    const size_t count = sizeof(synth_filter_cutoff_values) /
                         sizeof(synth_filter_cutoff_values[0]);
    if (direction > 0 && index + 1U < count) {
      index++;
    } else if (direction < 0 && index > 0U) {
      index--;
    }
    synth_app.config.filter.cutoff_hz = synth_filter_cutoff_values[index];
  } else if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_RESONANCE) {
    int resonance =
        (int)synth_app.config.filter.resonance_percent + direction * 5;
    if (resonance < 0) {
      resonance = 0;
    } else if (resonance > 100) {
      resonance = 100;
    }
    synth_app.config.filter.resonance_percent = (uint8_t)resonance;
  } else if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_AMOUNT) {
    int amount =
        (int)synth_app.config.filter.envelope_amount_percent + direction * 5;
    if (amount < 0) {
      amount = 0;
    } else if (amount > 100) {
      amount = 100;
    }
    synth_app.config.filter.envelope_amount_percent = (uint8_t)amount;
  } else if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_SUSTAIN) {
    int sustain = (int)synth_app.config.filter.sustain_percent + direction * 5;
    if (sustain < 0) {
      sustain = 0;
    } else if (sustain > 100) {
      sustain = 100;
    }
    synth_app.config.filter.sustain_percent = (uint8_t)sustain;
  } else {
    uint32_t *value = synth_selected_filter_envelope_value();
    if (value != NULL) {
      size_t index = synth_envelope_value_index(*value);
      const size_t count =
          sizeof(synth_envelope_values) / sizeof(synth_envelope_values[0]);
      if (direction > 0 && index + 1U < count) {
        index++;
      } else if (direction < 0 && index > 0U) {
        index--;
      }
      *value = synth_envelope_values[index];
    }
  }
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
}

static void synth_adjust_oscillator2_selected(int direction) {
  solar_os_synth_oscillator_config_t *oscillator2 =
      &synth_app.config.oscillator2;
  switch (synth_app.oscillator2_selected) {
  case SYNTH_OSCILLATOR2_CONTROL_WAVE: {
    int waveform = (int)oscillator2->waveform + direction;
    if (waveform < SOLAR_OS_SYNTH_WAVE_SQUARE) {
      waveform = SOLAR_OS_SYNTH_WAVE_CUSTOM;
    } else if (waveform > SOLAR_OS_SYNTH_WAVE_CUSTOM) {
      waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    }
    oscillator2->waveform = (solar_os_synth_waveform_t)waveform;
    break;
  }
  case SYNTH_OSCILLATOR2_CONTROL_OCTAVE: {
    int octave = oscillator2->octave + direction;
    if (octave < SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN) {
      octave = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN;
    } else if (octave > SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX) {
      octave = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX;
    }
    oscillator2->octave = (int8_t)octave;
    break;
  }
  case SYNTH_OSCILLATOR2_CONTROL_DETUNE: {
    int detune = oscillator2->detune_cents + direction * 5;
    if (detune < SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS) {
      detune = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS;
    } else if (detune > SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS) {
      detune = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS;
    }
    oscillator2->detune_cents = (int16_t)detune;
    break;
  }
  case SYNTH_OSCILLATOR2_CONTROL_MIX: {
    int mix = oscillator2->mix_percent + direction * 5;
    if (mix < 0) {
      mix = 0;
    } else if (mix > 100) {
      mix = 100;
    }
    oscillator2->mix_percent = (uint8_t)mix;
    break;
  }
  default:
    break;
  }
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
}

static size_t synth_glide_value_index(uint16_t value) {
  size_t best = 0U;
  uint32_t best_distance = UINT32_MAX;
  for (size_t i = 0;
       i < sizeof(synth_glide_values) / sizeof(synth_glide_values[0]); i++) {
    const uint32_t candidate = synth_glide_values[i];
    const uint32_t distance =
        candidate > value ? candidate - value : value - candidate;
    if (distance < best_distance) {
      best = i;
      best_distance = distance;
    }
  }
  return best;
}

static void synth_apply_performance(bool release_notes) {
  if (release_notes) {
    synth_release_all(false);
  }
  synth_app.last_error = solar_os_synth_voice_configure_performance(
      SYNTH_APP_OWNER, &synth_app.performance);
}

static void synth_adjust_mode_selected(int direction) {
  if (synth_app.mode_selected == SYNTH_MODE_CONTROL_VOICES) {
    const bool mono = direction > 0;
    if (synth_app.performance.mono != mono) {
      synth_app.performance.mono = mono;
      synth_apply_performance(true);
    }
    return;
  }
  if (synth_app.mode_selected == SYNTH_MODE_CONTROL_HOLD) {
    (void)synth_set_hold_mode(direction > 0);
    return;
  }

  size_t index = synth_glide_value_index(synth_app.performance.glide_ms);
  const size_t count =
      sizeof(synth_glide_values) / sizeof(synth_glide_values[0]);
  if (direction > 0 && index + 1U < count) {
    index++;
  } else if (direction < 0 && index > 0U) {
    index--;
  }
  synth_app.performance.glide_ms = synth_glide_values[index];
  synth_apply_performance(false);
}

static void synth_move_wave_cursor(int direction, size_t step) {
  const size_t count = synth_app.wave_steps;
  if (step >= count) {
    step = count / 2U;
  }
  if (direction > 0) {
    synth_app.wave_cursor = (synth_app.wave_cursor + step) % count;
  } else {
    synth_app.wave_cursor =
        (synth_app.wave_cursor + count - (step % count)) % count;
  }
}

static bool synth_handle_wave_control(uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
    synth_move_wave_cursor(-1, 1U);
    return true;
  case SOLAR_OS_KEY_RIGHT:
    synth_move_wave_cursor(1, 1U);
    return true;
  case SOLAR_OS_KEY_SHIFT_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
    synth_move_wave_cursor(-1, 8U);
    return true;
  case SOLAR_OS_KEY_SHIFT_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
    synth_move_wave_cursor(1, 8U);
    return true;
  case SOLAR_OS_KEY_CTRL_SHIFT_LEFT:
    synth_move_wave_cursor(-1, 32U);
    return true;
  case SOLAR_OS_KEY_CTRL_SHIFT_RIGHT:
    synth_move_wave_cursor(1, 32U);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
    synth_wavetable_draw(1, false);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
    synth_wavetable_draw(-1, false);
    return true;
  case SOLAR_OS_KEY_SHIFT_UP:
  case SOLAR_OS_KEY_CTRL_SHIFT_UP:
    synth_wavetable_draw(1, true);
    return true;
  case SOLAR_OS_KEY_SHIFT_DOWN:
  case SOLAR_OS_KEY_CTRL_SHIFT_DOWN:
    synth_wavetable_draw(-1, true);
    return true;
  case SOLAR_OS_KEY_PAGE_UP:
  case SOLAR_OS_KEY_SHIFT_PAGE_UP:
    if (synth_app.octave < SYNTH_APP_OCTAVE_MAX) {
      synth_app.octave++;
    }
    return true;
  case SOLAR_OS_KEY_PAGE_DOWN:
  case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
    if (synth_app.octave > SYNTH_APP_OCTAVE_MIN) {
      synth_app.octave--;
    }
    return true;
  case SOLAR_OS_KEY_HOME:
  case SOLAR_OS_KEY_SHIFT_HOME:
  case SOLAR_OS_KEY_CTRL_HOME:
  case SOLAR_OS_KEY_CTRL_SHIFT_HOME:
    synth_app.wave_cursor = 0;
    return true;
  case SOLAR_OS_KEY_END:
  case SOLAR_OS_KEY_SHIFT_END:
  case SOLAR_OS_KEY_CTRL_END:
  case SOLAR_OS_KEY_CTRL_SHIFT_END:
    synth_app.wave_cursor = synth_app.wave_steps - 1U;
    return true;
  case SOLAR_OS_KEY_ENTER:
    synth_wavetable_cycle_steps();
    return true;
  case '+':
  case '=':
    if (synth_app.wave_brush < synth_wavetable_brush_max()) {
      synth_app.wave_brush++;
    }
    return true;
  case '-':
    if (synth_app.wave_brush > 0U) {
      synth_app.wave_brush--;
    }
    return true;
  case 'b':
  case 'B':
    synth_wavetable_seed(
        (synth_wave_baseline_t)((synth_app.baseline + 1) % SYNTH_BASE_COUNT),
        true);
    return true;
  case 'r':
  case 'R':
    synth_wavetable_seed(synth_app.baseline, true);
    return true;
  case 'm':
  case 'M':
    synth_wavetable_smooth();
    return true;
  case 'n':
  case 'N':
    synth_wavetable_normalize();
    return true;
  case '0':
    synth_wavetable_seed(SYNTH_BASE_FLAT, true);
    return true;
  case '\b':
  case 0x7fU:
  case SOLAR_OS_KEY_DELETE:
    synth_wavetable_undo();
    return true;
  default:
    return false;
  }
}

static bool synth_handle_filter_control(uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.filter_selected =
        synth_app.filter_selected == 0
            ? SYNTH_FILTER_CONTROL_COUNT - 1
            : (synth_filter_control_t)(synth_app.filter_selected - 1);
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.filter_selected =
        (synth_filter_control_t)((synth_app.filter_selected + 1) %
                                 SYNTH_FILTER_CONTROL_COUNT);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP:
    synth_adjust_filter_selected(1);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN:
    synth_adjust_filter_selected(-1);
    return true;
  case SOLAR_OS_KEY_PAGE_UP:
  case SOLAR_OS_KEY_SHIFT_PAGE_UP:
    if (synth_app.octave < SYNTH_APP_OCTAVE_MAX) {
      synth_app.octave++;
    }
    return true;
  case SOLAR_OS_KEY_PAGE_DOWN:
  case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
    if (synth_app.octave > SYNTH_APP_OCTAVE_MIN) {
      synth_app.octave--;
    }
    return true;
  case '+':
  case '=':
    if (synth_app.velocity <=
        SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX - SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity += SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX;
    }
    return true;
  case '-':
    if (synth_app.velocity > SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity -= SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = 1;
    }
    return true;
  default:
    return false;
  }
}

static bool synth_handle_oscillator2_control(uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.oscillator2_selected =
        synth_app.oscillator2_selected == 0
            ? SYNTH_OSCILLATOR2_CONTROL_COUNT - 1
            : (synth_oscillator2_control_t)(synth_app.oscillator2_selected - 1);
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.oscillator2_selected =
        (synth_oscillator2_control_t)((synth_app.oscillator2_selected + 1) %
                                      SYNTH_OSCILLATOR2_CONTROL_COUNT);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP:
    synth_adjust_oscillator2_selected(1);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN:
    synth_adjust_oscillator2_selected(-1);
    return true;
  case SOLAR_OS_KEY_PAGE_UP:
  case SOLAR_OS_KEY_SHIFT_PAGE_UP:
    if (synth_app.octave < SYNTH_APP_OCTAVE_MAX) {
      synth_app.octave++;
    }
    return true;
  case SOLAR_OS_KEY_PAGE_DOWN:
  case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
    if (synth_app.octave > SYNTH_APP_OCTAVE_MIN) {
      synth_app.octave--;
    }
    return true;
  case '+':
  case '=':
    if (synth_app.velocity <=
        SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX - SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity += SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX;
    }
    return true;
  case '-':
    if (synth_app.velocity > SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity -= SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = 1;
    }
    return true;
  default:
    return false;
  }
}

static bool synth_handle_preset_control(uint8_t key) {
  const size_t row = synth_app.preset_selected % SYNTH_APP_FACTORY_PRESET_COUNT;
  const bool user = synth_app.preset_selected >= SYNTH_APP_FACTORY_PRESET_COUNT;
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.preset_selected = row;
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.preset_selected = SYNTH_APP_FACTORY_PRESET_COUNT + row;
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP: {
    const size_t previous = (row + SYNTH_APP_FACTORY_PRESET_COUNT - 1U) %
                            SYNTH_APP_FACTORY_PRESET_COUNT;
    synth_app.preset_selected =
        (user ? SYNTH_APP_FACTORY_PRESET_COUNT : 0U) + previous;
    return true;
  }
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN: {
    const size_t next = (row + 1U) % SYNTH_APP_FACTORY_PRESET_COUNT;
    synth_app.preset_selected =
        (user ? SYNTH_APP_FACTORY_PRESET_COUNT : 0U) + next;
    return true;
  }
  case SOLAR_OS_KEY_ENTER:
    synth_preset_load_selected();
    return true;
  case 'v':
  case 'V':
    synth_preset_save_selected();
    return true;
  case SOLAR_OS_KEY_PAGE_UP:
  case SOLAR_OS_KEY_SHIFT_PAGE_UP:
    if (synth_app.octave < SYNTH_APP_OCTAVE_MAX) {
      synth_app.octave++;
    }
    return true;
  case SOLAR_OS_KEY_PAGE_DOWN:
  case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
    if (synth_app.octave > SYNTH_APP_OCTAVE_MIN) {
      synth_app.octave--;
    }
    return true;
  case '+':
  case '=':
    if (synth_app.velocity <=
        SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX - SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity += SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX;
    }
    return true;
  case '-':
    if (synth_app.velocity > SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity -= SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = 1;
    }
    return true;
  default:
    return false;
  }
}

static bool synth_handle_mode_control(uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.mode_selected =
        synth_app.mode_selected == 0
            ? SYNTH_MODE_CONTROL_COUNT - 1
            : (synth_mode_control_t)(synth_app.mode_selected - 1);
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.mode_selected = (synth_mode_control_t)(
        (synth_app.mode_selected + 1) % SYNTH_MODE_CONTROL_COUNT);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP:
    synth_adjust_mode_selected(1);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN:
    synth_adjust_mode_selected(-1);
    return true;
  case SOLAR_OS_KEY_ENTER:
    if (synth_app.mode_selected == SYNTH_MODE_CONTROL_VOICES) {
      synth_app.performance.mono = !synth_app.performance.mono;
      synth_apply_performance(true);
      return true;
    }
    if (synth_app.mode_selected == SYNTH_MODE_CONTROL_HOLD) {
      (void)synth_set_hold_mode(!synth_app.hold_mode);
      return true;
    }
    return false;
  case SOLAR_OS_KEY_PAGE_UP:
  case SOLAR_OS_KEY_SHIFT_PAGE_UP:
    if (synth_app.octave < SYNTH_APP_OCTAVE_MAX) {
      synth_app.octave++;
    }
    return true;
  case SOLAR_OS_KEY_PAGE_DOWN:
  case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
    if (synth_app.octave > SYNTH_APP_OCTAVE_MIN) {
      synth_app.octave--;
    }
    return true;
  default:
    return false;
  }
}

static void synth_select_tab(synth_tab_t tab) {
  synth_app.tab = tab;
}

static bool synth_handle_control(solar_os_context_t *ctx, uint8_t key) {
  if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE) {
    solar_os_context_request_exit(ctx);
    return true;
  }
  synth_app.compact_parameter_valid = false;
  if (key == '\t') {
    synth_select_tab((synth_tab_t)((synth_app.tab + 1) % SYNTH_TAB_COUNT));
    return true;
  }
  if (key >= '1' && key <= '6') {
    synth_select_tab((synth_tab_t)(key - '1'));
    return true;
  }
  if (key == 'x' || key == 'X') {
    synth_app.keyboard_visible = !synth_app.keyboard_visible;
    return true;
  }
  if (synth_app.tab == SYNTH_TAB_WAVE) {
    return synth_handle_wave_control(key);
  }
  if (synth_app.tab == SYNTH_TAB_FILTER) {
    return synth_handle_filter_control(key);
  }
  if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
    return synth_handle_oscillator2_control(key);
  }
  if (synth_app.tab == SYNTH_TAB_PRESET) {
    return synth_handle_preset_control(key);
  }
  if (synth_app.tab == SYNTH_TAB_MODE) {
    return synth_handle_mode_control(key);
  }

  switch (key) {
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.selected = synth_app.selected == 0
                             ? SYNTH_CONTROL_COUNT - 1
                             : (synth_control_t)(synth_app.selected - 1);
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.selected =
        (synth_control_t)((synth_app.selected + 1) % SYNTH_CONTROL_COUNT);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP:
    synth_adjust_selected(1);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN:
    synth_adjust_selected(-1);
    return true;
  case SOLAR_OS_KEY_PAGE_UP:
  case SOLAR_OS_KEY_SHIFT_PAGE_UP:
    if (synth_app.octave < SYNTH_APP_OCTAVE_MAX) {
      synth_app.octave++;
    }
    return true;
  case SOLAR_OS_KEY_PAGE_DOWN:
  case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
    if (synth_app.octave > SYNTH_APP_OCTAVE_MIN) {
      synth_app.octave--;
    }
    return true;
  case '+':
  case '=':
    if (synth_app.velocity <=
        SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX - SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity += SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX;
    }
    return true;
  case '-':
    if (synth_app.velocity > SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity -= SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = 1;
    }
    return true;
  default:
    return false;
  }
}

static esp_err_t synth_start(solar_os_context_t *ctx) {
  const int argc = solar_os_context_argc(ctx);
  const bool headless =
      argc == 2 && strcmp(solar_os_context_argv(ctx, 1), "--headless") == 0;
  if (argc > 2 || (argc == 2 && !headless)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!headless && solar_os_context_gfx(ctx) == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  memset(&synth_app, 0, sizeof(synth_app));
  synth_app.headless = headless;
  synth_app.midi_subscription =
      (solar_os_midi_subscription_t)SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
  synth_app.config = synth_default_config();
  synth_app.performance = synth_default_performance();
  synth_app.octave = 4;
  synth_app.velocity = SOLAR_OS_SYNTH_VOICE_DEFAULT_VELOCITY;
  synth_app.keyboard_visible = true;
  synth_app.tab = SYNTH_TAB_PLAY;
  synth_app.baseline = SYNTH_BASE_SQUARE;
  synth_app.wave_steps = SYNTH_APP_DEFAULT_WAVE_STEPS;
  synth_app.wave_cursor = synth_app.wave_steps / 4U;
  synth_app.wave_brush = 1U;
  snprintf(synth_app.preset_message, sizeof(synth_app.preset_message),
           "Arrows select  Enter load  V save");
  synth_preset_scan_slots();
  synth_wavetable_fill(synth_app.baseline);
  solar_os_audio_status_t audio_status;
  solar_os_audio_get_status(&audio_status);
  synth_app.volume = audio_status.volume;
  synth_app.last_error = synth_wavetable_upload();
  if (synth_app.last_error == ESP_OK) {
    synth_app.last_error =
        solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  }
  synth_midi_subscribe();
  if (synth_app.last_error == ESP_OK) {
    synth_app.last_error = solar_os_synth_voice_configure_performance(
        SYNTH_APP_OWNER, &synth_app.performance);
  }
  synth_parameters_register();
  solar_os_context_set_graphics_active(ctx, !synth_app.headless);
  synth_render(ctx);
  return ESP_OK;
}

static void synth_stop(solar_os_context_t *ctx) {
  synth_parameters_unregister();
  synth_midi_unsubscribe();
  synth_release_all(true);
  synth_app.suspended = false;
  solar_os_context_set_graphics_active(ctx, false);
}

static void synth_suspend(solar_os_context_t *ctx) {
  synth_parameters_unregister();
  synth_midi_unsubscribe();
  synth_release_all(true);
  synth_app.suspended = true;
  solar_os_context_set_graphics_active(ctx, false);
}

static void synth_resume(solar_os_context_t *ctx) {
  synth_app.suspended = false;
  synth_app.last_error = synth_wavetable_upload();
  if (synth_app.last_error == ESP_OK) {
    synth_app.last_error =
        solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  }
  synth_midi_subscribe();
  if (synth_app.last_error == ESP_OK) {
    synth_app.last_error = solar_os_synth_voice_configure_performance(
        SYNTH_APP_OWNER, &synth_app.performance);
  }
  synth_parameters_register();
  solar_os_context_set_graphics_active(ctx, !synth_app.headless);
  synth_render(ctx);
}

static bool synth_event(solar_os_context_t *ctx,
                        const solar_os_event_t *event) {
  if (event == NULL) {
    return false;
  }

  if (event->type == SOLAR_OS_EVENT_KEY) {
    const solar_os_input_key_event_t *key = &event->data.key;
    const int semitone = synth_semitone_for_key(key);
    bool changed = false;
    bool performance_activity = false;
    if (semitone >= 0) {
      if (key->action == SOLAR_OS_INPUT_KEY_PRESS) {
        performance_activity = true;
        changed = synth_app.hold_mode ? synth_toggle_note(key, semitone)
                                      : synth_note_on(key, semitone);
      } else if (key->action == SOLAR_OS_INPUT_KEY_RELEASE &&
                 !synth_app.hold_mode) {
        performance_activity = true;
        changed = synth_note_off(key, semitone);
      }
    } else if (key->action == SOLAR_OS_INPUT_KEY_REPEAT &&
               (key->key == 'x' || key->key == 'X')) {
      changed = false;
    } else if (key->action != SOLAR_OS_INPUT_KEY_RELEASE) {
      changed = synth_handle_control(ctx, key->key);
    }
    synth_render_changed(ctx, changed, performance_activity, synth_now_ms());
    return true;
  }

  if (event->type == SOLAR_OS_EVENT_CHAR) {
    solar_os_input_key_event_t key = {
        .key = (uint8_t)event->data.ch,
        .action = SOLAR_OS_INPUT_KEY_PRESS,
    };
    const int semitone = synth_semitone_for_key(&key);
    const bool changed =
        semitone >= 0
            ? (synth_app.hold_mode ? synth_toggle_note(&key, semitone)
                                   : synth_note_on(&key, semitone))
            : synth_handle_control(ctx, key.key);
    synth_render_changed(ctx, changed, semitone >= 0, synth_now_ms());
    return true;
  }

  if (event->type == SOLAR_OS_EVENT_TICK) {
    bool changed = synth_app.parameter_dirty;
    bool performance_activity = false;
    synth_app.parameter_dirty = false;
    if (synth_app.midi_subscribed) {
      solar_os_midi_message_t message;
      while (solar_os_midi_receive(&synth_app.midi_subscription, &message) ==
             ESP_OK) {
        performance_activity = true;
        changed |= synth_handle_midi_message(&message);
      }
    }
    for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
      synth_held_note_t *held = &synth_app.held[i];
      if (held->active && held->release_at_ms != 0 &&
          (int32_t)(event->data.tick_ms - held->release_at_ms) >= 0) {
        const bool released = synth_release_held(held);
        changed |= released;
        performance_activity |= released;
      }
    }

    if ((uint32_t)(event->data.tick_ms - synth_app.last_status_poll_ms) >=
        SYNTH_APP_STATUS_POLL_MS) {
      solar_os_synth_voice_status_t status;
      solar_os_synth_voice_get_status(&status);
      synth_app.last_status_poll_ms = event->data.tick_ms;
      if (status.active_voices != synth_app.last_active_voices ||
          status.render_deadline_misses != synth_app.last_deadline_misses ||
          (synth_scope_visible(ctx) &&
           status.pcm_generation != synth_app.last_pcm_generation) ||
          status.running != synth_app.last_running) {
        changed = true;
      }
    }
    synth_render_changed(ctx, changed, performance_activity,
                         event->data.tick_ms);
    return true;
  }

  if (event->type == SOLAR_OS_EVENT_RESUME) {
    synth_resume(ctx);
    return true;
  }
  return false;
}

const solar_os_app_t solar_os_synth_app = {
    .name = "synth",
    .summary = "synthesizer and sound designer",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_KEY_EVENTS,
    .start = synth_start,
    .suspend = synth_suspend,
    .resume = synth_resume,
    .stop = synth_stop,
    .event = synth_event,
    .state_slot = &synth_app_state,
    .state_size = sizeof(synth_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 5U,
    .tick_deadline_ms = 10U,
};
