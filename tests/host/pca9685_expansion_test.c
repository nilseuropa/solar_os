#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_pca9685.h"
#include "solar_os_pwm.h"
#include "solar_os_servo.h"
#define RAD(x) ((x) * (3.14159265358979323846f / 180.0f))
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"
#include "freertos/FreeRTOS.h"

extern const solar_os_expansion_driver_t solar_os_pca9685_expansion_driver;
static uint8_t registers[2][256];
static int fail_reg = -1;
static unsigned fail_writes;
static unsigned allocations, allocated_bytes, probes;
static bool fail_probe, fail_alloc, oe_level, fail_oe;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src), copied = len < size - 1U ? len : size - 1U;
    if (size) { memcpy(dst, src, copied); dst[copied] = '\0'; }
    return len;
}
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && strcmp(tag, "pca9685.device") == 0);
    if (fail_alloc) return NULL;
    void *ptr = calloc(count, size);
    if (ptr) { allocations++; allocated_bytes = (unsigned)(count * size); }
    return ptr;
}
void solar_os_memory_free(void *ptr) { if (ptr) { assert(allocations); allocations--; free(ptr); } }
void vTaskDelay(TickType_t ticks) { assert(ticks >= 1U); }

bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    (void)bus; (void)index;
    return strcmp(name, "i2c0") == 0 || strcmp(name, "i2c1") == 0;
}
esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address)
{
    assert(strcmp(name, "i2c0") == 0 || strcmp(name, "i2c1") == 0);
    assert(address == 0x40 || address == 0x41);
    probes++;
    return fail_probe ? ESP_FAIL : ESP_OK;
}
esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
    (void)name;
    assert(address == 0x40 || address == 0x41);
    assert((size_t)reg + len <= 256);
    memcpy(data, &registers[address - 0x40][reg], len);
    return ESP_OK;
}
esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t address, uint8_t reg, const uint8_t *data, size_t len)
{
    (void)name;
    assert(address == 0x40 || address == 0x41);
    assert((size_t)reg + len <= 256);
    uint8_t *regs = registers[address - 0x40];
    if (reg == 0xfe) assert(regs[0] & 0x10);
    if (len > 1) assert(regs[0] & 0x20);
    if (reg == fail_reg && fail_writes) { fail_writes--; return ESP_FAIL; }
    memcpy(&regs[reg], data, len);
    if (reg == 0xfd && len == 1) for (size_t i = 0; i < 16; i++) regs[0x09 + 4 * i] = data[0];
    return ESP_OK;
}
esp_err_t solar_os_gpio_line_configure(const solar_os_gpio_line_ref_t *line,
    solar_os_gpio_line_mode_t mode, solar_os_gpio_line_pull_t pull)
{
    assert(line->line == 2 && line->controller[0] == '\0');
    assert(mode == SOLAR_OS_GPIO_LINE_MODE_OUTPUT && pull == SOLAR_OS_GPIO_LINE_PULL_NONE);
    return ESP_OK;
}
esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *line, bool level)
{
    assert(line->line == 2);
    if (fail_oe) return ESP_FAIL;
    oe_level = level;
    return ESP_OK;
}

