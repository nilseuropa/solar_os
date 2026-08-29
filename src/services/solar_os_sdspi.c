#include "solar_os_sdspi.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "sd_card.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
} solar_os_sdspi_device_t;

static solar_os_sdspi_device_t sdspi;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin)
{
    bool have_spi = false;
    bool have_cs = false;

    if (bindings == NULL || spi_bus == NULL || spi_bus_len == 0 || cs_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_bus[0] = '\0';
    *cs_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) {
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(spi_bus, binding->target, spi_bus_len);
            have_spi = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS) {
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            *cs_pin = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(spi_bus, binding->target, spi_bus_len);
                have_spi = true;
            }
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_spi || !have_cs ||
        !solar_os_expansion_spi_cs_allowed(spi_bus, *cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t solar_os_sdspi_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin = -1;
    solar_os_expansion_spi_bus_t bus;

    if (name == NULL || name[0] == '\0' || sdspi.active) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = parse_bindings(bindings,
                                   binding_count,
                                   spi_bus,
                                   sizeof(spi_bus),
                                   &cs_pin);
    if (ret != ESP_OK || !solar_os_expansion_find_spi_bus(spi_bus, &bus, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = sd_card_configure_sdspi(bus.host, cs_pin);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = sd_card_init();
    if (ret != ESP_OK) {
        (void)sd_card_unmount();
        (void)sd_card_clear_sdspi_config();
        return ret;
    }

    memset(&sdspi, 0, sizeof(sdspi));
    sdspi.active = true;
    sdspi.cs_pin = cs_pin;
    strlcpy(sdspi.name, name, sizeof(sdspi.name));
    strlcpy(sdspi.spi_bus, spi_bus, sizeof(sdspi.spi_bus));
    return ESP_OK;
}

esp_err_t solar_os_sdspi_detach(const char *name)
{
    if (!sdspi.active || name == NULL || strcmp(sdspi.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (sd_card_has_mounts()) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)sd_card_unmount();
    const esp_err_t ret = sd_card_clear_sdspi_config();
    if (ret == ESP_OK) {
        memset(&sdspi, 0, sizeof(sdspi));
    }
    return ret;
}

static size_t diagnostics_append(char *buffer, size_t len, size_t used, const char *format, ...)
{
    if (buffer == NULL || len == 0 || used >= len - 1 || format == NULL) {
        return used;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer + used, len - used, format, args);
    va_end(args);
    if (written < 0) {
        return used;
    }
    const size_t available = len - used;
    return (size_t)written >= available ? len - 1 : used + (size_t)written;
}

static const char *diagnostics_card_type(sd_card_kind_t kind)
{
    switch (kind) {
    case SD_CARD_KIND_SDHC: return "SDHC";
    case SD_CARD_KIND_SDHC_UHS1: return "SDHC/SDXC (UHS-I)";
    case SD_CARD_KIND_SDIO: return "SDIO";
    case SD_CARD_KIND_MMC: return "MMC";
    case SD_CARD_KIND_SDSC:
    default: return "SDSC";
    }
}

static const char *diagnostics_fresult_name(int result)
{
    switch ((FRESULT)result) {
    case FR_OK: return "FR_OK";
    case FR_DISK_ERR: return "FR_DISK_ERR";
    case FR_INT_ERR: return "FR_INT_ERR";
    case FR_NOT_READY: return "FR_NOT_READY";
    case FR_NO_FILE: return "FR_NO_FILE";
    case FR_NO_PATH: return "FR_NO_PATH";
    case FR_INVALID_NAME: return "FR_INVALID_NAME";
    case FR_DENIED: return "FR_DENIED";
    case FR_EXIST: return "FR_EXIST";
    case FR_INVALID_OBJECT: return "FR_INVALID_OBJECT";
    case FR_WRITE_PROTECTED: return "FR_WRITE_PROTECTED";
    case FR_INVALID_DRIVE: return "FR_INVALID_DRIVE";
    case FR_NOT_ENABLED: return "FR_NOT_ENABLED";
    case FR_NO_FILESYSTEM: return "FR_NO_FILESYSTEM";
    case FR_MKFS_ABORTED: return "FR_MKFS_ABORTED";
    case FR_TIMEOUT: return "FR_TIMEOUT";
    case FR_LOCKED: return "FR_LOCKED";
    case FR_NOT_ENOUGH_CORE: return "FR_NOT_ENOUGH_CORE";
    case FR_TOO_MANY_OPEN_FILES: return "FR_TOO_MANY_OPEN_FILES";
    case FR_INVALID_PARAMETER: return "FR_INVALID_PARAMETER";
    default: return "FR_UNKNOWN";
    }
}

size_t solar_os_sdspi_format_last_diagnostics(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return 0;
    }
    buffer[0] = '\0';
    sd_card_diagnostics_t diagnostics;
    if (!sd_card_get_last_diagnostics(&diagnostics)) {
        return 0;
    }

    size_t used = diagnostics_append(buffer, len, 0, "SD-SPI probe:\n");
    if (!diagnostics.card_initialized) {
        return diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  Card initialization: %s (0x%x)\n",
                                  esp_err_to_name(diagnostics.init_error),
                                  (unsigned)diagnostics.init_error);
    }

    if (diagnostics.kind != SD_CARD_KIND_SDIO && diagnostics.kind != SD_CARD_KIND_MMC) {
        used = diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  SDIO: not present (CMD52/CMD5 unsupported)\n");
    }
    used = diagnostics_append(buffer, len, used, "  Name: %s\n", diagnostics.name);
    used = diagnostics_append(buffer,
                              len,
                              used,
                              "  Type: %s\n",
                              diagnostics_card_type(diagnostics.kind));
    if (diagnostics.real_freq_khz == 0) {
        used = diagnostics_append(buffer, len, used, "  Speed: N/A\n");
    } else {
        used = diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  Speed: %u.%02u MHz (limit: %u.%02u MHz)%s\n",
                                  (unsigned)diagnostics.real_freq_khz / 1000U,
                                  ((unsigned)diagnostics.real_freq_khz % 1000U) / 10U,
                                  (unsigned)diagnostics.max_freq_khz / 1000U,
                                  ((unsigned)diagnostics.max_freq_khz % 1000U) / 10U,
                                  diagnostics.ddr ? ", DDR" : "");
    }
    used = diagnostics_append(buffer,
                              len,
                              used,
                              "  Size: %" PRIu64 "MB\n",
                              diagnostics.size_bytes / (1024ULL * 1024ULL));
    used = diagnostics_append(buffer,
                              len,
                              used,
                              "  CSD: ver=%d, sector_size=%d, capacity=%d read_bl_len=%d\n",
                              diagnostics.csd_version,
                              diagnostics.sector_size,
                              diagnostics.capacity_sectors,
                              diagnostics.read_block_len);
    if (diagnostics.kind == SD_CARD_KIND_MMC) {
        used = diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  EXT CSD: bus_width=%u\n",
                                  (unsigned)diagnostics.bus_width);
    } else if (diagnostics.kind != SD_CARD_KIND_SDIO) {
        used = diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  SSR: bus_width=%u\n",
                                  (unsigned)diagnostics.bus_width);
    }
    if (diagnostics.diskio_error != ESP_OK) {
        used = diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  Block I/O: %s failed: %s (0x%x)\n",
                                  diagnostics.diskio_operation[0] != '\0' ?
                                      diagnostics.diskio_operation : "operation",
                                  esp_err_to_name(diagnostics.diskio_error),
                                  (unsigned)diagnostics.diskio_error);
    }
    if (diagnostics.fatfs_result != FR_OK) {
        used = diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  FatFs: %s (%d)\n",
                                  diagnostics_fresult_name(diagnostics.fatfs_result),
                                  diagnostics.fatfs_result);
    }
    if (diagnostics.mount_error == ESP_OK) {
        used = diagnostics_append(buffer, len, used, "  Mount: %s\n", SD_CARD_MOUNT_POINT);
    } else {
        used = diagnostics_append(buffer,
                                  len,
                                  used,
                                  "  Mount: failed: %s (0x%x)\n",
                                  esp_err_to_name(diagnostics.mount_error),
                                  (unsigned)diagnostics.mount_error);
    }
    return used;
}
