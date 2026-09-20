#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_shell_expansion_internal.h"

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

static solar_os_expansion_binding_t binding(
    solar_os_expansion_binding_kind_t kind,
    const char *role,
    const char *target,
    int value)
{
    solar_os_expansion_binding_t result = {
        .kind = kind,
        .value = value,
        .aux = -1,
    };
    strlcpy(result.role, role != NULL ? role : "", sizeof(result.role));
    strlcpy(result.target, target != NULL ? target : "", sizeof(result.target));
    return result;
}

int main(void)
{
    static const solar_os_expansion_binding_spec_t display_specs[] = {
        {.key = "spi", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS},
        {.key = "cs", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS},
        {.key = "dc", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc"},
        {.key = "reset", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
        {.key = "busy", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy"},
    };
    const solar_os_expansion_driver_t display_driver = {
        .name = "ssd1683",
        .binding_specs = display_specs,
        .binding_spec_count = sizeof(display_specs) / sizeof(display_specs[0]),
    };
    solar_os_expansion_device_t display = {
        .name = "display0",
        .driver = "ssd1683",
        .binding_count = 5U,
    };
    display.bindings[0] = binding(SOLAR_OS_EXPANSION_BINDING_SPI_BUS,
                                  "", "spi0", -1);
    display.bindings[1] = binding(SOLAR_OS_EXPANSION_BINDING_SPI_CS,
                                  "cs", "spi0", 9);
    display.bindings[2] = binding(SOLAR_OS_EXPANSION_BINDING_GPIO,
                                  "dc", "", 10);
    display.bindings[3] = binding(SOLAR_OS_EXPANSION_BINDING_GPIO,
                                  "reset", "", 11);
    display.bindings[4] = binding(SOLAR_OS_EXPANSION_BINDING_GPIO,
                                  "busy", "", 12);

    char command[192];
    assert(solar_os_shell_expansion_attach_command(&display_driver,
                                                    &display,
                                                    command,
                                                    sizeof(command)) == ESP_OK);
    assert(strcmp(command,
                  "expansion attach ssd1683 display0 spi=spi0 cs=gpio9 "
                  "dc=gpio10 reset=gpio11 busy=gpio12") == 0);

    static const solar_os_expansion_binding_spec_t sensor_specs[] = {
        {.key = "i2c", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS},
        {.key = "addr", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS},
        {.key = "power", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE},
    };
    const solar_os_expansion_driver_t sensor_driver = {
        .name = "sensor",
        .binding_specs = sensor_specs,
        .binding_spec_count = sizeof(sensor_specs) / sizeof(sensor_specs[0]),
    };
    solar_os_expansion_device_t sensor = {
        .name = "sensor one",
        .driver = "sensor",
        .binding_count = 3U,
    };
    sensor.bindings[0] = binding(SOLAR_OS_EXPANSION_BINDING_I2C_BUS,
                                 "", "i2c0", -1);
    sensor.bindings[1] = binding(SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
                                 "", "", 0x5f);
    sensor.bindings[2] = binding(SOLAR_OS_EXPANSION_BINDING_GPIO_LINE,
                                 "power", "gpiox0", 9);
    assert(solar_os_shell_expansion_attach_command(&sensor_driver,
                                                    &sensor,
                                                    command,
                                                    sizeof(command)) == ESP_OK);
    assert(strcmp(command,
                  "expansion attach sensor \"sensor one\" i2c=i2c0 "
                  "addr=0x5f power=gpiox0:9") == 0);

    const solar_os_expansion_driver_t manual_driver = {
        .name = "manual",
        .allow_unlisted_bindings = true,
    };
    solar_os_expansion_device_t manual = {
        .name = "keys;one",
        .driver = "manual",
        .binding_count = 2U,
    };
    manual.bindings[0] = binding(SOLAR_OS_EXPANSION_BINDING_GPIO,
                                 "UP", "", 17);
    manual.bindings[1] = binding(SOLAR_OS_EXPANSION_BINDING_PARAMETER,
                                 "count", "", 8);
    assert(solar_os_shell_expansion_attach_command(&manual_driver,
                                                    &manual,
                                                    command,
                                                    sizeof(command)) == ESP_OK);
    assert(strcmp(command,
                  "expansion attach manual \"keys;one\" "
                  "key:UP=gpio17 count=8") == 0);

    char short_command[24];
    assert(solar_os_shell_expansion_attach_command(&display_driver,
                                                    &display,
                                                    short_command,
                                                    sizeof(short_command)) ==
           ESP_ERR_INVALID_SIZE);

    puts("expansion command tests passed");
    return 0;
}
