#include "solar_os_git_state.h"

#include <stdbool.h>
#include <string.h>

#ifdef ESP_PLATFORM
extern const solar_os_git_state_initializer_t _solar_git_initializers_start[];
extern const solar_os_git_state_initializer_t _solar_git_initializers_end[];
#define STATE_START _solar_git_initializers_start
#define STATE_END _solar_git_initializers_end
#else
extern const solar_os_git_state_initializer_t __start_solaros_git_initializers[];
extern const solar_os_git_state_initializer_t __stop_solaros_git_initializers[];
#define STATE_START __start_solaros_git_initializers
#define STATE_END __stop_solaros_git_initializers
#endif

void solar_os_git_state_init(void)
{
    static SOLAR_OS_GIT_STATE_BSS bool initialized;
    if (initialized) return;
    for (const solar_os_git_state_initializer_t *entry = STATE_START;
         entry < STATE_END; entry++)
        memcpy(entry->destination, entry->initial_value, entry->size);
    initialized = true;
}
