// SPDX-License-Identifier: GPL-2.0-or-later
#include "sound_mixer.h"
#include <string.h>
#include <limits.h>

void solar_os_doom_sound_mixer_init(solar_os_doom_sound_mixer_t *mixer, uint32_t rate)
{ memset(mixer, 0, sizeof(*mixer)); mixer->rate = rate; }

bool solar_os_doom_sound_mixer_playing(const solar_os_doom_sound_mixer_t *mixer, int handle)
{
    if (handle <= 0) return false;
    const solar_os_doom_sound_voice_t *voice = &mixer->voices[(unsigned)handle & 31U];
    return voice->handle == handle && voice->data != NULL;
}
void solar_os_doom_sound_mixer_update(solar_os_doom_sound_mixer_t *mixer,
    int handle, int volume, int separation, uint32_t pitch)
{
    if (!solar_os_doom_sound_mixer_playing(mixer, handle)) return;
    solar_os_doom_sound_voice_t *voice = &mixer->voices[(unsigned)handle & 31U];
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    if (separation < 0) separation = 0;
    if (separation > 255) separation = 255;
    voice->left = separation <= 128 ? volume : volume * (255 - separation) / 127;
    voice->right = separation >= 128 ? volume : volume * separation / 128;
    /* Q16 pitch and position support arbitrary negotiated output rates. */
    if (pitch < 16384U) pitch = 16384U;
    if (pitch > 262144U) pitch = 262144U;
    voice->step = (uint64_t)voice->rate * pitch / mixer->rate;
    if (!voice->step) voice->step = 1;
}
int solar_os_doom_sound_mixer_play(solar_os_doom_sound_mixer_t *mixer, unsigned channel,
    const uint8_t *data, uint32_t length, uint32_t rate, int volume, int separation, uint32_t pitch)
{
    if (channel >= SOLAR_OS_DOOM_SOUND_CHANNELS || !data || !length ||
        !rate || rate > 65535U || mixer->rate < 8000U || mixer->rate > 96000U) return -1;
    mixer->generation = (mixer->generation + 1U) & 0x03ffffffU;
    if (!mixer->generation) mixer->generation = 1;
    int handle = (int)((mixer->generation << 5) | channel);
    mixer->voices[channel] = (solar_os_doom_sound_voice_t){
        .data = data, .length = length, .rate = rate, .handle = handle,
    };
    solar_os_doom_sound_mixer_update(mixer, handle, volume, separation, pitch);
    return handle;
}
void solar_os_doom_sound_mixer_stop(solar_os_doom_sound_mixer_t *mixer, int handle)
{
    if (solar_os_doom_sound_mixer_playing(mixer, handle))
        mixer->voices[(unsigned)handle & 31U].data = NULL;
}
void solar_os_doom_sound_mixer_render(solar_os_doom_sound_mixer_t *mixer, int16_t *out, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        int32_t left = 0, right = 0;
        for (unsigned n = 0; n < SOLAR_OS_DOOM_SOUND_CHANNELS; n++) {
            solar_os_doom_sound_voice_t *voice = &mixer->voices[n];
            if (!voice->data) continue;
            uint32_t position = voice->position >> 16;
            if (position >= voice->length) { voice->data = NULL; continue; }
            uint32_t next = position + 1U < voice->length ? position + 1U : position;
            int32_t sample = ((int)voice->data[position] - 128) * 256;
            sample += ((int)voice->data[next] - voice->data[position]) *
                (int32_t)(voice->position & 65535U) / 256;
            left += sample * voice->left / 127;
            right += sample * voice->right / 127;
            voice->position += voice->step;
            if ((voice->position >> 16) >= voice->length) voice->data = NULL;
        }
        if (left > INT16_MAX) left = INT16_MAX;
        if (left < INT16_MIN) left = INT16_MIN;
        if (right > INT16_MAX) right = INT16_MAX;
        if (right < INT16_MIN) right = INT16_MIN;
        out[i * 2] = left; out[i * 2 + 1] = right;
    }
}
