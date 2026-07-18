#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "solar_os_buses.h"
#include "solar_os_expansion.h"
#include "solar_os_resources.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void expansion_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  expansion [status]");
    solar_os_shell_io_writeln(term, "  expansion scan");
    solar_os_shell_io_writeln(term, "  expansion drivers");
    solar_os_shell_io_writeln(term, "  expansion devices");
    solar_os_shell_io_writeln(term, "  expansion bus create spi <name> host=<spi2|spi3> sclk=<gpio> mosi=<gpio> [miso=<gpio|none>] cs=<gpio> [cs=<gpio> ...] [max=<bytes>]");
    solar_os_shell_io_writeln(term, "  expansion bus remove <name>");
    solar_os_shell_io_writeln(term, "  expansion attach <driver> <name> <resource...>");
    solar_os_shell_io_writeln(term, "  expansion detach <name>");
}

static bool parse_int_arg(const char *text, int min, int max, int *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    if (strncmp(text, "gpio", 4) == 0) {
        text += 4;
    }
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static void print_cap(solar_os_shell_io_t *term, solar_os_board_capability_t cap, const char *name)
{
    solar_os_shell_io_printf(term, "%s%s", solar_os_board_has(cap) ? " " : "", solar_os_board_has(cap) ? name : "");
}

static void print_bus_cap(solar_os_shell_io_t *term,
                          solar_os_bus_protocol_t protocol,
                          const char *name)
{
    if (solar_os_bus_count_protocol(protocol) > 0) {
        solar_os_shell_io_printf(term, " %s", name);
    }
}

static void expansion_print_bus_meta(solar_os_shell_io_t *term,
                                     const char *name,
                                     solar_os_bus_protocol_t protocol)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, protocol, &info)) {
        return;
    }
    if (protocol == SOLAR_OS_BUS_PROTOCOL_SPI) {
        solar_os_shell_io_printf(term,
                                 " [%s %s %s leases=%u]",
                                 solar_os_bus_origin_name(info.origin),
                                 solar_os_bus_sharing_name(info.sharing),
                                 info.ready ? "ready" : "idle",
                                 (unsigned)info.lease_count);
    } else {
        solar_os_shell_io_printf(term,
                                 " [%s %s leases=%u]",
                                 solar_os_bus_origin_name(info.origin),
                                 solar_os_bus_sharing_name(info.sharing),
                                 (unsigned)info.lease_count);
    }
}

static const char *spi_host_name(int host)
{
    switch (host) {
    case SPI2_HOST:
        return "spi2";
    case SPI3_HOST:
        return "spi3";
    default:
        return "unknown";
    }
}

static void expansion_print_resources(solar_os_shell_io_t *term)
{
    solar_os_shell_io_write(term, "Capabilities:");
    print_cap(term, SOLAR_OS_BOARD_CAP_EXPANSION_GPIO, "gpio");
    print_bus_cap(term, SOLAR_OS_BUS_PROTOCOL_I2C, "i2c");
    print_bus_cap(term, SOLAR_OS_BUS_PROTOCOL_SPI, "spi");
    print_bus_cap(term, SOLAR_OS_BUS_PROTOCOL_UART, "uart");
    print_bus_cap(term, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, "onewire");
    print_cap(term, SOLAR_OS_BOARD_CAP_EXPANSION_ADC, "adc");
    print_cap(term, SOLAR_OS_BOARD_CAP_EXPANSION_PWM, "pwm");
    if (!solar_os_expansion_available()) {
        solar_os_shell_io_write(term, " none");
    }
    solar_os_shell_io_put_char(term, '\n');

    for (size_t i = 0; i < solar_os_expansion_i2c_bus_count(); i++) {
        solar_os_expansion_i2c_bus_t bus;
        if (solar_os_expansion_get_i2c_bus(i, &bus)) {
            solar_os_shell_io_printf(term,
                                     "I2C %-6s port %d SDA GPIO%d SCL GPIO%d",
                                     bus.name,
                                     bus.port,
                                     bus.sda_pin,
                                     bus.scl_pin);
            expansion_print_bus_meta(term, bus.name, SOLAR_OS_BUS_PROTOCOL_I2C);
            solar_os_shell_io_put_char(term, '\n');
        }
    }

    for (size_t i = 0; i < solar_os_expansion_spi_bus_count(); i++) {
        solar_os_expansion_spi_bus_t bus;
        if (!solar_os_expansion_get_spi_bus(i, &bus)) {
            continue;
        }
        solar_os_shell_io_printf(term,
                                 "SPI %-6s host %s SCK GPIO%d MISO ",
                                 bus.name,
                                 spi_host_name(bus.host),
                                 bus.sclk_pin);
        if (bus.miso_pin >= 0) {
            solar_os_shell_io_printf(term, "GPIO%d", bus.miso_pin);
        } else {
            solar_os_shell_io_write(term, "none");
        }
        solar_os_shell_io_printf(term, " MOSI GPIO%d CS", bus.mosi_pin);
        for (size_t cs = 0; cs < bus.cs_count && cs < SOLAR_OS_EXPANSION_SPI_CS_MAX; cs++) {
            solar_os_shell_io_printf(term, " %s(GPIO%d)", bus.cs[cs].name, bus.cs[cs].pin);
        }
        expansion_print_bus_meta(term, bus.name, SOLAR_OS_BUS_PROTOCOL_SPI);
        solar_os_shell_io_put_char(term, '\n');
    }

    for (size_t i = 0; i < solar_os_expansion_uart_port_count(); i++) {
        solar_os_expansion_uart_port_t port;
        if (solar_os_expansion_get_uart_port(i, &port)) {
            solar_os_shell_io_printf(term,
                                     "UART %-5s port %d TX GPIO%d RX GPIO%d",
                                     port.name,
                                     port.port,
                                     port.tx_pin,
                                     port.rx_pin);
            expansion_print_bus_meta(term, port.name, SOLAR_OS_BUS_PROTOCOL_UART);
            solar_os_shell_io_put_char(term, '\n');
        }
    }
}

