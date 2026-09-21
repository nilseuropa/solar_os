#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_input.h"

#define SOLAR_OS_INPUT_ACTION_MAX_BINDINGS 16U
#define SOLAR_OS_INPUT_ACTION_COMMAND_MAX 192U
#define SOLAR_OS_INPUT_ACTION_COOLDOWN_DEFAULT_MS 250U
#define SOLAR_OS_INPUT_ACTION_COOLDOWN_MAX_MS 3600000U
#define SOLAR_OS_INPUT_ACTION_WORKER_STACK 6144U

typedef esp_err_t (*solar_os_input_action_runner_t)(const char *command);

typedef struct {
    uint32_t id;
    bool any_source;
    char source[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
    solar_os_input_gesture_t gesture;
    bool any_direction;
    solar_os_input_gesture_direction_t direction;
    uint32_t cooldown_ms;
    uint32_t trigger_count;
    uint32_t dropped_count;
    char command[SOLAR_OS_INPUT_ACTION_COMMAND_MAX];
} solar_os_input_action_binding_t;

esp_err_t solar_os_input_actions_init(void);
void solar_os_input_actions_set_runner(solar_os_input_action_runner_t runner);
esp_err_t solar_os_input_actions_start(void);
void solar_os_input_actions_stop(void);
bool solar_os_input_actions_running(void);
bool solar_os_input_actions_worker_active(void);

esp_err_t solar_os_input_actions_bind(
    const char *source,
    solar_os_input_gesture_t gesture,
    bool any_direction,
    solar_os_input_gesture_direction_t direction,
    uint32_t cooldown_ms,
    const char *command,
    uint32_t *binding_id);
size_t solar_os_input_actions_count(void);
bool solar_os_input_actions_get(size_t index,
                                solar_os_input_action_binding_t *binding);
esp_err_t solar_os_input_actions_unbind(uint32_t binding_id);
size_t solar_os_input_actions_clear(void);

/* Emit a local key tap through a service-owned virtual keyboard source. */
esp_err_t solar_os_input_actions_emit_key(const char *name);
