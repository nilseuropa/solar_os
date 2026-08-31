#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
  uint32_t rendered_blocks;
  uint16_t peak_sample;
  bool running;
} solar_os_gameboy_audio_stats_t;

esp_err_t solar_os_gameboy_audio_init(void);
esp_err_t solar_os_gameboy_audio_resume(void);
void solar_os_gameboy_audio_suspend(void);
void solar_os_gameboy_audio_reset(void);
void solar_os_gameboy_audio_deinit(void);
void solar_os_gameboy_audio_take_stats(
    solar_os_gameboy_audio_stats_t *stats);

uint8_t solar_os_gameboy_audio_read(uint16_t address);
void solar_os_gameboy_audio_write(uint16_t address, uint8_t value);
