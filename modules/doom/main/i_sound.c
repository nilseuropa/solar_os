/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  System interface for sound.
 *
 *-----------------------------------------------------------------------------
 */

#include "config.h"
#include <math.h>
#include <unistd.h>


#include "z_zone.h"

#include "m_swap.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "lprintf.h"
#include "s_sound.h"

#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"

#include "d_main.h"
#include "sound_mixer.h"

int snd_card = 0;
int mus_card = 0;
int snd_samplerate = 0;

static solar_os_doom_sound_mixer_t mixer;
static bool sound_initialized, sound_running;
static struct {
  const uint8_t *data;
  uint32_t length, rate;
  int lump;
  bool tried;
} sound_cache[NUMSFX];

static void render_effects(int16_t *samples, size_t frames, uint32_t rate, void *user)
{
  (void)user;
  if (rate != mixer.rate) { memset(samples, 0, frames * 2U * sizeof(*samples)); return; }
  solar_os_doom_sound_mixer_render(&mixer, samples, frames);
}

void solar_os_doom_sound_suspend(void)
{
  if (!sound_initialized || !sound_running) return;
  /* close joins the renderer before the module touches or frees its state. */
  solar_os_doom_app->synth_close();
  sound_running = false;
}

void solar_os_doom_sound_resume(void)
{
  if (!sound_initialized || sound_running) return;
  uint32_t rate = 0;
  uint8_t channels = 0;
  /* Probe the selected output using the existing negotiated PCM API. The
   * synth callback then owns that output and mixes independently of video. */
  int result = solar_os_doom_app->audio_open(&rate, &channels);
  if (result == SOLAR_OS_NATIVE_OK) {
    solar_os_doom_app->audio_close();
    if (rate < 8000U || rate > 96000U || !channels || channels > 2)
      result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
  }
  if (result == SOLAR_OS_NATIVE_OK) {
    if (mixer.rate && mixer.rate != rate)
      for (unsigned i = 0; i < SOLAR_OS_DOOM_SOUND_CHANNELS; i++) {
        solar_os_doom_sound_voice_t *voice = &mixer.voices[i];
        if (voice->data) {
          voice->step = (uint64_t)voice->step * mixer.rate / rate;
          if (!voice->step) voice->step = 1;
        }
      }
    mixer.rate = rate;
    result = solar_os_doom_app->synth_open(render_effects, NULL, 256, rate);
  }
  sound_running = result == SOLAR_OS_NATIVE_OK;
  if (sound_running) {
    snd_samplerate = rate;
    lprintf(LO_CONFIRM, "doom: sound effects enabled at %u Hz\n", (unsigned)rate);
  } else lprintf(LO_WARN, "doom: audio unavailable (%d); continuing silently\n", result);
}

static uint32_t sound_pitch(int pitch)
{
  if (!pitched_sounds || !solar_os_doom_libc->pow_float) return 65536U;
  if (pitch < 0) pitch = 0;
  if (pitch > 255) pitch = 255;
  return solar_os_doom_libc->pow_float(2.0f, (pitch - 128) / 64.0f) * 65536.0f;
}

static bool cache_effect(int id)
{
  if (sound_cache[id].tried) return sound_cache[id].data != NULL;
  sound_cache[id].tried = true;
  int lump = S_sfx[id].lumpnum;
  if (lump < 0) lump = I_GetSfxLumpNum(&S_sfx[id]);
  if (lump < 0 || lump >= numlumps) return false;
  int size = W_LumpLength(lump);
  if (size < 8) return false;
  const uint8_t *data = W_CacheLumpNum(lump);
  uint32_t rate = data[2] | (uint32_t)data[3] << 8;
  uint32_t length = data[4] | (uint32_t)data[5] << 8 |
    (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
  /* DMX type 3: unsigned 8-bit PCM, with 16 padding samples at each end. */
  if (data[0] != 3 || data[1] || !rate || length <= 48U || length > (uint32_t)size - 8U) {
    W_UnlockLumpNum(lump);
    return false;
  }
  sound_cache[id].data = data + 8U + 16U;
  sound_cache[id].length = length - 32U;
  sound_cache[id].rate = rate;
  sound_cache[id].lump = lump;
  /* The mapping stays pinned until audio closes; callbacks never access VFS. */
  return true;
}

void I_UpdateSoundParams(int handle, int volume, int seperation, int pitch)
{
  if (!sound_initialized || !solar_os_doom_app->synth_lock()) return;
  solar_os_doom_sound_mixer_update(&mixer, handle, volume, seperation, sound_pitch(pitch));
  solar_os_doom_app->synth_unlock();
}


void I_SetChannels(void)
{
  if (numChannels < 1) numChannels = 1;
  if (numChannels > SOLAR_OS_DOOM_SOUND_CHANNELS) numChannels = SOLAR_OS_DOOM_SOUND_CHANNELS;
}

int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
  if (!sfx) return -1;
  if (sfx->link) sfx = sfx->link;
  if (!sfx->name || strlen(sfx->name) > 6U) return -1;
  char name[9];
  snprintf(name, sizeof(name), "DS%s", sfx->name);
  return W_CheckNumForName(name);
}

