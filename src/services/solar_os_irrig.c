#include "solar_os_irrig.h"

#include <string.h>

#include "driver/gpio.h"
#include "nvs.h"
#include "solar_os_log.h"

#define IRRIG_NVS_NAMESPACE "irrig"
#define IRRIG_NVS_CONFIG_KEY "cfg"
#define IRRIG_CONFIG_MAGIC 0x4952U /* "IR" */
#define IRRIG_CONFIG_VERSION 1U
#define IRRIG_MINUTES_PER_DAY 1440U

typedef struct {
    int8_t pin;
    solar_os_irrig_schedule_t schedules[SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE];
} irrig_zone_config_t;

typedef struct {
    uint16_t magic;
    uint8_t version;
    uint8_t zone_count;
    irrig_zone_config_t zones[SOLAR_OS_IRRIG_ZONES_MAX];
} irrig_config_t;

typedef struct {
    bool initialized;
    irrig_config_t config;
    solar_os_irrig_mode_t mode;
    bool manual_on[SOLAR_OS_IRRIG_ZONES_MAX];
    bool schedule_active[SOLAR_OS_IRRIG_ZONES_MAX];
    bool output_on[SOLAR_OS_IRRIG_ZONES_MAX];
    bool pin_configured[SOLAR_OS_IRRIG_ZONES_MAX];
    bool time_warning_logged;
} irrig_state_t;

static const char *TAG = "solar_os_irrig";
static irrig_state_t irrig;

static void irrig_config_defaults(irrig_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->magic = IRRIG_CONFIG_MAGIC;
    config->version = IRRIG_CONFIG_VERSION;
    config->zone_count = SOLAR_OS_IRRIG_ZONES_DEFAULT;
    for (size_t zone = 0; zone < SOLAR_OS_IRRIG_ZONES_MAX; zone++) {
        config->zones[zone].pin = -1;
    }
}

static esp_err_t irrig_config_save(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(IRRIG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(nvs, IRRIG_NVS_CONFIG_KEY, &irrig.config, sizeof(irrig.config));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void irrig_config_load(void)
{
    irrig_config_defaults(&irrig.config);

    nvs_handle_t nvs;
    if (nvs_open(IRRIG_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    irrig_config_t loaded;
    size_t size = sizeof(loaded);
    const esp_err_t ret = nvs_get_blob(nvs, IRRIG_NVS_CONFIG_KEY, &loaded, &size);
    nvs_close(nvs);

    if (ret != ESP_OK ||
        size != sizeof(loaded) ||
        loaded.magic != IRRIG_CONFIG_MAGIC ||
        loaded.version != IRRIG_CONFIG_VERSION ||
        loaded.zone_count == 0 ||
        loaded.zone_count > SOLAR_OS_IRRIG_ZONES_MAX) {
        return;
    }

    irrig.config = loaded;
}

static bool irrig_zone_valid(uint8_t zone)
{
    return irrig.initialized && zone < irrig.config.zone_count;
}

static void irrig_drive_pin(uint8_t zone, bool on)
{
    const int pin = irrig.config.zones[zone].pin;
    if (pin < 0) {
        return;
    }

    if (!irrig.pin_configured[zone]) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (uint32_t)pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&config) != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "zone %c: gpio %d config failed", 'A' + zone, pin);
            return;
        }
        irrig.pin_configured[zone] = true;
    }

    /* Relay boards are almost universally active-low: drive low = on. */
    (void)gpio_set_level((gpio_num_t)pin, on ? 0 : 1);
}

static void irrig_set_output(uint8_t zone, bool on)
{
    if (irrig.output_on[zone] != on) {
        irrig.output_on[zone] = on;
        SOLAR_OS_LOGI(TAG, "zone %c %s", 'A' + zone, on ? "ON" : "OFF");
    }
    irrig_drive_pin(zone, on);
}

static bool irrig_schedule_matches(const solar_os_irrig_schedule_t *schedule,
                                   uint16_t minute_of_day,
                                   uint8_t weekday)
{
    if (!schedule->active) {
        return false;
    }
    if (minute_of_day < schedule->start_minute || minute_of_day >= schedule->end_minute) {
        return false;
    }

    /* datetime weekday is tm_wday-style (0 = Sunday); the mask is
     * Monday-first (bit0 = Monday ... bit6 = Sunday). */
    const uint8_t day_bit = weekday == 0 ? 6U : (uint8_t)(weekday - 1U);
    return (schedule->days & (uint8_t)(1U << day_bit)) != 0;
}

esp_err_t solar_os_irrig_init(void)
{
    if (irrig.initialized) {
        return ESP_OK;
    }

    memset(&irrig, 0, sizeof(irrig));
    irrig_config_load();
    irrig.mode = SOLAR_OS_IRRIG_MODE_AUTO;
    irrig.initialized = true;

    SOLAR_OS_LOGI(TAG, "engine ready: %u zones", (unsigned)irrig.config.zone_count);
    return ESP_OK;
}

uint8_t solar_os_irrig_zone_count(void)
{
    return irrig.initialized ? irrig.config.zone_count : 0;
}

