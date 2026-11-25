#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_input_keymap_types_v1.h"

#define SOLAR_OS_NATIVE_KEYMAP_SERVICE "input.keymap"
#define SOLAR_OS_NATIVE_KEYMAP_ABI 1U

/* Callbacks serialize profile changes with the producer's hardware worker.
 * lock must reject a stopped producer; prepare drains pending hardware events.
 * They return native error codes and run in the requesting task without an
 * application scope. The service copies defaults/descriptor before returning
 * and retains independent code and producer references until close succeeds.
 * Keep user data alive until then. Publish with activate only after producer
 * initialization succeeds. Stop the producer before close, outside its lock. */
typedef struct {
    uint32_t struct_size, library_lease;
    solar_os_native_handle_t producer;
    const solar_os_input_keymap_t *defaults;
    int (*lock)(void *user);
    void (*unlock)(void *user);
    int (*prepare)(void *user);
    void *user;
} solar_os_native_keymap_provider_v1_t;

/* Value helpers retain no pointers. Validate a map once before decoding into
 * zero-initialized, caller-owned state; serialize access to each state. Large
 * profile records should use caller-owned heap storage on small task stacks.
 * get copies a snapshot; set/parse consume their input before returning. Parse
 * preserves output on failure and supports base == output. Profile changes
 * preserve physical topology and use the existing core prepare/release rules.
 * Provider process/priority/release_all require the producer's hardware lock.
 * Apply snapshot releases before presses, ordered by press_priority. Close
 * retires immediately and returns BUSY until active operations finish. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*info)(const char *name, solar_os_input_keymap_info_t *info);
    int (*get)(const char *name, bool defaults, solar_os_input_keymap_t *map);
    int (*set)(const char *name, const solar_os_input_keymap_t *map);
    int (*reset)(const char *name);
    int (*load)(const char *name, const char *path);
    unsigned (*find_slot)(const solar_os_input_keymap_t *map, uint16_t physical);
    int (*validate)(const solar_os_input_keymap_t *map);
    void (*release)(solar_os_input_keymap_state_t *state);
    bool (*decode)(solar_os_input_keymap_state_t *state, const solar_os_input_keymap_t *map,
                   uint16_t physical, bool pressed, solar_os_input_keymap_transition_t *transition);
    int (*parse)(const char *json, const solar_os_input_keymap_t *base, solar_os_input_keymap_t *out);
    int (*provider_open)(const solar_os_native_keymap_provider_v1_t *descriptor, solar_os_native_handle_t *profile);
    int (*provider_activate)(solar_os_native_handle_t profile);
    int (*provider_process)(solar_os_native_handle_t profile, uint16_t physical, bool pressed, bool *published);
    int (*provider_press_priority)(solar_os_native_handle_t profile, uint16_t physical, unsigned *priority);
    int (*provider_release_all)(solar_os_native_handle_t profile);
    int (*provider_close)(solar_os_native_handle_t profile);
} solar_os_native_keymap_api_v1_t;
