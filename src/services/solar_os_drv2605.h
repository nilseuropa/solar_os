#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

#define SOLAR_OS_DRV2605_ADDRESS 0x5AU

/* Maximum waveform slots in a sequence (DRV2605 supports up to 8). */
#define SOLAR_OS_DRV2605_WAVEFORM_MAX 8U

esp_err_t solar_os_drv2605_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count);
esp_err_t solar_os_drv2605_detach(const char *name);

/* Play one of the 123 built-in library effects (1–123). */
esp_err_t solar_os_drv2605_play_effect(uint8_t effect_id);

/* Play a waveform sequence (up to SOLAR_OS_DRV2605_WAVEFORM_MAX effect IDs,
 * terminated by 0). Played back-to-back without gaps. */
esp_err_t solar_os_drv2605_play_sequence(const uint8_t *effects, size_t count);

esp_err_t solar_os_drv2605_stop(void);

bool solar_os_drv2605_is_active(void);