esp_err_t solar_os_irrig_set_zone_count(uint8_t count)
{
    if (!irrig.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (count == 0 || count > SOLAR_OS_IRRIG_ZONES_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count == irrig.config.zone_count) {
        return ESP_OK;
    }

    /* Zones dropping out of range go quiet immediately. */
    for (uint8_t zone = count; zone < irrig.config.zone_count; zone++) {
        irrig_set_output(zone, false);
        irrig.manual_on[zone] = false;
        irrig.schedule_active[zone] = false;
    }

    irrig.config.zone_count = count;
    return irrig_config_save();
}

esp_err_t solar_os_irrig_get_schedule(uint8_t zone,
                                      uint8_t slot,
                                      solar_os_irrig_schedule_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!irrig_zone_valid(zone) || slot >= SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = irrig.config.zones[zone].schedules[slot];
    return ESP_OK;
}

esp_err_t solar_os_irrig_set_schedule(uint8_t zone,
                                      uint8_t slot,
                                      const solar_os_irrig_schedule_t *schedule)
{
    if (schedule == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!irrig_zone_valid(zone) || slot >= SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (schedule->start_minute >= IRRIG_MINUTES_PER_DAY ||
        schedule->end_minute > IRRIG_MINUTES_PER_DAY ||
        schedule->start_minute >= schedule->end_minute ||
        (schedule->days & ~(uint8_t)SOLAR_OS_IRRIG_DAYS_ALL) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    irrig.config.zones[zone].schedules[slot] = *schedule;
    return irrig_config_save();
}

int solar_os_irrig_zone_pin(uint8_t zone)
{
    return irrig_zone_valid(zone) ? irrig.config.zones[zone].pin : -1;
}

esp_err_t solar_os_irrig_set_zone_pin(uint8_t zone, int pin)
{
    if (!irrig_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pin != -1 && (pin < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(pin))) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Release the old pin's output before switching. */
    irrig_set_output(zone, false);
    irrig.config.zones[zone].pin = (int8_t)pin;
    irrig.pin_configured[zone] = false;
    return irrig_config_save();
}

solar_os_irrig_mode_t solar_os_irrig_mode(void)
{
    return irrig.mode;
}

void solar_os_irrig_set_mode(solar_os_irrig_mode_t mode)
{
    if (mode != SOLAR_OS_IRRIG_MODE_AUTO && mode != SOLAR_OS_IRRIG_MODE_MANUAL) {
        return;
    }
    if (irrig.mode != mode) {
        irrig.mode = mode;
        SOLAR_OS_LOGI(TAG, "mode: %s", mode == SOLAR_OS_IRRIG_MODE_AUTO ? "auto" : "manual");
    }
}

esp_err_t solar_os_irrig_set_manual(uint8_t zone, bool on)
{
    if (!irrig_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }

    irrig.manual_on[zone] = on;
    return ESP_OK;
}

esp_err_t solar_os_irrig_zone_status(uint8_t zone, solar_os_irrig_zone_status_t *out)
{
    if (out == NULL || !irrig_zone_valid(zone)) {
        return ESP_ERR_INVALID_ARG;
    }

    out->output_on = irrig.output_on[zone];
    out->schedule_active = irrig.schedule_active[zone];
    out->manual_on = irrig.manual_on[zone];
    out->pin = irrig.config.zones[zone].pin;
    return ESP_OK;
}

void solar_os_irrig_update(const solar_os_datetime_t *now)
{
    if (!irrig.initialized) {
        return;
    }

    const bool time_ok = now != NULL &&
        solar_os_time_datetime_is_valid(now) &&
        now->clock_integrity;

    if (!time_ok) {
        if (!irrig.time_warning_logged) {
            irrig.time_warning_logged = true;
            SOLAR_OS_LOGW(TAG, "clock not trustworthy; all zones held off");
        }
        for (uint8_t zone = 0; zone < irrig.config.zone_count; zone++) {
            irrig.schedule_active[zone] = false;
            irrig_set_output(zone, irrig.mode == SOLAR_OS_IRRIG_MODE_MANUAL ?
                                       irrig.manual_on[zone] :
                                       false);
        }
        return;
    }
    if (irrig.time_warning_logged) {
        irrig.time_warning_logged = false;
        SOLAR_OS_LOGI(TAG, "clock valid again; schedules resumed");
    }

    const uint16_t minute_of_day = (uint16_t)((now->hour * 60U) + now->minute);
    for (uint8_t zone = 0; zone < irrig.config.zone_count; zone++) {
        bool active = false;
        for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
            if (irrig_schedule_matches(&irrig.config.zones[zone].schedules[slot],
                                       minute_of_day,
                                       now->weekday)) {
                active = true;
                break;
            }
        }

        irrig.schedule_active[zone] = active;
        irrig_set_output(zone, irrig.mode == SOLAR_OS_IRRIG_MODE_MANUAL ?
                                   irrig.manual_on[zone] :
                                   active);
    }
}

void solar_os_irrig_all_off(void)
{
    if (!irrig.initialized) {
        return;
    }

    for (uint8_t zone = 0; zone < irrig.config.zone_count; zone++) {
        irrig.manual_on[zone] = false;
        irrig.schedule_active[zone] = false;
        irrig_set_output(zone, false);
    }
}
