#include "solar_os_gameboy_audio.h"

#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_SYNTH

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_synth.h"

#define AUDIO_SAMPLE_RATE 16000
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#include "vendor/peanut_gb/minigb_apu.h"

#define GAMEBOY_AUDIO_OWNER "gameboy"
#define GAMEBOY_AUDIO_GAIN 4

static const char *TAG = "solar_os_gameboy_audio";
typedef struct {
  struct minigb_apu_ctx apu;
  SemaphoreHandle_t mutex;
  StaticSemaphore_t mutex_storage;
  bool initialized;
  bool running;
  uint32_t rendered_blocks;
  uint16_t peak_sample;
} gameboy_audio_state_t;

static gameboy_audio_state_t *gameboy_audio_state;
#define gameboy_apu (gameboy_audio_state->apu)
#define gameboy_apu_mutex (gameboy_audio_state->mutex)
#define gameboy_apu_mutex_storage (gameboy_audio_state->mutex_storage)
#define gameboy_apu_initialized (gameboy_audio_state->initialized)
#define gameboy_apu_running (gameboy_audio_state->running)

static esp_err_t gameboy_audio_ensure_mutex(void) {
  if (gameboy_audio_state == NULL) {
    gameboy_audio_state = solar_os_memory_calloc(
        1U, sizeof(*gameboy_audio_state), SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
        "gameboy.audio");
    if (gameboy_audio_state == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }
  if (gameboy_apu_mutex == NULL) {
    gameboy_apu_mutex =
        xSemaphoreCreateMutexStatic(&gameboy_apu_mutex_storage);
  }
  if (gameboy_apu_mutex == NULL) {
    solar_os_memory_free(gameboy_audio_state);
    gameboy_audio_state = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static void gameboy_audio_lock(void) {
  (void)xSemaphoreTake(gameboy_apu_mutex, portMAX_DELAY);
}

static void gameboy_audio_unlock(void) {
  (void)xSemaphoreGive(gameboy_apu_mutex);
}

static void gameboy_audio_render(int16_t *samples, size_t frames,
                                 uint32_t sample_rate, void *user) {
  (void)user;
  if (gameboy_audio_state == NULL || samples == NULL || frames != AUDIO_SAMPLES ||
      sample_rate != AUDIO_SAMPLE_RATE || !gameboy_apu_initialized) {
    return;
  }
  gameboy_audio_lock();
  minigb_apu_audio_callback(&gameboy_apu, samples);
  uint16_t peak = 0U;
  for (size_t i = 0; i < frames * 2U; i++) {
    int32_t amplified = (int32_t)samples[i] * GAMEBOY_AUDIO_GAIN;
    if (amplified > INT16_MAX) {
      amplified = INT16_MAX;
    } else if (amplified < INT16_MIN) {
      amplified = INT16_MIN;
    }
    samples[i] = (int16_t)amplified;
    const uint16_t magnitude = (uint16_t)(
        amplified < 0 ? (uint32_t)(-amplified) : (uint32_t)amplified);
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  gameboy_audio_state->rendered_blocks++;
  if (peak > gameboy_audio_state->peak_sample) {
    gameboy_audio_state->peak_sample = peak;
  }
  gameboy_audio_unlock();
}

esp_err_t solar_os_gameboy_audio_resume(void) {
  if (gameboy_audio_state == NULL || !gameboy_apu_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (gameboy_apu_running) {
    solar_os_synth_status_t status;
    solar_os_synth_get_status(&status);
    if (status.running && strcmp(status.owner, GAMEBOY_AUDIO_OWNER) == 0) {
      return ESP_OK;
    }
    gameboy_apu_running = false;
  }
  const solar_os_synth_config_t config = {
      .owner = GAMEBOY_AUDIO_OWNER,
      .render = gameboy_audio_render,
      .user = NULL,
      .block_frames = AUDIO_SAMPLES,
  };
  const esp_err_t err = solar_os_synth_start(&config);
  if (err == ESP_OK) {
    gameboy_apu_running = true;
  }
  return err;
}

esp_err_t solar_os_gameboy_audio_init(void) {
  if (gameboy_audio_state != NULL) {
    solar_os_gameboy_audio_deinit();
    if (gameboy_audio_state != NULL) {
      return ESP_ERR_INVALID_STATE;
    }
  }
  esp_err_t err = gameboy_audio_ensure_mutex();
  if (err != ESP_OK) {
    return err;
  }
  gameboy_audio_lock();
  minigb_apu_audio_init(&gameboy_apu);
  gameboy_apu_initialized = true;
  gameboy_audio_unlock();
  err = solar_os_gameboy_audio_resume();
  if (err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "output unavailable: %s", esp_err_to_name(err));
  }
  return err;
}

void solar_os_gameboy_audio_suspend(void) {
  if (gameboy_audio_state == NULL || !gameboy_apu_running) {
    return;
  }
  const esp_err_t err = solar_os_synth_stop(GAMEBOY_AUDIO_OWNER);
  if (err != ESP_OK) {
    SOLAR_OS_LOGW(TAG, "stop failed: %s", esp_err_to_name(err));
  } else {
    gameboy_apu_running = false;
  }
}

void solar_os_gameboy_audio_reset(void) {
  if (gameboy_audio_state == NULL || gameboy_apu_mutex == NULL) {
    return;
  }
  gameboy_audio_lock();
  minigb_apu_audio_init(&gameboy_apu);
  gameboy_audio_unlock();
}

void solar_os_gameboy_audio_deinit(void) {
  if (gameboy_audio_state == NULL) {
    return;
  }
  solar_os_gameboy_audio_suspend();
  if (!gameboy_apu_running && gameboy_apu_mutex != NULL) {
    gameboy_audio_lock();
    gameboy_apu_initialized = false;
    gameboy_audio_unlock();
    vSemaphoreDelete(gameboy_apu_mutex);
    solar_os_memory_free(gameboy_audio_state);
    gameboy_audio_state = NULL;
  }
}

void solar_os_gameboy_audio_take_stats(
    solar_os_gameboy_audio_stats_t *stats) {
  if (stats == NULL) {
    return;
  }
  memset(stats, 0, sizeof(*stats));
  if (gameboy_audio_state == NULL || gameboy_apu_mutex == NULL) {
    return;
  }
  gameboy_audio_lock();
  stats->rendered_blocks = gameboy_audio_state->rendered_blocks;
  stats->peak_sample = gameboy_audio_state->peak_sample;
  stats->running = gameboy_apu_running;
  gameboy_audio_state->rendered_blocks = 0U;
  gameboy_audio_state->peak_sample = 0U;
  gameboy_audio_unlock();
}

uint8_t solar_os_gameboy_audio_read(uint16_t address) {
  if (gameboy_audio_state == NULL || gameboy_apu_mutex == NULL ||
      !gameboy_apu_initialized ||
      address < 0xFF10U || address > 0xFF3FU) {
    return 0xFFU;
  }
  gameboy_audio_lock();
  const uint8_t value = minigb_apu_audio_read(&gameboy_apu, address);
  gameboy_audio_unlock();
  return value;
}

void solar_os_gameboy_audio_write(uint16_t address, uint8_t value) {
  if (gameboy_audio_state == NULL || gameboy_apu_mutex == NULL ||
      !gameboy_apu_initialized ||
      address < 0xFF10U || address > 0xFF3FU) {
    return;
  }
  gameboy_audio_lock();
  minigb_apu_audio_write(&gameboy_apu, address, value);
  gameboy_audio_unlock();
}

#else

esp_err_t solar_os_gameboy_audio_init(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_gameboy_audio_resume(void) {
  return ESP_ERR_NOT_SUPPORTED;
}

void solar_os_gameboy_audio_suspend(void) {}

void solar_os_gameboy_audio_reset(void) {}

void solar_os_gameboy_audio_deinit(void) {}

void solar_os_gameboy_audio_take_stats(
    solar_os_gameboy_audio_stats_t *stats) {
  if (stats != NULL) {
    *stats = (solar_os_gameboy_audio_stats_t){0};
  }
}

uint8_t solar_os_gameboy_audio_read(uint16_t address) {
  (void)address;
  return 0xFFU;
}

void solar_os_gameboy_audio_write(uint16_t address, uint8_t value) {
  (void)address;
  (void)value;
}

#endif
