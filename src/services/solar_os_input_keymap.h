#pragma once

#include "solar_os_input_keymap_mapper.h"

#define SOLAR_OS_INPUT_KEYMAP_PHYSICAL (1U << 0)
#define SOLAR_OS_INPUT_KEYMAP_MODIFIERS (1U << 1)
#define SOLAR_OS_INPUT_KEYMAP_LAYERS (1U << 2)
#define SOLAR_OS_INPUT_KEYMAP_TAP_HOLD (1U << 3)
#define SOLAR_OS_INPUT_KEYMAP_MATRIX (1U << 4)

typedef uint32_t solar_os_input_keymap_handle_t;
#define SOLAR_OS_INPUT_KEYMAP_INVALID 0U

typedef struct {
    solar_os_input_source_t source;
    uint32_t capabilities;
    uint16_t key_count, first, stride;
    uint8_t rows, cols;
} solar_os_input_keymap_info_t;

/* Hardware callbacks serialize mapping with the producer. lock must reject a
 * stopped device; prepare drains pending hardware events before a map change.
 * The registry never holds its own lock while calling these functions. */
typedef struct {
    esp_err_t (*lock)(void *context);
    void (*unlock)(void *context);
    esp_err_t (*prepare)(void *context);
    void *context;
} solar_os_input_keymap_ops_t;

esp_err_t solar_os_input_keymap_register(solar_os_input_source_t source,
    const solar_os_input_keymap_t *defaults, const solar_os_input_keymap_ops_t *ops,
    solar_os_input_keymap_handle_t *handle);
/* Publish only after producer initialization succeeds. Before activation,
 * no external operation can acquire the newly registered profile. */
esp_err_t solar_os_input_keymap_activate(solar_os_input_keymap_handle_t handle);
/* Stop the producer first; unregister before closing its input source.
 * Call unregister outside the hardware lock. A
 * timeout retains the profile until outstanding operations finish; retry it. */
esp_err_t solar_os_input_keymap_unregister(solar_os_input_keymap_handle_t handle);
/* Producer calls require its hardware lock. Repeat and event queues remain
 * owned by the common input service. */
esp_err_t solar_os_input_keymap_process(solar_os_input_keymap_handle_t handle,
    uint16_t physical, bool pressed, bool *published);
void solar_os_input_keymap_release_all(solar_os_input_keymap_handle_t handle);

/* Existing sources without a registered profile return capabilities=0. */
esp_err_t solar_os_input_keymap_info(const char *name, solar_os_input_keymap_info_t *info);
esp_err_t solar_os_input_keymap_get(const char *name, bool defaults, solar_os_input_keymap_t *map);
esp_err_t solar_os_input_keymap_set(const char *name, const solar_os_input_keymap_t *map);
esp_err_t solar_os_input_keymap_reset(const char *name);
esp_err_t solar_os_input_keymap_load(const char *name, const char *path);
