#include "solar_os_shell_expansion_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_board.h"
#include "solar_os_buses.h"
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

static const solar_os_expansion_driver_t *find_driver(
    const char *name,
    solar_os_expansion_driver_t *snapshot)
{
    if (name == NULL || snapshot == NULL) {
        return NULL;
    }
    for (size_t i = 0U; i < solar_os_expansion_driver_count(); i++) {
        if (solar_os_expansion_get_driver(i, snapshot) &&
            strcmp(snapshot->name, name) == 0) {
            return snapshot;
        }
    }
    return NULL;
}

static esp_err_t write_string(FILE *file, const char *value)
{
    if (file == NULL || value == NULL || fputc('"', file) == EOF) {
        return ESP_FAIL;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; cursor++) {
        const char *escape = NULL;
        switch (*cursor) {
        case '\b': escape = "\\b"; break;
        case '\t': escape = "\\t"; break;
        case '\n': escape = "\\n"; break;
        case '\f': escape = "\\f"; break;
        case '\r': escape = "\\r"; break;
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        default:
            if (iscntrl(*cursor) || fputc(*cursor, file) == EOF) {
                return ESP_FAIL;
            }
            continue;
        }
        if (fputs(escape, file) == EOF) {
            return ESP_FAIL;
        }
    }
    return fputc('"', file) == EOF ? ESP_FAIL : ESP_OK;
}

static bool supported_bus_protocol(solar_os_bus_protocol_t protocol)
{
    return protocol == SOLAR_OS_BUS_PROTOCOL_I2C ||
           protocol == SOLAR_OS_BUS_PROTOCOL_SPI ||
           protocol == SOLAR_OS_BUS_PROTOCOL_UART ||
           protocol == SOLAR_OS_BUS_PROTOCOL_ONEWIRE ||
           protocol == SOLAR_OS_BUS_PROTOCOL_PS2 ||
           protocol == SOLAR_OS_BUS_PROTOCOL_MIDI;
}

