#include <assert.h>
#include <string.h>
#include "solar_os_bq27220.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"

static solar_os_battery_provider_t provider;
static uint16_t soc;
static bool discharging;
static int failing_reg = -1;

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index) {
    (void)bus; (void)index; return !strcmp(name, "i2c0");
}
esp_err_t solar_os_bus_i2c_probe(const char *bus, uint8_t address) {
    assert(!strcmp(bus, "i2c0") && address == 0x55); return ESP_OK;
}
esp_err_t solar_os_bus_i2c_read_reg(const char *bus, uint8_t address, uint8_t reg, uint8_t *data, size_t len) {
    assert(!strcmp(bus, "i2c0") && address == 0x55);
    if (reg == failing_reg) return ESP_FAIL;
    if (reg == 0x08) {
        assert(len == 4); data[0] = 0x74; data[1] = 0x0e;
        data[2] = discharging; data[3] = 0;
    } else {
        assert(reg == 0x2c && len == 2); data[0] = soc; data[1] = soc >> 8;
    }
    return ESP_OK;
}
esp_err_t solar_os_battery_register_provider(const char *name, const solar_os_battery_provider_t *value) {
    assert(!strcmp(name, "battery0")); provider = *value; return ESP_OK;
}
esp_err_t solar_os_battery_unregister_provider(const char *name) {
    assert(!strcmp(name, "battery0")); memset(&provider, 0, sizeof(provider)); return ESP_OK;
}
int main(void) {
    const solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value=0x55},
    };
    assert(solar_os_bq27220_attach("battery0", bindings, 2) == ESP_OK);
    for (soc = 0; soc <= 100; soc++) {
        for (unsigned state = 0; state < 2; state++) {
            discharging = state;
            solar_os_battery_sample_t sample = {0};
            assert(provider.read(provider.user, &sample) == ESP_OK);
            assert(sample.battery_mv == 3700 && sample.percent_valid && sample.percent == soc);
            assert(sample.charging == !discharging && sample.external_power == !discharging);
        }
    }
    const uint16_t invalid[] = {101, 256, 65535};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        soc = invalid[i];
        solar_os_battery_sample_t sample = {.battery_mv=1234}, before = sample;
        assert(provider.read(provider.user, &sample) == ESP_ERR_INVALID_RESPONSE);
        assert(!memcmp(&sample, &before, sizeof(sample)));
    }
    soc = 50;
    for (unsigned read = 0; read < 2; read++) {
        failing_reg = read ? 0x2c : 0x08;
        solar_os_battery_sample_t sample = {.battery_mv=1234}, before = sample;
        assert(provider.read(provider.user, &sample) == ESP_FAIL);
        assert(!memcmp(&sample, &before, sizeof(sample)));
    }
    assert(solar_os_bq27220_detach("battery0") == ESP_OK);
    return 0;
}