static void expansion_print_drivers(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "DRIVER  PROBE  SUMMARY");
    for (size_t i = 0; i < solar_os_expansion_driver_count(); i++) {
        solar_os_expansion_driver_t driver;
        if (!solar_os_expansion_get_driver(i, &driver)) {
            continue;
        }
        solar_os_shell_io_printf(term,
                                 "%-7s %-5s %s%s\n",
                                 driver.name,
                                 driver.probe_supported ? "yes" : "no",
                                 driver.summary,
                                 solar_os_expansion_driver_supported(driver.name) ? "" : " (unsupported)");
    }
}

static void expansion_print_binding(solar_os_shell_io_t *term, const solar_os_expansion_binding_t *binding)
{
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        solar_os_shell_io_printf(term,
                                 " %s:%s=GPIO%d",
                                 solar_os_expansion_binding_kind_name(binding->kind),
                                 binding->role,
                                 binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        solar_os_shell_io_printf(term,
                                 " %s=%s",
                                 solar_os_expansion_binding_kind_name(binding->kind),
                                 binding->target);
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        solar_os_shell_io_printf(term, " addr=0x%02x", binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        solar_os_shell_io_printf(term, " cs:%s=GPIO%d", binding->target, binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        solar_os_shell_io_printf(term, " uart=%s", binding->target);
        break;
    default:
        break;
    }
}

static void expansion_print_devices(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_expansion_device_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "no expansion devices attached");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        solar_os_expansion_device_t device;
        if (!solar_os_expansion_get_device(i, &device)) {
            continue;
        }
        solar_os_shell_io_printf(term, "%s driver=%s", device.name, device.driver);
        for (size_t b = 0; b < device.binding_count; b++) {
            expansion_print_binding(term, &device.bindings[b]);
        }
        solar_os_shell_io_put_char(term, '\n');
    }
}

static void expansion_print_claims(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_resource_claim_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "Claims: none");
        return;
    }

    solar_os_shell_io_writeln(term, "Claims:");
    for (size_t i = 0; i < count; i++) {
        solar_os_resource_claim_t claim;
        if (!solar_os_resource_get_claim(i, &claim)) {
            continue;
        }
        solar_os_shell_io_printf(term,
                                 "  %-8s %d",
                                 solar_os_resource_kind_name(claim.kind),
                                 claim.primary);
        if (claim.secondary >= 0) {
            solar_os_shell_io_printf(term, ".%d", claim.secondary);
        }
        solar_os_shell_io_printf(term,
                                 " owner=%s%s%s\n",
                                 claim.owner,
                                 claim.label[0] != '\0' ? " label=" : "",
                                 claim.label);
    }
}

