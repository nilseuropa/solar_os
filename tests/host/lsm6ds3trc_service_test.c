#include <assert.h>
#include <string.h>
#include "solar_os_lsm6ds3trc.h"
#include "solar_os_imu.h"
#include "solar_os_buses.h"
#include "freertos/semphr.h"

static uint8_t registers[256];
static int64_t now;
static solar_os_imu_registration_t provider;
static esp_err_t register_result, unregister_result;
static unsigned released;

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex); released++; }
void vTaskDelay(TickType_t ticks) { now += (int64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return now; }
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index) {
    (void)bus; (void)index; return !strcmp(name, "i2c0");
}
esp_err_t solar_os_bus_i2c_read_reg(const char *bus, uint8_t addr, uint8_t reg, uint8_t *data, size_t size) {
    assert(!strcmp(bus, "i2c0") && addr == 0x6a);
    if (reg == 0x12) registers[reg] &= ~1U;
    memcpy(data, registers + reg, size); return ESP_OK;
}
esp_err_t solar_os_bus_i2c_write_reg(const char *bus, uint8_t addr, uint8_t reg, const uint8_t *data, size_t size) {
    assert(!strcmp(bus, "i2c0") && addr == 0x6a && size == 1);
    registers[reg] = *data; return ESP_OK;
}
esp_err_t solar_os_imu_register(const solar_os_imu_registration_t *registration) {
    assert(!strcmp(registration->driver, "lsm6ds3tr-c"));
    assert(registration->capabilities == (SOLAR_OS_IMU_CAP_ACCELERATION | SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY));
    provider = *registration; return register_result;
}
esp_err_t solar_os_imu_unregister(const char *name) {
    assert(!strcmp(name, "imu0")); return unregister_result;
}
int main(void) {
    const solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x6a},
    };
    registers[0x0f] = 0x6a;
    register_result = ESP_FAIL;
    assert(solar_os_lsm6ds3trc_attach("imu0", bindings, 2) == ESP_FAIL);
    assert(released == 1 && registers[0x10] == 0 && registers[0x11] == 0);
    register_result = ESP_OK;
    assert(solar_os_lsm6ds3trc_attach("imu0", bindings, 2) == ESP_OK);
    assert(solar_os_lsm6ds3trc_attach("imu0", bindings, 2) == ESP_ERR_INVALID_STATE);
    solar_os_imu_sample_t sample = {.timestamp_us=123}, before = sample;
    assert(provider.ops->read_sample(provider.ctx, 10, &sample) == ESP_ERR_TIMEOUT);
    assert(now == 10000 && !memcmp(&sample, &before, sizeof(sample)));
    registers[0x1e] = 3;
    assert(provider.ops->read_sample(provider.ctx, 10, &sample) == ESP_OK);
    assert(sample.valid == provider.capabilities && sample.timestamp_us == 10000);
    unregister_result = ESP_FAIL;
    assert(solar_os_lsm6ds3trc_detach("imu0") == ESP_FAIL && registers[0x10] == 0x40);
    unregister_result = ESP_OK;
    assert(solar_os_lsm6ds3trc_detach("imu0") == ESP_OK && released == 2);
    assert(registers[0x10] == 0 && registers[0x11] == 0);
    return 0;
}
