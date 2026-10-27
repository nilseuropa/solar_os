#pragma once

#include "drv8830.h"
#include "solar_os_expansion.h"

typedef struct {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int interrupt_gpio; /* -1 when FAULTn is not wired. */
} solar_os_drv8830_info_t;

typedef struct {
    solar_os_drv8830_chip_status_t chip;
    bool interrupt_present;
    bool fault_asserted; /* Active-low FAULTn, sampled when status is read. */
} solar_os_drv8830_status_t;

/* Expansion registry owns the bus lease, address and optional GPIO claims.
 * Attach/detach callbacks are called by that registry, not directly by apps. */
esp_err_t solar_os_drv8830_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings, size_t count);
esp_err_t solar_os_drv8830_detach(const char *name);
bool solar_os_drv8830_get(size_t index, solar_os_drv8830_info_t *info);
esp_err_t solar_os_drv8830_status(const char *name, solar_os_drv8830_status_t *status);
esp_err_t solar_os_drv8830_drive(const char *name, int voltage_mv);
esp_err_t solar_os_drv8830_stop(const char *name, bool brake);
esp_err_t solar_os_drv8830_clear(const char *name);
