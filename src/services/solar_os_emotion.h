#pragma once
#include "esp_err.h"
#include "solar_os_emotion_abi.h"
#include "solar_os_gfx.h"
#include "solar_os_native_emotion_abi.h"

typedef struct solar_os_emotion solar_os_emotion_t;

/* Each instance has an independent worker and an eight-command queue.
 * Foreground surfaces retain a display claim while running; suspension joins
 * their workers before releasing it. Other targets are exclusively claimed
 * until close. Calls on a live instance may enqueue concurrently;
 * callers must serialize close/suspend/resume with all other API operations.
 * Failed close retains the pointer, claims and library lease for retry.
 * Failed open may return a non-NULL instance if admission cleanup times out;
 * callers must retain it and retry close. */
esp_err_t solar_os_emotion_open(const solar_os_emotion_options_t *options,
    solar_os_gfx_t *foreground, solar_os_emotion_t **out, char *detail, size_t detail_size);
esp_err_t solar_os_emotion_close(solar_os_emotion_t **instance);
esp_err_t solar_os_emotion_suspend(solar_os_emotion_t *instance);
esp_err_t solar_os_emotion_resume(solar_os_emotion_t *instance);
esp_err_t solar_os_emotion_set(solar_os_emotion_t *instance, const char *pose, uint32_t transition_ms);
esp_err_t solar_os_emotion_react(solar_os_emotion_t *instance, const char *pose,
    uint32_t hold_ms, uint32_t transition_ms, const char *cue);
esp_err_t solar_os_emotion_look(solar_os_emotion_t *instance, int32_t x, int32_t y);
esp_err_t solar_os_emotion_blink(solar_os_emotion_t *instance);
void solar_os_emotion_status(solar_os_emotion_t *instance, solar_os_emotion_status_t *status);
bool solar_os_emotion_uses_gfx(const solar_os_emotion_t *instance, const solar_os_gfx_t *gfx);