int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority)
{
  (void)priority;
  if (!sound_running || id < 1 || id >= NUMSFX || channel < 0 ||
      channel >= SOLAR_OS_DOOM_SOUND_CHANNELS || !cache_effect(id)) return -1;
  if (!solar_os_doom_app->synth_lock()) return -1;
  int handle = solar_os_doom_sound_mixer_play(&mixer, channel, sound_cache[id].data,
    sound_cache[id].length, sound_cache[id].rate, vol, sep, sound_pitch(pitch));
  solar_os_doom_app->synth_unlock();
  return handle;
}



void I_StopSound (int handle)
{
  if (!sound_initialized || !solar_os_doom_app->synth_lock()) return;
  solar_os_doom_sound_mixer_stop(&mixer, handle);
  solar_os_doom_app->synth_unlock();
}


int I_SoundIsPlaying(int handle)
{
  if (!sound_initialized || !solar_os_doom_app->synth_lock()) return 0;
  bool playing = solar_os_doom_sound_mixer_playing(&mixer, handle);
  solar_os_doom_app->synth_unlock();
  return playing;
}


int I_AnySoundStillPlaying(void)
{
  if (!sound_initialized || !solar_os_doom_app->synth_lock()) return 0;
  bool playing = false;
  for (unsigned i = 0; i < SOLAR_OS_DOOM_SOUND_CHANNELS; i++)
    if (mixer.voices[i].data) { playing = true; break; }
  solar_os_doom_app->synth_unlock();
  return playing;
}



void I_ShutdownSound(void)
{
  if (!sound_initialized) return;
  solar_os_doom_sound_suspend();
  sound_initialized = false;
  memset(&mixer, 0, sizeof(mixer));
  for (unsigned i = 0; i < NUMSFX; i++) {
    if (sound_cache[i].data) W_UnlockLumpNum(sound_cache[i].lump);
  }
  memset(sound_cache, 0, sizeof(sound_cache));
}

void solar_os_doom_sound_close(void) { I_ShutdownSound(); }

//static SDL_AudioSpec audio;

void I_InitSound(void)
{
  mus_card = 0;
  bool has_effects = false;
  for (int i = 0; i < numlumps; i++)
    if (!strncmp(lumpinfo[i].name, "DS", 2)) { has_effects = true; break; }
  if (!has_effects) {
    snd_card = 0;
    lprintf(LO_WARN, "doom: IWAD contains no sound effects\n");
    return;
  }
  if (nosfxparm || solar_os_doom_app->struct_size <
      offsetof(solar_os_native_foreground_api_v1_t, synth_close) + sizeof(solar_os_doom_app->synth_close) ||
      !solar_os_doom_app->synth_open || !solar_os_doom_app->synth_lock ||
      !solar_os_doom_app->synth_unlock || !solar_os_doom_app->synth_close) {
    snd_card = 0;
    return;
  }
  snd_card = 1;
  solar_os_doom_sound_mixer_init(&mixer, 0);
  sound_initialized = true;
  solar_os_doom_sound_resume();
}




void I_ShutdownMusic(void)
{
}

void I_InitMusic(void)
{
}

void I_PlaySong(int handle, int looping)
{
}

extern int mus_pause_opt; // From m_misc.c

void I_PauseSong (int handle)
{
}

void I_ResumeSong (int handle)
{
}

void I_StopSong(int handle)
{
}

void I_UnRegisterSong(int handle)
{
}

int I_RegisterSong(const void *data, size_t len)
{
  return (0);
}

int I_RegisterMusic( const char* filename, musicinfo_t *song )
{
    return 1;
}

void I_SetMusicVolume(int volume)
{
}


