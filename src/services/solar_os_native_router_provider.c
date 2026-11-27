#include "solar_os_native_router_provider.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_NETWORK
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"
#include "solar_os_network.h"

typedef struct {
    char name[SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U];
    solar_os_native_router_provider_v1_t callbacks;
    solar_os_native_handle_t lease;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    bool registered;
} native_router_t;

static esp_err_t router_start(void *opaque)
{
    native_router_t *router = opaque;
    return solar_os_native_callback_result(router->callbacks.start(router->callbacks.user));
}

static esp_err_t router_stop(void *opaque)
{
    native_router_t *router = opaque;
    return solar_os_native_callback_result(router->callbacks.stop(router->callbacks.user));
}

static void router_status(void *opaque, solar_os_network_router_status_t *out)
{
    native_router_t *router = opaque;
    solar_os_native_network_router_v1_t status = {0};
    router->callbacks.status(router->callbacks.user, &status);
    out->downstream_active = status.downstream_active != 0;
    out->enabled = status.enabled != 0;
    out->active = status.active != 0;
    out->nat_enabled = status.nat_enabled != 0;
    out->last_error = solar_os_native_callback_result(status.error);
    out->client_count = status.client_count > UINT16_MAX ? UINT16_MAX : (uint16_t)status.client_count;
    out->client_limit = status.client_limit > UINT16_MAX ? UINT16_MAX : (uint16_t)status.client_limit;
#define COPY(field) do { \
    _Static_assert(sizeof(out->field) == sizeof(status.field), "router field capacity"); \
    memcpy(out->field, status.field, sizeof(out->field)); \
    out->field[sizeof(out->field) - 1U] = '\0'; \
} while (0)
    COPY(downstream);
    COPY(address);
    COPY(label);
#undef COPY
}

static int router_destroy(void *opaque)
{
    native_router_t *router = opaque;
    if (router->registered) {
        const esp_err_t error = solar_os_network_router_unregister_owned(router->name, router);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        router->registered = false;
    }
    if (router->lease) {
        const int result = solar_os_native_library_release(router->lease);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (router->driver_pin) solar_os_native_driver_unpin(router->driver_pin);
#endif
    solar_os_memory_free(router);
    return SOLAR_OS_NATIVE_OK;
}

static int router_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_ROUTER_PROVIDER);
}

static int router_select(solar_os_native_handle_t handle)
{
    void *opaque;
    const int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_ROUTER_PROVIDER, &opaque);
    if (result) return result;
    const native_router_t *router = opaque;
    const esp_err_t error = solar_os_network_router_select(router->name, router);
    solar_os_native_handle_release_exclusive(handle);
    return error == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
}

static int router_register(const solar_os_native_router_provider_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->name[0] || !in->start || !in->stop || !in->status ||
        strnlen(in->name, SOLAR_OS_NETWORK_PATH_NAME_MAX + 1U) > SOLAR_OS_NETWORK_PATH_NAME_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_router_t *router = solar_os_memory_calloc(1, sizeof(*router),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.routing.router");
    if (!router) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    router->callbacks = *in;
    strlcpy(router->name, in->name, sizeof(router->name));
    router->callbacks.name = router->name;
    solar_os_native_handle_t handle;
    int result = solar_os_native_handle_create_exclusive(SOLAR_OS_NATIVE_HANDLE_ROUTER_PROVIDER,
        router, router_destroy, NULL, &handle);
    if (result) {
        solar_os_memory_free(router);
        return result;
    }
    if (in->library_lease) {
        result = solar_os_native_library_retain(in->library_lease, &router->lease);
    } else {
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        router->driver_pin = solar_os_native_driver_pin();
        result = router->driver_pin ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#else
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#endif
    }
    if (!result) {
        const solar_os_network_router_provider_t provider = {
            .name = router->name, .context = router,
            .start = router_start, .stop = router_stop, .get_status = router_status,
        };
        const esp_err_t error = solar_os_network_router_register(&provider);
        result = solar_os_native_result(error);
        router->registered = result == SOLAR_OS_NATIVE_OK;
    }
    solar_os_native_handle_release_exclusive(handle);
    if (result && router_close(handle) == SOLAR_OS_NATIVE_OK) return result;
    *out = handle;
    return result;
}

static const solar_os_native_router_provider_api_v1_t router_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_ROUTER_PROVIDER_ABI,
    .struct_size = sizeof(router_provider_api),
    .register_router = router_register, .select = router_select, .close = router_close,
};
#endif

const void *solar_os_native_router_provider_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_router_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
