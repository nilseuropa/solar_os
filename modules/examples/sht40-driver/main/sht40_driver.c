#include <stdatomic.h>

#include "solar_os_native_client.h"
#include "solar_os_native_bound_driver_abi.h"
#include "sht40_protocol.h"

static const solar_os_native_bus_api_v1_t *bus;
static const solar_os_native_memory_api_v1_t *memory;
static const solar_os_native_sensors_api_v1_t *sensors;
static const solar_os_native_time_api_v1_t *clock;

typedef struct {
    solar_os_native_handle_t bus_lease, provider;
    atomic_uint measuring;
} sht40_device_t;

static int read_environment(void *user, solar_os_native_environment_v1_t *sample)
{
    sht40_device_t *device = user;
    if (atomic_exchange(&device->measuring, 1U)) return SOLAR_OS_NATIVE_ERROR_BUSY;
    const uint8_t command = SHT40_CMD_MEASURE_HIGH;
    uint8_t response[6];
    int result = bus->i2c_write(device->bus_lease, SHT40_SENSOR_ADDRESS, &command, sizeof(command));
    if (!result) {
        clock->sleep_ms(SHT40_MEASURE_DELAY_MS);
        result = bus->i2c_read(device->bus_lease, SHT40_SENSOR_ADDRESS, response, sizeof(response));
    }
    if (!result && !sht40_decode_sample(response, &sample->temperature_c, &sample->humidity_percent)) {
        result = SOLAR_OS_NATIVE_ERROR_IO;
    }
    atomic_store(&device->measuring, 0U);
    return result;
}

static int attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count, void **instance)
{
    const char *bus_name = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS) bus_name = bindings[i].target;
    }
    if (!bus_name || !instance) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    sht40_device_t *device = memory->alloc(sizeof(*device));
    if (!device) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    *device = (sht40_device_t){0};
    atomic_init(&device->measuring, 0U);
    int result = bus->acquire(bus_name, SOLAR_OS_NATIVE_BUS_I2C, name, &device->bus_lease);
    if (!result) {
        const uint8_t command = SHT40_CMD_SOFT_RESET;
        result = bus->i2c_write(device->bus_lease, SHT40_SENSOR_ADDRESS, &command, sizeof(command));
    }
    if (!result) {
        const solar_os_native_sensor_provider_v1_t provider = {
            .kind = SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT,
            .name = name,
            .driver = "sht40-driver",
            .user = device,
            .ops.environment.read = read_environment,
        };
        result = sensors->provider_register(&provider, &device->provider);
    }
    if (result) {
        if (device->bus_lease) bus->close(device->bus_lease);
        memory->free(device);
        return result;
    }
    *instance = device;
    return SOLAR_OS_NATIVE_OK;
}

static int detach(void *instance)
{
    sht40_device_t *device = instance;
    if (device->provider) {
        const int result = sensors->provider_close(device->provider);
        if (result) return result;
        device->provider = 0;
    }
    const int result = bus->close(device->bus_lease);
    if (result) return result;
    memory->free(device);
    return SOLAR_OS_NATIVE_OK;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_get(
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_host_api_v1_t, get_service));
    if (!host) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    const solar_os_native_driver_registration_api_v1_t *driver = solar_os_native_driver_registration_get(
        host->get_service, SOLAR_OS_NATIVE_THROUGH(solar_os_native_driver_registration_api_v1_t, register_driver));
    if (!driver || !driver->register_driver) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    bus = solar_os_native_bus_get(host->get_service, SOLAR_OS_NATIVE_THROUGH(solar_os_native_bus_api_v1_t, i2c_write));
    memory = solar_os_native_memory_get(host->get_service, SOLAR_OS_NATIVE_THROUGH(solar_os_native_memory_api_v1_t, free));
    sensors = solar_os_native_sensors_get(host->get_service, SOLAR_OS_NATIVE_THROUGH(solar_os_native_sensors_api_v1_t, provider_close));
    clock = solar_os_native_time_get(host->get_service, SOLAR_OS_NATIVE_THROUGH(solar_os_native_time_api_v1_t, sleep_ms));
    if (!bus || !memory || !sensors || !clock) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;

    static const int addresses[] = {SHT40_SENSOR_ADDRESS};
    static const solar_os_expansion_binding_spec_t bindings[] = {
        {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
        {.key = "addr", .value_hint = "0x44", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
         .required = true, .allowed_values = addresses, .allowed_value_count = 1},
    };
    const solar_os_native_bound_driver_descriptor_v1_t descriptor = {
        .abi_version = SOLAR_OS_NATIVE_DRIVER_REGISTRATION_ABI,
        .struct_size = sizeof(descriptor),
        .id = "sht40-driver",
        .summary = "SHT40 temperature and humidity sensor",
        .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
        .binding_specs = bindings,
        .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
        .attach = attach,
        .detach = detach,
    };
    return driver->register_driver(&descriptor);
}
