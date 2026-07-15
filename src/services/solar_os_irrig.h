#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_time.h"

/*
 * Irrigation engine: N zones (configurable 1..8, e.g. a 2-zone build
 * for a smaller site), each with 4 schedule slots (start/end time,
 * weekday mask, active flag), driving one relay GPIO per zone.
 *
 * A schedule is "on" when: slot active AND start <= now < end AND
 * today's weekday bit set -- windows do not cross midnight (same
 * semantics as the original standalone controller this was ported
 * from). In AUTO mode a zone's relay follows its schedules; in MANUAL
 * mode it follows the per-zone manual switches instead. When the
 * clock isn't trustworthy (no RTC integrity), everything turns off --
 * watering blind is worse than not watering.
 *
 * Zone relay pins are runtime config (NVS), default unassigned (-1):
 * the whole engine runs as a pure state machine until pins are
 * assigned, so schedules and modes can be exercised safely with no
 * hardware wired. Relays are treated as active-low (on = pin low),
 * matching the usual relay boards.
 *
 * Everything here runs on the main loop (job events and shell
 * commands both dispatch there), so there is no locking.
 */
#define SOLAR_OS_IRRIG_ZONES_MAX 8U
#define SOLAR_OS_IRRIG_ZONES_DEFAULT 4U
#define SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE 4U
#define SOLAR_OS_IRRIG_DAYS_ALL 0x7fU

typedef struct {
    uint16_t start_minute; /* minutes of day, 0..1439 */
    uint16_t end_minute;   /* exclusive */
    uint8_t days;          /* bit0 = Monday ... bit6 = Sunday */
    bool active;
} solar_os_irrig_schedule_t;

typedef enum {
    SOLAR_OS_IRRIG_MODE_AUTO,
    SOLAR_OS_IRRIG_MODE_MANUAL,
} solar_os_irrig_mode_t;

typedef struct {
    bool output_on;       /* what the relay is driven to right now */
    bool schedule_active; /* what the schedules say (regardless of mode) */
    bool manual_on;       /* the manual switch (regardless of mode) */
    int pin;              /* -1 = unassigned */
} solar_os_irrig_zone_status_t;

esp_err_t solar_os_irrig_init(void);

uint8_t solar_os_irrig_zone_count(void);
esp_err_t solar_os_irrig_set_zone_count(uint8_t count);

esp_err_t solar_os_irrig_get_schedule(uint8_t zone,
                                      uint8_t slot,
                                      solar_os_irrig_schedule_t *out);
esp_err_t solar_os_irrig_set_schedule(uint8_t zone,
                                      uint8_t slot,
                                      const solar_os_irrig_schedule_t *schedule);

int solar_os_irrig_zone_pin(uint8_t zone);
esp_err_t solar_os_irrig_set_zone_pin(uint8_t zone, int pin);

solar_os_irrig_mode_t solar_os_irrig_mode(void);
void solar_os_irrig_set_mode(solar_os_irrig_mode_t mode);
esp_err_t solar_os_irrig_set_manual(uint8_t zone, bool on);

esp_err_t solar_os_irrig_zone_status(uint8_t zone, solar_os_irrig_zone_status_t *out);

/*
 * Re-evaluate all zones against the given local time and drive any
 * assigned relay pins. Pass NULL (or an invalid/integrity-less time)
 * to force everything off. Logs zone on/off transitions.
 */
void solar_os_irrig_update(const solar_os_datetime_t *now);
void solar_os_irrig_all_off(void);