int main(void)
{
    const solar_os_expansion_driver_t *driver = &solar_os_pca9685_expansion_driver;
    assert(strcmp(driver->name, "pca9685") == 0 && driver->probe_supported);
    assert(driver->required_capabilities == SOLAR_OS_BOARD_CAP_EXPANSION_I2C);
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x40},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "oe", .value = 2},
    };
    fail_alloc = true;
    assert(driver->attach("servos0", bindings, 3) == ESP_ERR_NO_MEM && !allocations);
    fail_alloc = false;
    bindings[1].value = 0x70;
    assert(driver->attach("servos0", bindings, 3) == ESP_ERR_INVALID_ARG && !allocations && !probes);
    bindings[1].value = 0x40;
    strcpy(bindings[1].target, "i2c1");
    assert(driver->attach("servos0", bindings, 3) == ESP_ERR_INVALID_ARG && !allocations && !probes);
    bindings[1].target[0] = '\0';
    fail_probe = true;
    assert(driver->attach("servos0", bindings, 3) == ESP_FAIL && !allocations && oe_level);
    fail_probe = false;
    assert(driver->attach("bad:name", bindings, 3) == ESP_ERR_INVALID_ARG);
    assert(driver->attach("servos0", bindings, 3) == ESP_OK && allocations == 1 && !oe_level);
    assert(allocated_bytes < 512U); /* No preallocated output-info/owner arrays. */
    assert(driver->attach("servos0", bindings, 3) == ESP_ERR_INVALID_STATE && allocations == 1);
    assert(solar_os_pwm_output_count() == 16);
    solar_os_pwm_output_info_t info;
    for (size_t i = 0; i < 16; i++) {
        assert(solar_os_pwm_get_output_info(i, &info));
        assert(strcmp(info.provider, "servos0") == 0 && info.output == i && info.pin == -1);
        assert(!info.active && !info.enabled && info.frequency_group == 0 && info.resolution_bits == 12);
        assert(registers[0][0x09 + 4 * i] == 0x10);
    }
    solar_os_servo_config_t config = {
        .output = {.provider = "servos0", .output = 0}, .freq_hz = 50,
        .pulse_min_us = 1000, .pulse_max_us = 2000, .angle_min_rad = RAD(-45), .angle_max_rad = RAD(45),
    };
    assert(solar_os_servo_open("arm", "python", &config) == ESP_OK);
    assert(solar_os_servo_set("arm", "python", 0) == ESP_OK);
    assert(solar_os_pwm_get_output_info(0, &info) && info.enabled && info.pulse_width_us == 1498);
    assert(info.duty_percent == 7 && strcmp(info.owner, "servo:arm") == 0);
    solar_os_pwm_output_t output = {.provider = "servos0", .output = 1};
    assert(solar_os_pwm_set_output(&output, 60, 50, false) == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_set_output(&output, 50, 100, false) == ESP_OK);
    assert(registers[0][0x0b] == 0x10 && registers[0][0x0d] == 0);
    assert(driver->detach("servos0") == ESP_ERR_INVALID_STATE && !oe_level && allocations == 1);
    fail_reg = 0x06; fail_writes = 1;
    assert(solar_os_servo_set("arm", "python", RAD(20)) == ESP_FAIL);
    solar_os_servo_info_t servo;
    assert(solar_os_servo_status("arm", &servo) == ESP_OK && servo.commanded_angle_rad == 0);
    assert(solar_os_servo_disable("arm", "python") == ESP_OK);
    assert(driver->detach("servos0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_pwm_set_output(&output, 60, 50, false) == ESP_ERR_INVALID_STATE);
    fail_reg = 0x06; fail_writes = 2; /* Closing fails, including restoration. */
    assert(solar_os_servo_close("arm", "python") == ESP_FAIL);
    assert(solar_os_pwm_get_output_info(0, &info) && info.reserved);
    assert(driver->detach("servos0") == ESP_ERR_INVALID_STATE);
    fail_reg = -1;
    assert(solar_os_servo_close("arm", "python") == ESP_OK);
    assert(solar_os_pwm_stop_output(&output) == ESP_OK);
    fail_oe = true;
    assert(driver->detach("servos0") == ESP_FAIL && allocations == 1);
    assert(solar_os_pwm_provider_count() == 0); /* Unregistered, context retained for retry. */
    fail_oe = false;
    fail_reg = 0xfd; fail_writes = 1;
    assert(driver->detach("servos0") == ESP_FAIL && allocations == 1 && oe_level);
    assert(solar_os_pwm_provider_count() == 0);
    assert(driver->detach("servos0") == ESP_OK && !allocations);
    assert(driver->detach("servos0") == ESP_ERR_NOT_FOUND);

    /* Two controllers have independent prescalers; calibration is per chip. */
    assert(driver->attach("servos0", bindings, 2) == ESP_OK);
    bindings[1].value = 0x41;
    bindings[2] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "osc_hz", .value = 26000000};
    assert(driver->attach("servos1", bindings, 3) == ESP_OK);
    output = (solar_os_pwm_output_t){.provider = "servos0", .output = 15};
    assert(solar_os_pwm_set_output(&output, 50, 1500, true) == ESP_OK);
    output = (solar_os_pwm_output_t){.provider = "servos1", .output = 15};
    assert(solar_os_pwm_set_output(&output, 100, 1500, true) == ESP_OK);
    assert(registers[0][0xfe] == 121 && registers[1][0xfe] == 62);
    assert(solar_os_pwm_stop_output(&output) == ESP_OK);
    assert(driver->detach("servos1") == ESP_OK);
    output = (solar_os_pwm_output_t){.provider = "servos0", .output = 15};
    /* A single output may change its frequency; failed changes restore it. */
    fail_reg = 0xfe; fail_writes = 1;
    assert(solar_os_pwm_set_output(&output, 100, 1500, true) == ESP_FAIL);
    assert(registers[0][0xfe] == 121);
    assert(solar_os_pwm_set_output(&output, 100, 1500, true) == ESP_OK);
    assert(solar_os_pwm_stop_output(&output) == ESP_OK);
    assert(driver->detach("servos0") == ESP_OK && !allocations);
    puts("PCA9685 expansion/provider/servo integration tests passed");
    return 0;
}
