#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SOLAR_OS_DRV8830_COAST = 0,
    SOLAR_OS_DRV8830_FORWARD = 1,
    SOLAR_OS_DRV8830_REVERSE = 2,
    SOLAR_OS_DRV8830_BRAKE = 3,
} solar_os_drv8830_mode_t;

#define SOLAR_OS_DRV8830_FAULT 0x01U
#define SOLAR_OS_DRV8830_OCP 0x02U
#define SOLAR_OS_DRV8830_UVLO 0x04U
#define SOLAR_OS_DRV8830_OTS 0x08U
#define SOLAR_OS_DRV8830_ILIMIT 0x10U
/* FAULT reports an active condition. OCP needs explicit recovery, and
 * extended current limiting requires acknowledgement. UVLO/OTS source bits
 * alone can remain as history after the chip has automatically recovered. */
#define SOLAR_OS_DRV8830_DRIVE_FAULT_MASK \
    (SOLAR_OS_DRV8830_FAULT | SOLAR_OS_DRV8830_OCP | SOLAR_OS_DRV8830_ILIMIT)

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} solar_os_drv8830_io_t;

typedef struct {
    solar_os_drv8830_io_t io;
    bool initialized;
} solar_os_drv8830_chip_t;

/* CONTROL is read back from hardware; voltage is the nominal DAC setting,
 * not a measurement. The caller serializes chip operations. */
typedef struct {
    uint8_t control;
    uint8_t fault;
    solar_os_drv8830_mode_t mode;
    uint16_t voltage_mv;
} solar_os_drv8830_chip_status_t;

esp_err_t solar_os_drv8830_chip_init(solar_os_drv8830_chip_t *chip,
                                     const solar_os_drv8830_io_t *io);
esp_err_t solar_os_drv8830_chip_status(solar_os_drv8830_chip_t *chip,
                                       solar_os_drv8830_chip_status_t *status);
/* Signed millivolts: zero coasts, otherwise abs(value) must be 480..5060.
 * Quantized to the nearest supported VSET code (6..63). Recovered UVLO/OTS
 * history remains readable and does not block drive on its own. */
esp_err_t solar_os_drv8830_chip_drive(solar_os_drv8830_chip_t *chip, int voltage_mv);
esp_err_t solar_os_drv8830_chip_stop(solar_os_drv8830_chip_t *chip, bool brake);
/* Coasts before clearing, so fault recovery cannot restart the motor. */
esp_err_t solar_os_drv8830_chip_clear(solar_os_drv8830_chip_t *chip);
