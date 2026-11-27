#include "solar_os_native_client.h"
#include "solar_os_native_device_providers.h"
#include "solar_os_native_image.h"
#include "solar_os_native_runtime.h"
#include "solar_os_sensors.h"
#include "solar_os_jobs.h"
#include "freertos/task.h"

static unsigned stream_refs, resets, samples;
static bool inject_io_error, test_busy_detach;

bool solar_os_memory_register_reclaimer(solar_os_memory_reclaimer_t *reclaimer)
{
    return reclaimer != NULL && reclaimer->reclaim != NULL;
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }

esp_err_t solar_os_jobs_register_dynamic(const char *id, const char *summary, const solar_os_job_t *job)
{
    (void)id; (void)summary; (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_jobs_unregister_dynamic(const char *id, const solar_os_job_t *job)
{
    (void)id; (void)job;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_gpio_controller_unregister_owned(const char *name, const void *context)
{
    (void)name; (void)context;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_stream_register_many(const solar_os_stream_driver_t *drivers, size_t count)
{
    assert(drivers && count == 2 && !stream_refs);
    stream_refs = count;
    return ESP_OK;
}

esp_err_t solar_os_stream_unregister_many_owned(const char *const *ids, const void *const *users, size_t count)
{
    assert(ids && users && count == stream_refs);
    stream_refs = 0;
    return ESP_OK;
}

static void *driver_allocate(size_t size)
{
    return solar_os_memory_calloc(1, size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "driver.instance");
}

static int driver_bus_acquire(const char *name, uint32_t protocol, const char *owner, solar_os_native_handle_t *lease)
{
    assert(!strcmp(name, "i2c0") && protocol == SOLAR_OS_NATIVE_BUS_I2C && !strcmp(owner, sensor_name));
    assert(solar_os_native_driver_has_binding(owner, SOLAR_OS_EXPANSION_BINDING_I2C_BUS, name));
    assert(!solar_os_native_driver_has_binding("other", SOLAR_OS_EXPANSION_BINDING_I2C_BUS, name));
    assert(!solar_os_native_driver_has_binding(owner, SOLAR_OS_EXPANSION_BINDING_I2C_BUS, "i2c1"));
    assert(bus_refs == 1 && lease);
    ++bus_refs;
    *lease = 42;
    return SOLAR_OS_NATIVE_OK;
}

static int driver_bus_close(solar_os_native_handle_t lease)
{
    assert(lease == 42 && bus_refs == 2);
    --bus_refs;
    return SOLAR_OS_NATIVE_OK;
}

static int driver_i2c_write(solar_os_native_handle_t lease, uint32_t address, const void *data, size_t size)
{
    assert(lease == 42 && address == 0x44 && bus_refs == 2 && size == 1 && data);
    const uint8_t command = *(const uint8_t *)data;
    if (inject_io_error) return SOLAR_OS_NATIVE_ERROR_IO;
    if (command == 0x94) ++resets;
    else {
        assert(command == 0xfd);
        ++samples;
        if (test_busy_detach) {
            assert(solar_os_expansion_detach(sensor_name) == ESP_ERR_INVALID_STATE);
            assert(solar_os_native_module_deactivate(SOLAR_OS_MODULE_TYPE_DRIVER, "sht40-driver", NULL, 0) == ESP_ERR_INVALID_STATE);
        }
    }
    return SOLAR_OS_NATIVE_OK;
}

static int driver_i2c_read(solar_os_native_handle_t lease, uint32_t address, void *data, size_t size)
{
    const uint8_t response[] = {0x80, 0x00, 0xa2, 0x80, 0x00, 0xa2};
    assert(lease == 42 && address == 0x44 && bus_refs == 2 && size == sizeof(response));
    memcpy(data, response, size);
    return SOLAR_OS_NATIVE_OK;
}

static void driver_sleep(uint32_t delay) { assert(delay == 10 && samples != 0); }

static const void *driver_service(const char *name, uint32_t abi, uint32_t minimum)
{
    static const solar_os_native_driver_registration_api_v1_t driver_api = {
        .abi_version = 1, .struct_size = sizeof(driver_api), .register_driver = solar_os_native_register_bound_driver,
    };
    static const solar_os_native_bus_api_v1_t bus_api = {
        .abi_version = 1, .struct_size = sizeof(bus_api), .acquire = driver_bus_acquire, .close = driver_bus_close,
        .i2c_read = driver_i2c_read, .i2c_write = driver_i2c_write,
    };
    static const solar_os_native_sensors_api_v1_t sensors_api = {
        .abi_version = 1, .struct_size = sizeof(sensors_api),
        .provider_register = solar_os_native_sensor_provider_register, .provider_close = solar_os_native_sensor_provider_close,
    };
    static const solar_os_native_memory_api_v1_t memory_api = {
        .abi_version = 1, .struct_size = sizeof(memory_api), .alloc = driver_allocate, .free = solar_os_memory_free,
    };
    static const solar_os_native_time_api_v1_t time_api = {
        .abi_version = 1, .struct_size = sizeof(time_api), .sleep_ms = driver_sleep,
    };
    if (abi != 1) return NULL;
    if (!strcmp(name, "driver.registration") && minimum <= sizeof(driver_api)) return &driver_api;
    if (!strcmp(name, "bus") && minimum <= sizeof(bus_api)) return &bus_api;
    if (!strcmp(name, "sensors") && minimum <= sizeof(sensors_api)) return &sensors_api;
    if (!strcmp(name, "memory") && minimum <= sizeof(memory_api)) return &memory_api;
    if (!strcmp(name, "time") && minimum <= sizeof(time_api)) return &time_api;
    return NULL;
}

const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void)
{
    static const solar_os_native_host_api_v1_t host = {
        .abi_version = 1, .struct_size = sizeof(host), .get_service = driver_service,
    };
    return &host;
}

#define main sht40_module_entry
#include "../../modules/examples/sht40-driver/main/sht40_driver.c"
#undef main

static bool image_loaded;
esp_err_t solar_os_native_image_load(const char *path, void **image, char *detail, size_t detail_len)
{
    (void)detail; (void)detail_len;
    assert(!image_loaded && !strcmp(path, "/modules/drivers/sht40-driver.elf"));
    image_loaded = true;
    *image = &image_loaded;
    return ESP_OK;
}

int solar_os_native_image_request(void *image, int argc, char **argv)
{
    assert(image_loaded && image == &image_loaded);
    return sht40_module_entry(argc, argv);
}

void solar_os_native_image_close(void *image)
{
    assert(image_loaded && image == &image_loaded && !bus_refs && !stream_refs);
    image_loaded = false;
}

static void test_bound_driver(void)
{
    const char id[] = "sht40-driver";
    assert(solar_os_sensors_init() == ESP_OK);
    solar_os_native_handle_t provider = 1;
    const solar_os_native_sensor_provider_v1_t unowned = {
        .kind = SOLAR_OS_NATIVE_SENSOR_ENVIRONMENT, .name = sensor_name, .driver = id,
        .ops.environment.read = read_environment,
    };
    assert(solar_os_native_sensor_provider_register(&unowned, &provider) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!provider && !stream_refs && !live_allocations);
    assert(sht40_module_entry(0, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(solar_os_native_module_activate(SOLAR_OS_MODULE_TYPE_DRIVER, id, "/modules/drivers/sht40-driver.elf", NULL, 0) == ESP_OK);
    assert(image_loaded && live_allocations == 1);
    solar_os_expansion_driver_info_t info;
    assert(solar_os_expansion_find_driver_info(id, &info) == ESP_OK);
    assert(info.category == SOLAR_OS_EXPANSION_CATEGORY_SENSOR && info.binding_spec_count == 2);
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x45},
    };
    assert(solar_os_expansion_attach(id, sensor_name, bindings, 2) == ESP_ERR_INVALID_ARG);
    assert(live_allocations == 1 && !stream_refs && !bus_refs);
    bindings[1].value = 0x44;
    inject_io_error = true;
    assert(solar_os_expansion_attach(id, sensor_name, bindings, 2) == ESP_FAIL);
    assert(live_allocations == 1 && !stream_refs && !bus_refs);
    inject_io_error = false;
    assert(solar_os_expansion_attach(id, sensor_name, bindings, 2) == ESP_OK);
    assert(bus_refs == 2 && stream_refs == 2 && resets == 1);
    assert(solar_os_native_module_deactivate(SOLAR_OS_MODULE_TYPE_DRIVER, id, NULL, 0) == ESP_ERR_INVALID_STATE);
    solar_os_environment_t environment;
    test_busy_detach = true;
    assert(solar_os_sensors_read_environment_from(sensor_name, &environment) == ESP_OK);
    assert(samples == 1 && environment.temperature_c > 42 && environment.temperature_c < 43);
    assert(environment.humidity_percent > 56 && environment.humidity_percent < 57);
    test_busy_detach = false;
    assert(solar_os_expansion_detach(sensor_name) == ESP_OK);
    assert(!stream_refs && !bus_refs && live_allocations == 1);
    assert(solar_os_sensors_read_environment_from(sensor_name, &environment) == ESP_ERR_NOT_FOUND);
    assert(solar_os_native_module_deactivate(SOLAR_OS_MODULE_TYPE_DRIVER, id, NULL, 0) == ESP_OK);
    assert(!image_loaded && !live_allocations);
    assert(solar_os_expansion_find_driver_info(id, &info) == ESP_ERR_NOT_FOUND);
}
