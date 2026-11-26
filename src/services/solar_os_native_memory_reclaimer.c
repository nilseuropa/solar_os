#include "solar_os_native_memory_reclaimer.h"

#include <stdatomic.h>
#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_memory_reclaimer_abi.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "esp_heap_caps.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"

typedef struct {
    solar_os_memory_reclaimer_t entry;
    solar_os_native_memory_reclaim_fn callback;
    void *user;
    solar_os_native_handle_t library;
    atomic_bool opening;
    bool registered;
    char owner[SOLAR_OS_MEMORY_TAG_MAX];
} native_memory_cache_t;

static uint32_t native_memory_requirements(uint32_t capabilities)
{
    uint32_t requirements = 0;
    if (capabilities & MALLOC_CAP_INTERNAL) requirements |= SOLAR_OS_NATIVE_MEMORY_REQUIRE_INTERNAL;
    if (capabilities & MALLOC_CAP_SPIRAM) requirements |= SOLAR_OS_NATIVE_MEMORY_REQUIRE_EXTERNAL;
    if (capabilities & MALLOC_CAP_DMA) requirements |= SOLAR_OS_NATIVE_MEMORY_REQUIRE_DMA;
    if (capabilities & MALLOC_CAP_8BIT) requirements |= SOLAR_OS_NATIVE_MEMORY_REQUIRE_BYTE_ACCESS;
#ifdef MALLOC_CAP_SIMD
    if (capabilities & MALLOC_CAP_SIMD) requirements |= SOLAR_OS_NATIVE_MEMORY_REQUIRE_VECTOR_ACCESS;
#endif
    return requirements;
}

static size_t native_memory_reclaim_cache(const solar_os_memory_pressure_request_t *request,
                                         void *opaque)
{
    const native_memory_cache_t *state = opaque;
    if (!request) return state->callback(state->user, NULL);
    const solar_os_native_memory_reclaim_request_v1_t view = {
        .size = request->size,
        .alignment = request->alignment,
        .requirements = native_memory_requirements(request->capabilities),
        .memory_class = request->memory_class,
        .tag = request->tag,
    };
    return state->callback(state->user, &view);
}

static int native_memory_close_cache(void *opaque)
{
    native_memory_cache_t *state = opaque;
    if (atomic_load(&state->opening)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (state->registered) {
        if (!solar_os_memory_unregister_reclaimer(&state->entry)) return SOLAR_OS_NATIVE_ERROR_BUSY;
        state->registered = false;
    }
    if (state->library) {
        const int result = solar_os_native_library_release(state->library);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int native_memory_register_cache(solar_os_native_handle_t lease, const char *owner,
                                        uint8_t priority, solar_os_native_memory_reclaim_fn callback,
                                        void *user, solar_os_native_handle_t *registration)
{
    if (!registration) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *registration = 0;
    if (!lease || !owner || !*owner || !callback) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t length = 0;
    while (length < SOLAR_OS_MEMORY_TAG_MAX && owner[length]) length++;
    if (length == SOLAR_OS_MEMORY_TAG_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_memory_cache_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.memory.cache");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    memcpy(state->owner, owner, length + 1U);
    state->callback = callback;
    state->user = user;
    state->entry = (solar_os_memory_reclaimer_t){
        .owner = state->owner, .reclaim = native_memory_reclaim_cache, .user = state, .priority = priority,
    };
    atomic_init(&state->opening, true);

    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_MEMORY_RECLAIMER;
    int result = solar_os_native_handle_create(kind, state, native_memory_close_cache, registration);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    result = solar_os_native_library_retain(lease, &state->library);
    if (!result) {
        state->registered = solar_os_memory_register_reclaimer(&state->entry);
        if (!state->registered) result = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    atomic_store(&state->opening, false);
    if (result) {
        const int close = solar_os_native_handle_close_kind(*registration, kind);
        if (!close || close == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) *registration = 0;
    }
    return result;
}

static int native_memory_reclaimer_close(solar_os_native_handle_t registration)
{
    return solar_os_native_handle_close_kind(registration, SOLAR_OS_NATIVE_HANDLE_MEMORY_RECLAIMER);
}

static const solar_os_native_memory_reclaimer_api_v1_t memory_reclaimer_api = {
    .abi_version = SOLAR_OS_NATIVE_MEMORY_RECLAIMER_ABI,
    .struct_size = sizeof(memory_reclaimer_api),
    .register_cache = native_memory_register_cache,
    .close = native_memory_reclaimer_close,
};
#endif

const void *solar_os_native_memory_reclaimer_get_service(const char *name, uint32_t abi,
                                                        uint32_t minimum)
{
#include "generated/native_memory_reclaimer_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
