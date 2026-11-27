#include "solar_os_native_port_provider.h"
#include "solar_os_config.h"
#include <stddef.h>
#include <string.h>

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include <stdatomic.h>
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_port_provider_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_port.h"

typedef struct {
    char name[SOLAR_OS_PORT_NAME_MAX];
    solar_os_native_port_provider_callbacks_v1_t callbacks;
} native_port_endpoint_t;

typedef struct {
    size_t count;
    solar_os_native_handle_t library;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    bool registered;
    atomic_bool opening;
    native_port_endpoint_t endpoints[];
} native_port_group_t;

_Static_assert(SOLAR_OS_NATIVE_PORT_READ == SOLAR_OS_PORT_CAP_READ &&
               SOLAR_OS_NATIVE_PORT_WRITE == SOLAR_OS_PORT_CAP_WRITE &&
               SOLAR_OS_NATIVE_PORT_CONFIG == SOLAR_OS_PORT_CAP_CONFIG &&
               SOLAR_OS_NATIVE_PORT_SERIAL == SOLAR_OS_PORT_CAP_SERIAL &&
               SOLAR_OS_NATIVE_PORT_USB_JTAG == SOLAR_OS_PORT_CAP_USB_JTAG,
               "native port capabilities");
_Static_assert(SOLAR_OS_NATIVE_SERIAL_ALL == SOLAR_OS_SERIAL_ALL &&
               SOLAR_OS_NATIVE_SERIAL_BAUD == SOLAR_OS_SERIAL_BAUD &&
               SOLAR_OS_NATIVE_SERIAL_BITS == SOLAR_OS_SERIAL_BITS &&
               SOLAR_OS_NATIVE_SERIAL_PARITY == SOLAR_OS_SERIAL_PARITY &&
               SOLAR_OS_NATIVE_SERIAL_STOP == SOLAR_OS_SERIAL_STOP &&
               SOLAR_OS_NATIVE_SERIAL_DTR == SOLAR_OS_SERIAL_DTR &&
               SOLAR_OS_NATIVE_SERIAL_RTS == SOLAR_OS_SERIAL_RTS,
               "native serial control flags");

static esp_err_t port_open(void *user)
{
    native_port_endpoint_t *endpoint = user;
    return solar_os_native_callback_result(endpoint->callbacks.open(endpoint->callbacks.user));
}

static esp_err_t port_close(void *user)
{
    native_port_endpoint_t *endpoint = user;
    return solar_os_native_callback_result(endpoint->callbacks.close(endpoint->callbacks.user));
}

