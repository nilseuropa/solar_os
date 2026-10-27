#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_DS3231_ADDRESS 0x68U
#define SOLAR_OS_DS3231_BUS_NAME_MAX 16

typedef struct {
    char bus[SOLAR_OS_DS3231_BUS_NAME_MAX];
    uint8_t address;
    bool clock_integrity_lost;
} solar_os_ds3231_t;

typedef struct {
    uint16_t year;
    uint8_t month, day, weekday, hour, minute, second;
    bool clock_integrity;
} solar_os_ds3231_datetime_t;

typedef enum {
    SOLAR_OS_DS3231_ALARM_MATCH_SECOND = 1U << 0,
    SOLAR_OS_DS3231_ALARM_MATCH_MINUTE = 1U << 1,
    SOLAR_OS_DS3231_ALARM_MATCH_HOUR = 1U << 2,
    SOLAR_OS_DS3231_ALARM_MATCH_DAY = 1U << 3,
    SOLAR_OS_DS3231_ALARM_MATCH_WEEKDAY = 1U << 4,
} solar_os_ds3231_alarm_match_t;

typedef struct {
    uint32_t match_fields;
    uint8_t second, minute, hour, day, weekday;
} solar_os_ds3231_alarm_t;

#define SOLAR_OS_DS3231_INTERRUPT_ALARM (1U << 0)

/* Calendar convention: 2000..2099, century=0, Sunday=1 on the chip.
 * API weekday values use Sunday=0; writes calculate weekday from the date.
 * Alarm 1 supplies the single OS alarm slot; Alarm 2 interrupts are disabled. */
esp_err_t solar_os_ds3231_init_device(solar_os_ds3231_t *device, const char *bus, uint8_t address);
esp_err_t solar_os_ds3231_get_datetime_device(const solar_os_ds3231_t *device,
                                             solar_os_ds3231_datetime_t *datetime);
esp_err_t solar_os_ds3231_set_datetime_device(solar_os_ds3231_t *device,
                                             const solar_os_ds3231_datetime_t *datetime);
esp_err_t solar_os_ds3231_set_alarm_device(const solar_os_ds3231_t *device,
                                          const solar_os_ds3231_alarm_t *alarm);
esp_err_t solar_os_ds3231_disable_alarm_device(const solar_os_ds3231_t *device);
esp_err_t solar_os_ds3231_get_interrupt_status_device(const solar_os_ds3231_t *device,
                                                     uint32_t *interrupts);
esp_err_t solar_os_ds3231_clear_interrupt_status_device(const solar_os_ds3231_t *device,
                                                       uint32_t interrupts);