static esp_err_t preflight(size_t *runtime_bus_count,
                           size_t *runtime_device_count,
                           char *unsupported_device,
                           size_t unsupported_device_len)
{
    *runtime_bus_count = 0U;
    *runtime_device_count = 0U;
    if (unsupported_device != NULL && unsupported_device_len > 0U) {
        unsupported_device[0] = '\0';
    }

    for (size_t i = 0U; i < solar_os_bus_count(); i++) {
        solar_os_bus_info_t bus = {0};
        if (!solar_os_bus_get(i, &bus)) {
            return ESP_FAIL;
        }
        if (bus.origin != SOLAR_OS_BUS_ORIGIN_RUNTIME) {
            continue;
        }
        if (!supported_bus_protocol(bus.protocol)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        (*runtime_bus_count)++;
    }

    for (size_t i = 0U; i < solar_os_expansion_device_count(); i++) {
        solar_os_expansion_device_t device = {0};
        if (!solar_os_expansion_get_device(i, &device)) {
            return ESP_FAIL;
        }
        if (device.origin != SOLAR_OS_EXPANSION_ORIGIN_RUNTIME) {
            continue;
        }
        solar_os_expansion_driver_t driver = {0};
        if (strcmp(device.driver, "manual") == 0 ||
            find_driver(device.driver, &driver) == NULL) {
            if (unsupported_device != NULL && unsupported_device_len > 0U) {
                strlcpy(unsupported_device, device.name, unsupported_device_len);
            }
            return ESP_ERR_NOT_SUPPORTED;
        }
        for (size_t binding = 0U; binding < device.binding_count; binding++) {
            char key[SOLAR_OS_EXPANSION_ROLE_MAX + 5U];
            char value[SOLAR_OS_EXPANSION_TARGET_MAX + 16U];
            bool string_value = false;
            const esp_err_t err = solar_os_shell_expansion_binding_manifest_field(
                &driver,
                &device.bindings[binding],
                key,
                sizeof(key),
                value,
                sizeof(value),
                &string_value);
            if (err != ESP_OK) {
                if (unsupported_device != NULL && unsupported_device_len > 0U) {
                    strlcpy(unsupported_device, device.name, unsupported_device_len);
                }
                return err;
            }
        }
        (*runtime_device_count)++;
    }
    return ESP_OK;
}

static esp_err_t write_bus(FILE *file, const solar_os_bus_info_t *bus)
{
    if (fputs("\n[[buses]]\nname = ", file) == EOF ||
        write_string(file, bus->name) != ESP_OK ||
        fputs("\nprotocol = ", file) == EOF ||
        write_string(file, solar_os_bus_protocol_name(bus->protocol)) != ESP_OK ||
        fputs("\nsharing = ", file) == EOF ||
        write_string(file, solar_os_bus_sharing_name(bus->sharing)) != ESP_OK ||
        fputc('\n', file) == EOF) {
        return ESP_FAIL;
    }

    int written = -1;
    switch (bus->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        written = fprintf(file,
                          "port = \"I2C_NUM_%d\"\n"
                          "sda = %d\n"
                          "scl = %d\n"
                          "speed_hz = %lu\n",
                          bus->config.i2c.port,
                          bus->config.i2c.sda_pin,
                          bus->config.i2c.scl_pin,
                          (unsigned long)bus->config.i2c.speed_hz);
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        written = fprintf(file,
                          "host = \"SPI%d_HOST\"\n"
                          "sclk = %d\n",
                          bus->config.spi.host + 1,
                          bus->config.spi.sclk_pin);
        if (written >= 0 && bus->config.spi.mosi_pin >= 0) {
            written = fprintf(file, "mosi = %d\n", bus->config.spi.mosi_pin);
        }
        if (written >= 0 && bus->config.spi.miso_pin >= 0) {
            written = fprintf(file, "miso = %d\n", bus->config.spi.miso_pin);
        }
        if (written >= 0) {
            written = fputs("cs = [", file);
        }
        for (size_t i = 0U; written >= 0 && i < bus->config.spi.cs_count; i++) {
            written = fprintf(file,
                              i == 0U ? "%d" : ", %d",
                              bus->config.spi.cs[i].pin);
        }
        if (written >= 0) {
            written = fprintf(file,
                              "]\nmax_transfer_size = %lu\n",
                              (unsigned long)bus->config.spi.max_transfer_size);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        written = fprintf(file,
                          "port = \"UART_NUM_%d\"\n"
                          "tx = %d\n"
                          "rx = %d\n"
                          "baud_rate = %lu\n",
                          bus->config.uart.port,
                          bus->config.uart.tx_pin,
                          bus->config.uart.rx_pin,
                          (unsigned long)bus->config.uart.baud_rate);
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        written = fprintf(file, "pin = %d\n", bus->config.onewire.pin);
        break;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        written = fprintf(file,
                          "clock = %d\n"
                          "data = %d\n",
                          bus->config.ps2.clock_pin,
                          bus->config.ps2.data_pin);
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    return written < 0 ? ESP_FAIL : ESP_OK;
}

static esp_err_t write_device(FILE *file,
                              const solar_os_expansion_driver_t *driver,
                              const solar_os_expansion_device_t *device)
{
    if (fputs("\n[[devices]]\ndriver = ", file) == EOF ||
        write_string(file, device->driver) != ESP_OK ||
        fputs("\nname = ", file) == EOF ||
        write_string(file, device->name) != ESP_OK ||
        fputs("\nbindings = { ", file) == EOF) {
        return ESP_FAIL;
    }
    for (size_t i = 0U; i < device->binding_count; i++) {
        char key[SOLAR_OS_EXPANSION_ROLE_MAX + 5U];
        char value[SOLAR_OS_EXPANSION_TARGET_MAX + 16U];
        bool string_value = false;
        esp_err_t err = solar_os_shell_expansion_binding_manifest_field(
            driver,
            &device->bindings[i],
            key,
            sizeof(key),
            value,
            sizeof(value),
            &string_value);
        if (err != ESP_OK ||
            (i > 0U && fputs(", ", file) == EOF) ||
            write_string(file, key) != ESP_OK ||
            fputs(" = ", file) == EOF) {
            return err == ESP_OK ? ESP_FAIL : err;
        }
        if (string_value) {
            err = write_string(file, value);
        } else if (fputs(value, file) == EOF) {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return fputs(" }\n", file) == EOF ? ESP_FAIL : ESP_OK;
}

esp_err_t solar_os_shell_expansion_export_manifest(
    const char *path,
    size_t *bus_count,
    size_t *device_count,
    char *unsupported_device,
    size_t unsupported_device_len)
{
    if (path == NULL || path[0] == '\0' || bus_count == NULL ||
        device_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = preflight(bus_count,
                              device_count,
                              unsupported_device,
                              unsupported_device_len);
    if (err != ESP_OK) {
        return err;
    }

    char staged[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    err = solar_os_storage_sibling_path(path, ".tmp", staged, sizeof(staged));
    if (err == ESP_OK) {
        err = solar_os_storage_sibling_path(path, ".bak", backup, sizeof(backup));
    }
    if (err != ESP_OK) {
        return err;
    }
    FILE *file = fopen(staged, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    if (fprintf(file,
                "schema = 1\n"
                "kind = \"solaros-expansion\"\n\n"
                "[base]\n"
                "board = \"%s\"\n"
                "firmware = \"%s\"\n",
                SOLAR_OS_BOARD_ID,
                SOLAR_OS_VERSION) < 0) {
        err = ESP_FAIL;
    }
    for (size_t i = 0U; err == ESP_OK && i < solar_os_bus_count(); i++) {
        solar_os_bus_info_t bus = {0};
        if (!solar_os_bus_get(i, &bus)) {
            err = ESP_FAIL;
        } else if (bus.origin == SOLAR_OS_BUS_ORIGIN_RUNTIME) {
            err = write_bus(file, &bus);
        }
    }
    for (size_t i = 0U; err == ESP_OK && i < solar_os_expansion_device_count(); i++) {
        solar_os_expansion_device_t device = {0};
        solar_os_expansion_driver_t driver = {0};
        if (!solar_os_expansion_get_device(i, &device)) {
            err = ESP_FAIL;
        } else if (device.origin == SOLAR_OS_EXPANSION_ORIGIN_RUNTIME) {
            if (find_driver(device.driver, &driver) == NULL) {
                err = ESP_ERR_NOT_SUPPORTED;
            } else {
                err = write_device(file, &driver, &device);
            }
        }
    }
    if (err == ESP_OK) {
        err = solar_os_storage_sync_file(file);
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(staged, path, backup);
    } else {
        (void)solar_os_storage_remove(staged);
    }
    return err;
}