static esp_err_t port_read(void *user, uint8_t *bytes, size_t capacity,
                           uint32_t timeout_ms, size_t *count)
{
    native_port_endpoint_t *endpoint = user;
    size_t actual = 0;
    const int result = endpoint->callbacks.read(endpoint->callbacks.user, bytes, capacity, timeout_ms, &actual);
    if (actual > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (count != NULL) {
        *count = actual;
    }
    return solar_os_native_callback_result(result);
}

static esp_err_t port_write(void *user, const uint8_t *bytes, size_t size, size_t *count)
{
    native_port_endpoint_t *endpoint = user;
    size_t actual = 0;
    const int result = endpoint->callbacks.write(endpoint->callbacks.user, bytes, size, &actual);
    if (actual > size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (count != NULL) {
        *count = actual;
    }
    return solar_os_native_callback_result(result);
}

static esp_err_t port_serial_get(const solar_os_native_port_provider_callbacks_v1_t *callbacks,
                                 solar_os_port_serial_config_t *config)
{
    if (!callbacks->serial_get) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_native_serial_config_v1_t value = {0};
    const int result = callbacks->serial_get(callbacks->user, &value);
    if (result != SOLAR_OS_NATIVE_OK) {
        return solar_os_native_callback_result(result);
    }
    if (value.data_bits > 8 || value.parity > 4 || value.stop_bits > 2 ||
        value.dtr > 1 || value.rts > 1) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = (solar_os_port_serial_config_t){
        value.baud_rate, value.data_bits, value.parity, value.stop_bits, value.dtr, value.rts,
    };
    return ESP_OK;
}

static esp_err_t port_control(void *user, solar_os_port_control_t request, void *data)
{
    const native_port_endpoint_t *endpoint = user;
    const solar_os_native_port_provider_callbacks_v1_t *callbacks = &endpoint->callbacks;
    switch (request) {
    case SOLAR_OS_PORT_SERIAL_GET:
        return port_serial_get(callbacks, data);
    case SOLAR_OS_PORT_SERIAL_UPDATE: {
        if (!callbacks->serial_set) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        const solar_os_port_serial_update_t *update = data;
        const solar_os_port_serial_config_t *config = &update->config;
        const solar_os_native_serial_config_v1_t value = {
            config->baud_rate, config->data_bits, config->parity, config->stop_bits, config->dtr, config->rts,
        };
        return solar_os_native_callback_result(callbacks->serial_set(callbacks->user, &value, update->mask));
    }
    case SOLAR_OS_PORT_SERIAL_BREAK:
        return callbacks->serial_break ?
            solar_os_native_callback_result(callbacks->serial_break(callbacks->user, *(uint16_t *)data)) :
            ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool endpoint_prepare(native_port_endpoint_t *endpoint,
                             const solar_os_native_port_provider_endpoint_v1_t *input,
                             solar_os_port_driver_t *driver)
{
    const solar_os_native_port_provider_callbacks_v1_t *callbacks = &input->callbacks;
    if (!input->name || !input->name[0] || strnlen(input->name, sizeof(endpoint->name)) >= sizeof(endpoint->name) ||
        (input->capabilities & SOLAR_OS_NATIVE_PORT_READ && !callbacks->read) ||
        (input->capabilities & SOLAR_OS_NATIVE_PORT_WRITE && !callbacks->write)) {
        return false;
    }
    strlcpy(endpoint->name, input->name, sizeof(endpoint->name));
    endpoint->callbacks = *callbacks;
    *driver = (solar_os_port_driver_t){
        .name = endpoint->name, .label = input->label, .capabilities = input->capabilities,
        .open = callbacks->open ? port_open : NULL, .close = callbacks->close ? port_close : NULL,
        .read = callbacks->read ? port_read : NULL, .write = callbacks->write ? port_write : NULL,
        .control = port_control, .user = endpoint,
    };
    return true;
}

static int group_destroy(void *opaque)
{
    native_port_group_t *group = opaque;
    if (atomic_load(&group->opening)) {
        return SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    if (group->registered) {
        const char *names[SOLAR_OS_PORT_MAX];
        const void *owners[SOLAR_OS_PORT_MAX];
        for (size_t i = 0; i < group->count; i++) {
            names[i] = group->endpoints[i].name;
            owners[i] = &group->endpoints[i];
        }
        const esp_err_t error = solar_os_port_unregister_many_owned(names, owners, group->count);
        if (error != ESP_OK) {
            return error == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
        }
        group->registered = false;
    }
    if (group->library) {
        const int result = solar_os_native_library_release(group->library);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) {
            return result;
        }
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (group->driver_pin) {
        solar_os_native_driver_unpin(group->driver_pin);
    }
#endif
    solar_os_memory_free(group);
    return SOLAR_OS_NATIVE_OK;
}

static int group_pin(native_port_group_t *group, solar_os_native_handle_t library)
{
    if (library) {
        return solar_os_native_library_retain(library, &group->library);
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    group->driver_pin = solar_os_native_driver_pin();
    if (group->driver_pin) {
        return SOLAR_OS_NATIVE_OK;
    }
#endif
    return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}

static int group_register(solar_os_native_handle_t library,
                          const solar_os_native_port_provider_endpoint_v1_t *endpoints,
                          size_t count, solar_os_native_handle_t *out)
{
    if (!out) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *out = 0;
    if (!endpoints || count == 0 || count > SOLAR_OS_PORT_MAX) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    native_port_group_t *group = solar_os_memory_calloc(1, sizeof(*group) + count * sizeof(group->endpoints[0]),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.ports.provider");
    if (!group) {
        return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    solar_os_port_driver_t drivers[SOLAR_OS_PORT_MAX];
    for (size_t i = 0; i < count; i++) {
        if (!endpoint_prepare(&group->endpoints[i], &endpoints[i], &drivers[i])) {
            solar_os_memory_free(group);
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
    }
    group->count = count;
    atomic_init(&group->opening, true);
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_PORT_PROVIDER;
    int result = solar_os_native_handle_create(kind, group, group_destroy, out);
    if (result != SOLAR_OS_NATIVE_OK) {
        solar_os_memory_free(group);
        return result;
    }
    result = group_pin(group, library);
    if (result == SOLAR_OS_NATIVE_OK) {
        result = solar_os_native_result(solar_os_port_register_many(drivers, count));
    }
    group->registered = result == SOLAR_OS_NATIVE_OK;
    atomic_store(&group->opening, false);
    if (result != SOLAR_OS_NATIVE_OK &&
        solar_os_native_handle_close_kind(*out, kind) == SOLAR_OS_NATIVE_OK) {
        *out = 0;
    }
    return result;
}

static int group_close(solar_os_native_handle_t registration)
{
    return solar_os_native_handle_close_kind(registration, SOLAR_OS_NATIVE_HANDLE_PORT_PROVIDER);
}

static const solar_os_native_port_provider_api_v1_t port_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_PORT_PROVIDER_ABI,
    .struct_size = sizeof(port_provider_api),
    .register_many = group_register,
    .close = group_close,
};
#endif

const void *solar_os_native_port_provider_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_port_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