static void expansion_print_probe_drivers(solar_os_shell_io_t *term)
{
    bool any = false;

    solar_os_shell_io_write(term, "Probe drivers:");
    for (size_t i = 0; i < solar_os_expansion_driver_count(); i++) {
        solar_os_expansion_driver_t driver;
        if (!solar_os_expansion_get_driver(i, &driver) ||
            !driver.probe_supported ||
            !solar_os_expansion_driver_supported(driver.name)) {
            continue;
        }
        solar_os_shell_io_printf(term, " %s", driver.name);
        any = true;
    }
    if (!any) {
        solar_os_shell_io_write(term, " none");
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void expansion_cmd_status(solar_os_shell_io_t *term)
{
    if (!solar_os_expansion_available()) {
        solar_os_shell_io_writeln(term, "expansion: no expansion resources on this board");
        return;
    }
    expansion_print_resources(term);
    expansion_print_devices(term);
    expansion_print_claims(term);
}

static bool binding_store(solar_os_expansion_binding_t *bindings,
                          size_t *binding_count,
                          solar_os_expansion_binding_kind_t kind,
                          const char *role,
                          const char *target,
                          int value,
                          int aux)
{
    if (*binding_count >= SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX) {
        return false;
    }
    solar_os_expansion_binding_t *binding = &bindings[*binding_count];
    *binding = (solar_os_expansion_binding_t) {
        .kind = kind,
        .value = value,
        .aux = aux,
    };
    strlcpy(binding->role, role != NULL ? role : "", sizeof(binding->role));
    strlcpy(binding->target, target != NULL ? target : "", sizeof(binding->target));
    (*binding_count)++;
    return true;
}

static bool parse_binding_token(const char *arg,
                                solar_os_expansion_binding_t *bindings,
                                size_t *binding_count)
{
    char key[16];
    const char *value = NULL;
    const char *eq = strchr(arg, '=');

    if (eq == NULL) {
        if (solar_os_expansion_find_i2c_bus(arg, NULL, NULL)) {
            return binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_I2C_BUS, "", arg, -1, -1);
        }
        if (solar_os_expansion_find_spi_bus(arg, NULL, NULL)) {
            return binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_SPI_BUS, "", arg, -1, -1);
        }
        if (solar_os_expansion_find_uart_port(arg, NULL, NULL)) {
            solar_os_expansion_uart_port_t port;
            (void)solar_os_expansion_find_uart_port(arg, &port, NULL);
            return binding_store(bindings,
                                 binding_count,
                                 SOLAR_OS_EXPANSION_BINDING_UART_PORT,
                                 "",
                                 arg,
                                 port.port,
                                 -1);
        }
        return false;
    }

    const size_t key_len = (size_t)(eq - arg);
    if (key_len == 0 || key_len >= sizeof(key)) {
        return false;
    }
    memcpy(key, arg, key_len);
    key[key_len] = '\0';
    value = eq + 1;

    if (strcmp(key, "i2c") == 0) {
        return solar_os_expansion_find_i2c_bus(value, NULL, NULL) &&
            binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_I2C_BUS, "", value, -1, -1);
    }
    if (strcmp(key, "spi") == 0) {
        return solar_os_expansion_find_spi_bus(value, NULL, NULL) &&
            binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_SPI_BUS, "", value, -1, -1);
    }
    if (strcmp(key, "uart") == 0) {
        solar_os_expansion_uart_port_t port;
        return solar_os_expansion_find_uart_port(value, &port, NULL) &&
            binding_store(bindings,
                          binding_count,
                          SOLAR_OS_EXPANSION_BINDING_UART_PORT,
                          "",
                          value,
                          port.port,
                          -1);
    }
    if (strcmp(key, "addr") == 0) {
        int address = 0;
        return parse_int_arg(value, 0x03, 0x77, &address) &&
            binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, "", "", address, -1);
    }

    int pin = -1;
    if (!parse_int_arg(value, 0, 63, &pin)) {
        return false;
    }
    if (strcmp(key, "cs") == 0 || strcmp(key, "ce") == 0) {
        char spi_target[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
        for (size_t i = 0; i < *binding_count; i++) {
            if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) {
                strlcpy(spi_target, bindings[i].target, sizeof(spi_target));
                break;
            }
        }
        return binding_store(bindings,
                             binding_count,
                             SOLAR_OS_EXPANSION_BINDING_SPI_CS,
                             "cs",
                             spi_target,
                             pin,
                             -1);
    }
    if (strcmp(key, "adc") == 0) {
        return binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_ADC, "adc", "", pin, -1);
    }
    if (strcmp(key, "pwm") == 0) {
        return binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_PWM, "pwm", "", pin, -1);
    }
    if (strcmp(key, "gpio") == 0 ||
        strcmp(key, "irq") == 0 ||
        strcmp(key, "reset") == 0 ||
        strcmp(key, "rst") == 0 ||
        strcmp(key, "dc") == 0 ||
        strcmp(key, "busy") == 0) {
        const char *role = strcmp(key, "rst") == 0 ? "reset" : key;
        return binding_store(bindings, binding_count, SOLAR_OS_EXPANSION_BINDING_GPIO, role, "", pin, -1);
    }

    return false;
}

