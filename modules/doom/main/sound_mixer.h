// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_DOOM_SOUND_CHANNELS 32U
typedef struct {
    const uint8_t *data;
    uint32_t length, rate, step;
    uint64_t position;
    int handle, left, right;
} solar_os_doom_sound_voice_t;
typedef struct {
    solar_os_doom_sound_voice_t voices[SOLAR_OS_DOOM_SOUND_CHANNELS];
    uint32_t rate, generation;
} solar_os_doom_sound_mixer_t;

void solar_os_doom_sound_mixer_init(solar_os_doom_sound_mixer_t *mixer, uint32_t rate);
int solar_os_doom_sound_mixer_play(solar_os_doom_sound_mixer_t *mixer, unsigned channel,
    const uint8_t *data, uint32_t length, uint32_t rate, int volume, int separation, uint32_t pitch);
void solar_os_doom_sound_mixer_update(solar_os_doom_sound_mixer_t *mixer,
    int handle, int volume, int separation, uint32_t pitch);
void solar_os_doom_sound_mixer_stop(solar_os_doom_sound_mixer_t *mixer, int handle);
bool solar_os_doom_sound_mixer_playing(const solar_os_doom_sound_mixer_t *mixer, int handle);
void solar_os_doom_sound_mixer_render(solar_os_doom_sound_mixer_t *mixer, int16_t *out, size_t frames);
