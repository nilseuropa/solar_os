#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_shell_expansion_internal.h"
#include "solar_os_shell_parse.h"
#include "../../src/services/solar_os_gpio_matrix_driver.c"

esp_err_t solar_os_gpio_matrix_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{ (void)name; (void)bindings; (void)count; return ESP_OK; }
esp_err_t solar_os_gpio_matrix_detach(const char *name)
{ (void)name; return ESP_OK; }

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

    char key[24];
    char value[32];
    bool string_value = false;
    assert(solar_os_shell_expansion_binding_manifest_field(
               &display_driver, &display.bindings[0], key, sizeof(key),
               value, sizeof(value), &string_value) == ESP_OK);
    assert(strcmp(key, "spi") == 0);
    assert(strcmp(value, "spi0") == 0);
    assert(string_value);
    assert(solar_os_shell_expansion_binding_manifest_field(
               &display_driver, &display.bindings[2], key, sizeof(key),
               value, sizeof(value), &string_value) == ESP_OK);
    assert(strcmp(key, "dc") == 0);
    assert(strcmp(value, "10") == 0);
    assert(!string_value);

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
    assert(solar_os_shell_expansion_binding_manifest_field(
               &sensor_driver, &sensor.bindings[2], key, sizeof(key),
               value, sizeof(value), &string_value) == ESP_OK);
    assert(strcmp(key, "power") == 0);
    assert(strcmp(value, "gpiox0:9") == 0);
    assert(string_value);

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

    solar_os_expansion_device_t matrix = {.name="matrix-keyboard", .driver="gpio-matrix"};
    for (unsigned i = 0; i < 18; i++) {
        char role[8]; snprintf(role, sizeof(role), "%s%u", i < 8 ? "row" : "col", i < 8 ? i : i - 8);
        matrix.bindings[matrix.binding_count++] = binding(SOLAR_OS_EXPANSION_BINDING_GPIO, role, "", (int)i + 10);
    }
    matrix.bindings[matrix.binding_count++] = binding(SOLAR_OS_EXPANSION_BINDING_PARAMETER, "drive", "", 1);
    matrix.bindings[matrix.binding_count++] = binding(SOLAR_OS_EXPANSION_BINDING_PARAMETER, "diodes", "", 1);
    matrix.bindings[matrix.binding_count++] = binding(SOLAR_OS_EXPANSION_BINDING_PARAMETER, "debounce-ms", "", 100);
    char matrix_command[SOLAR_OS_SHELL_COMMAND_LINE_MAX];
    assert(solar_os_shell_expansion_attach_command(&solar_os_gpio_matrix_expansion_driver,
        &matrix, matrix_command, sizeof(matrix_command)) == ESP_OK);
    assert(strlen(matrix_command) > 192 && strstr(matrix_command, "col9=gpio27") != NULL);
    char *matrix_argv[SOLAR_OS_SHELL_COMMAND_ARG_MAX];
    solar_os_shell_parse_result_t parsed = solar_os_shell_tokenize(matrix_command,
        matrix_argv, SOLAR_OS_SHELL_COMMAND_ARG_MAX);
    assert(parsed.error == SOLAR_OS_SHELL_PARSE_OK && parsed.argc == 25);
    assert(strcmp(matrix_argv[24], "debounce-ms=100") == 0);
    puts("expansion command tests passed (including full GPIO matrix startup command)");
    return 0;
}
