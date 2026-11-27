#include "lua_runtime.h"

#define NUMBER(index, minimum, maximum, fallback) \
    solar_os_lua_integer(L, index, minimum, maximum, fallback)

static int audio_tone(lua_State *L)
{
    const solar_os_audio_tone_step_t tone = {
        .frequency_hz = (uint32_t)NUMBER(1, SOLAR_OS_AUDIO_TONE_MIN_HZ, SOLAR_OS_AUDIO_TONE_MAX_HZ, 440),
        .duration_ms = (uint32_t)NUMBER(2, 1, SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_MS, 0),
    };
    const solar_os_audio_tone_request_t request = {
        .steps = &tone,
        .step_count = 1,
        .volume = (uint8_t)NUMBER(3, 0, 255, SOLAR_OS_AUDIO_FILE_VOLUME_GLOBAL),
        .drop_if_busy = true,
    };
    uint32_t id = 0;
    int result = lua_runtime.audio->enqueue_tone(&request, &id);
    if (!result) lua_runtime.tone = id;
    return solar_os_lua_check(L, result);
}

static solar_os_native_handle_t synth_handle(lua_State *L)
{
    if (!lua_runtime.synth) solar_os_lua_check(L, lua_runtime.voice->open(&lua_runtime.synth));
    return lua_runtime.synth;
}

static solar_os_synth_voice_status_t synth_current(lua_State *L)
{
    solar_os_synth_voice_status_t status;
    solar_os_lua_check(L, lua_runtime.voice->status(&status));
    return status;
}

static solar_os_synth_waveform_t waveform(lua_State *L)
{
    solar_os_synth_waveform_t wave;
    if (!lua_runtime.voice->parse_waveform(luaL_checkstring(L, 1), &wave))
        luaL_argerror(L, 1, "unknown waveform");
    return wave;
}

static int synth_status(lua_State *L)
{
    solar_os_synth_voice_status_t status = synth_current(L);
    lua_newtable(L);
    lua_pushstring(L, status.owner);
    lua_setfield(L, -2, "owner");
    lua_pushboolean(L, status.running);
    lua_setfield(L, -2, "running");
    return 1;
}

static int synth_configure(lua_State *L)
{
    solar_os_synth_voice_config_t config = synth_current(L).config;
    config.waveform = waveform(L);
    config.attack_ms = NUMBER(2, 0, UINT32_MAX, SOLAR_OS_SYNTH_VOICE_DEFAULT_ATTACK_MS);
    config.decay_ms = NUMBER(3, 0, UINT32_MAX, SOLAR_OS_SYNTH_VOICE_DEFAULT_DECAY_MS);
    config.sustain_percent = NUMBER(4, 0, 100, SOLAR_OS_SYNTH_VOICE_DEFAULT_SUSTAIN_PERCENT);
    config.release_ms = NUMBER(5, 0, UINT32_MAX, SOLAR_OS_SYNTH_VOICE_DEFAULT_RELEASE_MS);
    return solar_os_lua_check(L, lua_runtime.voice->configure(synth_handle(L), &config));
}

static int synth_oscillator(lua_State *L)
{
    solar_os_synth_voice_config_t config = synth_current(L).config;
    config.oscillator2 = (solar_os_synth_oscillator_config_t){
        .waveform = waveform(L),
        .octave = NUMBER(2, -2, 2, SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_OCTAVE),
        .detune_cents = NUMBER(3, -100, 100, SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_DETUNE_CENTS),
        .mix_percent = NUMBER(4, 0, 100, SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_MIX_PERCENT),
    };
    return solar_os_lua_check(L, lua_runtime.voice->configure(synth_handle(L), &config));
}

static int synth_filter(lua_State *L)
{
    solar_os_synth_voice_config_t config = synth_current(L).config;
    config.filter = (solar_os_synth_filter_config_t){
        .cutoff_hz = NUMBER(1, 0, UINT32_MAX, 0),
        .resonance_percent = NUMBER(2, 0, 100, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RESONANCE_PERCENT),
        .envelope_amount_percent = NUMBER(3, 0, 100, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ENVELOPE_AMOUNT_PERCENT),
        .attack_ms = NUMBER(4, 0, UINT32_MAX, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ATTACK_MS),
        .decay_ms = NUMBER(5, 0, UINT32_MAX, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_DECAY_MS),
        .sustain_percent = NUMBER(6, 0, 100, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_SUSTAIN_PERCENT),
        .release_ms = NUMBER(7, 0, UINT32_MAX, SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RELEASE_MS),
    };
    return solar_os_lua_check(L, lua_runtime.voice->configure(synth_handle(L), &config));
}

static int synth_performance(lua_State *L)
{
    const solar_os_synth_voice_performance_t performance = {
        .mono = lua_toboolean(L, 1),
        .glide_ms = NUMBER(2, 0, SOLAR_OS_SYNTH_VOICE_GLIDE_MAX_MS, SOLAR_OS_SYNTH_VOICE_DEFAULT_GLIDE_MS),
    };
    return solar_os_lua_check(L, lua_runtime.voice->configure_performance(synth_handle(L), &performance));
}

static int synth_on(lua_State *L)
{
    const uint32_t hz = NUMBER(1, 1, UINT32_MAX, 0);
    const uint8_t velocity = NUMBER(2, 0, 127, SOLAR_OS_SYNTH_VOICE_DEFAULT_VELOCITY);
    return solar_os_lua_check(L, lua_runtime.voice->note_on(synth_handle(L), hz, velocity));
}

static int synth_off(lua_State *L)
{
    const uint32_t hz = NUMBER(1, 1, UINT32_MAX, 0);
    return solar_os_lua_check(L, lua_runtime.voice->note_off(synth_handle(L), hz));
}

static int synth_all_off(lua_State *L)
{
    return solar_os_lua_check(L, lua_runtime.voice->all_notes_off(synth_handle(L)));
}

static int synth_stop(lua_State *L)
{
    if (!lua_runtime.synth) return 0;
    const int result = lua_runtime.voice->close(lua_runtime.synth);
    if (!result) lua_runtime.synth = 0;
    return solar_os_lua_check(L, result);
}

void solar_os_lua_audio(lua_State *L)
{
    if (lua_runtime.audio) {
        static const luaL_Reg audio[] = {{"tone", audio_tone}, {NULL, NULL}};
        luaL_newlib(L, audio);
        lua_setfield(L, -2, "audio");
    }
    if (lua_runtime.voice) {
        static const luaL_Reg synth[] = {
            {"status", synth_status}, {"configure", synth_configure},
            {"configure_oscillator2", synth_oscillator}, {"configure_filter", synth_filter},
            {"configure_performance", synth_performance}, {"note_on", synth_on}, {"note_off", synth_off},
            {"all_notes_off", synth_all_off}, {"stop", synth_stop}, {NULL, NULL},
        };
        luaL_newlib(L, synth);
        lua_setfield(L, -2, "synth");
    }
}