static void expansion_print_attach_error(solar_os_shell_io_t *term, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_SUPPORTED:
        solar_os_shell_io_writeln(term, "expansion: no expansion resources on this board");
        break;
    case ESP_ERR_NOT_FOUND:
        solar_os_shell_io_writeln(term, "expansion attach: unknown or unsupported driver");
        break;
    case ESP_ERR_INVALID_STATE:
        solar_os_shell_io_writeln(term, "expansion attach: device name or resource already in use");
        break;
    case ESP_ERR_INVALID_ARG:
        solar_os_shell_io_writeln(term, "expansion attach: invalid resource for this board");
        break;
    case ESP_ERR_NO_MEM:
        solar_os_shell_io_writeln(term, "expansion attach: no free device or resource slots");
        break;
    case ESP_ERR_INVALID_RESPONSE:
        solar_os_shell_io_writeln(term, "expansion attach: device probe failed");
        break;
    default:
        solar_os_shell_io_printf(term, "expansion attach failed: %s\n", esp_err_to_name(err));
        break;
    }
}

static void expansion_cmd_attach(solar_os_shell_io_t *term, int argc, char **argv)
{
    solar_os_expansion_binding_t bindings[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    size_t binding_count = 0;

    if (argc < 5) {
        solar_os_shell_io_writeln(term, "usage: expansion attach <driver> <name> <resource...>");
        return;
    }

    for (int i = 4; i < argc; i++) {
        if (!parse_binding_token(argv[i], bindings, &binding_count)) {
            solar_os_shell_io_printf(term, "expansion attach: invalid resource '%s'\n", argv[i]);
            return;
        }
    }

    const esp_err_t err = solar_os_expansion_attach(argv[2], argv[3], bindings, binding_count);
    if (err != ESP_OK) {
        expansion_print_attach_error(term, err);
        return;
    }
    solar_os_shell_io_printf(term, "attached %s using %s\n", argv[3], argv[2]);
}

static void expansion_cmd_detach(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: expansion detach <name>");
        return;
    }

    const esp_err_t err = solar_os_expansion_detach(argv[2]);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(term, "detached %s\n", argv[2]);
    } else if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "expansion detach: %s not found\n", argv[2]);
    } else if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_printf(term, "expansion detach: %s is busy\n", argv[2]);
    } else {
        solar_os_shell_io_printf(term, "expansion detach failed: %s\n", esp_err_to_name(err));
    }
}

static bool parse_spi_host(const char *text, int *host)
{
    if (text == NULL || host == NULL) {
        return false;
    }
    if (strcmp(text, "spi2") == 0) {
        *host = SPI2_HOST;
        return true;
    }
    if (strcmp(text, "spi3") == 0) {
        *host = SPI3_HOST;
        return true;
    }
    return false;
}

static void expansion_print_bus_error(solar_os_shell_io_t *term,
                                      const char *operation,
                                      esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_SUPPORTED:
        solar_os_shell_io_printf(term, "expansion bus %s: protocol is not runtime-routable yet\n", operation);
        break;
    case ESP_ERR_NOT_FOUND:
        solar_os_shell_io_printf(term, "expansion bus %s: bus not found\n", operation);
        break;
    case ESP_ERR_NOT_ALLOWED:
        solar_os_shell_io_printf(term, "expansion bus %s: board-defined buses cannot be removed\n", operation);
        break;
    case ESP_ERR_INVALID_STATE:
        solar_os_shell_io_printf(term, "expansion bus %s: name, host, or pin is already in use\n", operation);
        break;
    case ESP_ERR_INVALID_ARG:
        solar_os_shell_io_printf(term, "expansion bus %s: invalid host or expansion pin assignment\n", operation);
        break;
    case ESP_ERR_NO_MEM:
        solar_os_shell_io_printf(term, "expansion bus %s: no free bus or resource slots\n", operation);
        break;
    default:
        solar_os_shell_io_printf(term,
                                 "expansion bus %s failed: %s\n",
                                 operation,
                                 esp_err_to_name(err));
        break;
    }
}

