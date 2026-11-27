#include "solar_os_native_power_provider.h"
#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"
#include "solar_os_power.h"

#include <stdatomic.h>
#include <string.h>

typedef struct {
    int (*off)(void *user);
    void *user;
    uint32_t lease;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    atomic_bool opening;
    bool registered;
} off_provider_t;

static esp_err_t provider_off(void *opaque)
{
    off_provider_t *provider = opaque;
    return solar_os_native_callback_result(provider->off(provider->user));
}

static int provider_destroy(void *opaque)
{
    off_provider_t *provider = opaque;
    if (atomic_load(&provider->opening)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (provider->registered) {
        const esp_err_t error = solar_os_power_unregister_off_provider(provider);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        provider->registered = false;
    }
    if (provider->lease) {
        const int result = solar_os_native_library_release(provider->lease);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (provider->driver_pin) solar_os_native_driver_unpin(provider->driver_pin);
#endif
    solar_os_memory_free(provider);
    return SOLAR_OS_NATIVE_OK;
}

static int provider_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_POWER_OFF_PROVIDER);
}

static int provider_register(const solar_os_native_power_off_provider_v1_t *in,
                              solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !in->off) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    off_provider_t *provider = solar_os_memory_calloc(1, sizeof(*provider),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.power.off-provider");
    if (!provider) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    provider->off = in->off;
    provider->user = in->user;
    atomic_init(&provider->opening, true);
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_POWER_OFF_PROVIDER,
        provider, provider_destroy, out);
    if (result) {
        solar_os_memory_free(provider);
        return result;
    }

    if (in->library_lease) {
        result = solar_os_native_library_retain(in->library_lease, &provider->lease);
    } else {
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        provider->driver_pin = solar_os_native_driver_pin();
        result = provider->driver_pin ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#else
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#endif
    }
    if (!result) {
        result = solar_os_native_result(solar_os_power_register_off_provider(in->name, provider_off, provider));
        provider->registered = result == SOLAR_OS_NATIVE_OK;
    }
    atomic_store(&provider->opening, false);
    if (result && !provider_close(*out)) *out = 0;
    return result;
}

static const solar_os_native_power_provider_api_v1_t power_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_POWER_PROVIDER_ABI,
    .struct_size = sizeof(power_provider_api),
    .register_off = provider_register,
    .close = provider_close,
};
#endif

const void *solar_os_native_power_provider_get_service(const char *name, uint32_t abi,
                                                        uint32_t minimum)
{
#include "generated/native_power_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
