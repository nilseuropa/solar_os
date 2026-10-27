#include "sound_mixer.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void)
{
    solar_os_doom_sound_mixer_t mixer;
    int16_t out[16];
    const uint8_t waveform[] = {128, 255, 0, 128};
    solar_os_doom_sound_mixer_init(&mixer, 16000);
    solar_os_doom_sound_mixer_render(&mixer, out, 8);
    for (unsigned i = 0; i < 16; i++) assert(out[i] == 0);
    assert(solar_os_doom_sound_mixer_play(&mixer, 32, waveform, 4, 16000, 127, 128, 65536) == -1);
    assert(solar_os_doom_sound_mixer_play(&mixer, 0, NULL, 4, 16000, 127, 128, 65536) == -1);
    int handle = solar_os_doom_sound_mixer_play(&mixer, 0, waveform, 4, 16000, 127, 128, 65536);
    assert(solar_os_doom_sound_mixer_playing(&mixer, handle));
    solar_os_doom_sound_mixer_render(&mixer, out, 4);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 32512 && out[3] == 32512);
    assert(out[4] == INT16_MIN && out[5] == INT16_MIN);
    assert(!solar_os_doom_sound_mixer_playing(&mixer, handle));

    handle = solar_os_doom_sound_mixer_play(&mixer, 0, waveform + 1, 1, 8000, 127, 0, 65536);
    solar_os_doom_sound_mixer_render(&mixer, out, 1);
    assert(out[0] == 32512 && out[1] == 0);
    solar_os_doom_sound_mixer_update(&mixer, handle, 64, 255, 65536);
    solar_os_doom_sound_mixer_render(&mixer, out, 1);
    assert(out[0] == 0 && out[1] == 32512 * 64 / 127);
    assert(!solar_os_doom_sound_mixer_playing(&mixer, handle));

    /* Interpolate 8 kHz samples at 16 kHz; pitch changes the read speed. */
    handle = solar_os_doom_sound_mixer_play(&mixer, 0, waveform, 4, 8000, 127, 128, 65536);
    solar_os_doom_sound_mixer_render(&mixer, out, 3);
    assert(out[0] == 0 && out[2] == 16256 && out[4] == 32512);
    solar_os_doom_sound_mixer_update(&mixer, handle, 127, 128, 131072);
    assert(mixer.voices[0].step == 65536);
    int replacement = solar_os_doom_sound_mixer_play(&mixer, 0, waveform, 4, 16000, 127, 128, 65536);
    assert(handle != replacement && !solar_os_doom_sound_mixer_playing(&mixer, handle));
    solar_os_doom_sound_mixer_stop(&mixer, handle);
    assert(solar_os_doom_sound_mixer_playing(&mixer, replacement));
    solar_os_doom_sound_mixer_stop(&mixer, replacement);
    assert(!solar_os_doom_sound_mixer_playing(&mixer, replacement));

    /* Multiple full-level voices saturate, rather than wrapping. */
    solar_os_doom_sound_mixer_play(&mixer, 0, waveform + 1, 2, 16000, 127, 128, 65536);
    solar_os_doom_sound_mixer_play(&mixer, 1, waveform + 1, 2, 16000, 127, 128, 65536);
    solar_os_doom_sound_mixer_render(&mixer, out, 2);
    assert(out[0] == INT16_MAX && out[1] == INT16_MAX);
    assert(out[2] == INT16_MIN && out[3] == INT16_MIN);
    solar_os_doom_sound_mixer_render(&mixer, out, 1);
    assert(out[0] == 0 && out[1] == 0);
    puts("DOOM sound mixer tests passed");
    return 0;
}