static void expansion_cmd_bus_create_spi(solar_os_shell_io_t *term,
                                         int argc,
                                         char **argv)
{
    if (argc < 6) {
        expansion_print_usage(term);
        return;
    }

    solar_os_bus_definition_t definition = {
        .name = argv[4],
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = SOLAR_OS_BUS_SHARED,
        .config.spi = {
            .host = -1,
            .sclk_pin = -1,
            .miso_pin = -1,
            .mosi_pin = -1,
            .max_transfer_size = 4096,
        },
    };

    for (int i = 5; i < argc; i++) {
        const char *eq = strchr(argv[i], '=');
        if (eq == NULL || eq == argv[i] || eq[1] == '\0') {
            solar_os_shell_io_printf(term, "expansion bus create: invalid option '%s'\n", argv[i]);
            return;
        }
        const size_t key_len = (size_t)(eq - argv[i]);
        const char *value = eq + 1;
        int parsed = -1;

        if (key_len == 4 && strncmp(argv[i], "host", key_len) == 0) {
            if (!parse_spi_host(value, &definition.config.spi.host)) {
                expansion_print_bus_error(term, "create", ESP_ERR_INVALID_ARG);
                return;
            }
        } else if (key_len == 4 && strncmp(argv[i], "sclk", key_len) == 0) {
            if (!parse_int_arg(value, 0, 63, &definition.config.spi.sclk_pin)) {
                expansion_print_bus_error(term, "create", ESP_ERR_INVALID_ARG);
                return;
            }
        } else if (key_len == 4 && strncmp(argv[i], "mosi", key_len) == 0) {
            if (!parse_int_arg(value, 0, 63, &definition.config.spi.mosi_pin)) {
                expansion_print_bus_error(term, "create", ESP_ERR_INVALID_ARG);
                return;
            }
        } else if (key_len == 4 && strncmp(argv[i], "miso", key_len) == 0) {
            if (strcmp(value, "none") != 0 &&
                !parse_int_arg(value, 0, 63, &definition.config.spi.miso_pin)) {
                expansion_print_bus_error(term, "create", ESP_ERR_INVALID_ARG);
                return;
            }
        } else if (key_len == 2 && strncmp(argv[i], "cs", key_len) == 0) {
            if (definition.config.spi.cs_count >= SOLAR_OS_BUS_SPI_CS_MAX ||
                !parse_int_arg(value, 0, 63, &parsed)) {
                expansion_print_bus_error(term, "create", ESP_ERR_INVALID_ARG);
                return;
            }
            solar_os_bus_pin_t *cs =
                &definition.config.spi.cs[definition.config.spi.cs_count++];
            cs->pin = parsed;
            (void)snprintf(cs->name, sizeof(cs->name), "gpio%d", parsed);
        } else if (key_len == 3 && strncmp(argv[i], "max", key_len) == 0) {
            if (!parse_int_arg(value, 1, 65536, &parsed)) {
                expansion_print_bus_error(term, "create", ESP_ERR_INVALID_ARG);
                return;
            }
            definition.config.spi.max_transfer_size = (uint32_t)parsed;
        } else {
            solar_os_shell_io_printf(term, "expansion bus create: unknown option '%s'\n", argv[i]);
            return;
        }
    }

    const esp_err_t err = solar_os_bus_register(&definition);
    if (err != ESP_OK) {
        expansion_print_bus_error(term, "create", err);
        return;
    }
    solar_os_shell_io_printf(term,
                             "created SPI bus %s on %s (idle until first device attaches)\n",
                             definition.name,
                             spi_host_name(definition.config.spi.host));
}

static void expansion_cmd_bus(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc >= 5 && strcmp(argv[2], "create") == 0 && strcmp(argv[3], "spi") == 0) {
        expansion_cmd_bus_create_spi(term, argc, argv);
        return;
    }
    if (argc == 4 && strcmp(argv[2], "remove") == 0) {
        const esp_err_t err = solar_os_bus_unregister(argv[3]);
        if (err != ESP_OK) {
            expansion_print_bus_error(term, "remove", err);
            return;
        }
        solar_os_shell_io_printf(term, "removed bus %s\n", argv[3]);
        return;
    }
    expansion_print_usage(term);
}

void solar_os_shell_cmd_expansion(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        expansion_cmd_status(term);
        return;
    }
    if (strcmp(argv[1], "scan") == 0) {
        expansion_print_resources(term);
        expansion_print_probe_drivers(term);
        return;
    }
    if (strcmp(argv[1], "drivers") == 0) {
        expansion_print_drivers(term);
        return;
    }
    if (strcmp(argv[1], "devices") == 0) {
        expansion_print_devices(term);
        return;
    }
    if (strcmp(argv[1], "bus") == 0) {
        expansion_cmd_bus(term, argc, argv);
        return;
    }
    if (strcmp(argv[1], "attach") == 0) {
        expansion_cmd_attach(term, argc, argv);
        return;
    }
    if (strcmp(argv[1], "detach") == 0) {
        expansion_cmd_detach(term, argc, argv);
        return;
    }

    expansion_print_usage(term);
}
