#include "solar_os_native_modem_provider.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_MODEM
#include "solar_os_memory.h"
#include "solar_os_modem.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"

typedef struct {
    char name[SOLAR_OS_MODEM_NAME_MAX];
    solar_os_native_handle_t lease;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    bool registered;
    void *user;
    solar_os_native_modem_operations_v1_t callbacks;
    solar_os_modem_ops_t ops;
} native_modem_t;

static esp_err_t modem_status(void *opaque, solar_os_modem_status_t *out)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.status(modem->user, out));
}

static esp_err_t modem_set_power(void *opaque, bool powered)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.set_power(modem->user, powered));
}

static esp_err_t modem_reset(void *opaque)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.reset(modem->user));
}

static esp_err_t modem_apply_profile(void *opaque, const solar_os_modem_profile_t *value)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.apply_profile(modem->user, value));
}

static esp_err_t modem_clear_profile(void *opaque)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.clear_profile(modem->user));
}

static esp_err_t modem_set_data_active(void *opaque, bool active)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.set_data_active(modem->user, active));
}

static esp_err_t modem_get_transport_rate(void *opaque, solar_os_modem_transport_rate_info_t *out)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.get_transport_rate(modem->user, out));
}

static esp_err_t modem_set_transport_rate(void *opaque, uint32_t rate)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.set_transport_rate(modem->user, rate));
}

static esp_err_t modem_unlock_sim(void *opaque, const char *pin)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.unlock_sim(modem->user, pin));
}

static esp_err_t modem_command(void *opaque, const char *text, uint32_t timeout, char *response, size_t capacity)
{
    native_modem_t *modem = opaque;
    return solar_os_native_callback_result(modem->callbacks.command(modem->user, text, timeout, response, capacity));
}

static int modem_destroy(void *opaque)
{
    native_modem_t *modem = opaque;
    if (modem->registered) {
        const esp_err_t error = solar_os_modem_unregister_owned(modem->name, modem);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        modem->registered = false;
    }
    if (modem->lease) {
        const int result = solar_os_native_library_release(modem->lease);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (modem->driver_pin) solar_os_native_driver_unpin(modem->driver_pin);
#endif
    solar_os_memory_free(modem);
    return SOLAR_OS_NATIVE_OK;
}

static int modem_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_MODEM_PROVIDER);
}

static int modem_register(const solar_os_native_modem_registration_v1_t *in,
    solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !*in->name || strnlen(in->name, SOLAR_OS_MODEM_NAME_MAX) >= SOLAR_OS_MODEM_NAME_MAX ||
        !in->operations.status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_modem_t *modem = solar_os_memory_calloc(1, sizeof(*modem),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.modem.provider");
    if (!modem) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strlcpy(modem->name, in->name, sizeof(modem->name));
    modem->user = in->user;
    modem->callbacks = in->operations;
    modem->ops = (solar_os_modem_ops_t){
        .get_status = modem_status,
        .set_power = in->operations.set_power ? modem_set_power : NULL,
        .reset = in->operations.reset ? modem_reset : NULL,
        .apply_profile = in->operations.apply_profile ? modem_apply_profile : NULL,
        .clear_profile = in->operations.clear_profile ? modem_clear_profile : NULL,
        .set_data_active = in->operations.set_data_active ? modem_set_data_active : NULL,
        .get_transport_rate = in->operations.get_transport_rate ? modem_get_transport_rate : NULL,
        .set_transport_rate = in->operations.set_transport_rate ? modem_set_transport_rate : NULL,
        .unlock_sim = in->operations.unlock_sim ? modem_unlock_sim : NULL,
        .command = in->operations.command ? modem_command : NULL,
    };
    solar_os_native_handle_t handle;
    int result = solar_os_native_handle_create_exclusive(SOLAR_OS_NATIVE_HANDLE_MODEM_PROVIDER,
        modem, modem_destroy, NULL, &handle);
    if (result) {
        solar_os_memory_free(modem);
        return result;
    }

    if (in->library_lease) {
        result = solar_os_native_library_retain(in->library_lease, &modem->lease);
    } else {
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        modem->driver_pin = solar_os_native_driver_pin();
        result = modem->driver_pin ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#else
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#endif
    }
    if (!result) {
        const solar_os_modem_registration_t registration = {
            .name = modem->name, .driver = in->driver, .transport = in->transport,
            .ops = &modem->ops, .ctx = modem, .powered = in->powered,
        };
        const esp_err_t error = solar_os_modem_register(&registration);
        result = solar_os_native_result(error);
        modem->registered = error == ESP_OK;
    }
    solar_os_native_handle_release_exclusive(handle);
    if (result && modem_close(handle) == SOLAR_OS_NATIVE_OK) return result;
    *out = handle;
    return result;
}

static const solar_os_native_modem_provider_api_v1_t modem_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_MODEM_PROVIDER_ABI, .struct_size = sizeof(modem_provider_api),
    .register_modem = modem_register, .close = modem_close,
};
#endif

const void *solar_os_native_modem_provider_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_modem_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
