#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_MOTOR_NAME_MAX 20
#define SOLAR_OS_MOTOR_DRIVER_MAX 32

typedef enum {
    SOLAR_OS_MOTOR_MODE_UNKNOWN,
    SOLAR_OS_MOTOR_MODE_COAST,
    SOLAR_OS_MOTOR_MODE_FORWARD,
    SOLAR_OS_MOTOR_MODE_REVERSE,
    SOLAR_OS_MOTOR_MODE_BRAKE,
} solar_os_motor_mode_t;

enum {
    SOLAR_OS_MOTOR_FAULT_UNKNOWN = 1U << 0,
    SOLAR_OS_MOTOR_FAULT_OVERCURRENT = 1U << 1,
    SOLAR_OS_MOTOR_FAULT_UNDERVOLTAGE = 1U << 2,
    SOLAR_OS_MOTOR_FAULT_OVERTEMPERATURE = 1U << 3,
    SOLAR_OS_MOTOR_FAULT_CURRENT_LIMIT = 1U << 4,
};

typedef struct {
    char name[SOLAR_OS_MOTOR_NAME_MAX];
    char driver[SOLAR_OS_MOTOR_DRIVER_MAX];
    bool can_drive;
    bool can_brake;
    bool can_clear;
} solar_os_motor_info_t;

typedef struct {
    solar_os_motor_info_t info;
    solar_os_motor_mode_t mode;
    bool drive_valid;
    float drive; /* Applied normalized command, not measured speed or torque. */
    bool voltage_valid;
    uint32_t voltage_mv; /* Nominal magnitude; direction is in mode. */
    uint32_t faults; /* Normalized present or recorded conditions. */
    uint32_t blocking_faults; /* Conditions that currently prevent drive. */
    bool fault_input_present;
    bool fault_input_asserted;
    bool raw_valid;
    uint32_t raw_control; /* Optional, driver-specific diagnostics. */
    uint32_t raw_fault;
} solar_os_motor_status_t;

typedef struct {
    esp_err_t (*status)(void *ctx, solar_os_motor_status_t *status);
    esp_err_t (*drive)(void *ctx, float value); /* Optional, normalized -1..1. */
    esp_err_t (*coast)(void *ctx);
    esp_err_t (*brake)(void *ctx); /* Optional. */
    esp_err_t (*clear)(void *ctx); /* Optional; leave the motor coasting. */
    /* Optional teardown, called during unregister instead of coast. On
     * failure the provider and its context remain registered for retry. */
    esp_err_t (*shutdown)(void *ctx);
} solar_os_motor_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const solar_os_motor_ops_t *ops;
    void *ctx;
} solar_os_motor_registration_t;

/* Register one named motor endpoint per channel. Callbacks are serialized per
 * endpoint and run outside the registry lock. Do not re-enter operations on
 * the same endpoint. The service does not own the provider context. */
esp_err_t solar_os_motor_register(const solar_os_motor_registration_t *registration);
/* Rejects unregister while operations are in flight; successful unregister
 * stops the motor before releasing the context back to its driver. */
esp_err_t solar_os_motor_unregister(const char *name);
size_t solar_os_motor_count(void);
bool solar_os_motor_get(size_t index, solar_os_motor_info_t *info);
esp_err_t solar_os_motor_status(const char *name, solar_os_motor_status_t *status);
/* Finite -1..1 command: sign selects direction; zero always means coast.
 * Providers map demand to hardware and enforce fault/recovery policy. */
esp_err_t solar_os_motor_drive(const char *name, float value);
esp_err_t solar_os_motor_coast(const char *name);
esp_err_t solar_os_motor_brake(const char *name);
esp_err_t solar_os_motor_clear(const char *name);
const char *solar_os_motor_mode_name(solar_os_motor_mode_t mode);
