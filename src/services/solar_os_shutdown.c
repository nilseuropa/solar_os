#include "solar_os_shutdown.h"

#include <stdatomic.h>

#define SHUTDOWN_STAGE_TIMEOUT_MS 10000U

static atomic_uint requested;
static atomic_int failure;
static solar_os_shutdown_stage_t stage;
static bool started;
static uint32_t stage_started_ms;

esp_err_t solar_os_shutdown_request(void)
{
    return atomic_exchange(&requested, 1U) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

bool solar_os_shutdown_requested(void)
{
    return atomic_load(&requested) != 0U;
}

void solar_os_shutdown_note_failure(esp_err_t error)
{
    if (error != ESP_OK && solar_os_shutdown_requested()) {
        int expected = ESP_OK;
        (void)atomic_compare_exchange_strong(&failure, &expected, error);
    }
}

esp_err_t solar_os_shutdown_error(void)
{
    return atomic_load(&failure);
}

solar_os_shutdown_stage_t solar_os_shutdown_stage(void)
{
    return stage;
}

void solar_os_shutdown_poll(solar_os_shutdown_step_t step, void *user, uint32_t now_ms)
{
    if (!solar_os_shutdown_requested() || solar_os_shutdown_error() != ESP_OK ||
        stage == SOLAR_OS_SHUTDOWN_COMPLETE) {
        return;
    }
    if (step == NULL) {
        solar_os_shutdown_note_failure(ESP_ERR_INVALID_ARG);
        return;
    }
    if (!started) {
        started = true;
        stage_started_ms = now_ms;
    }
    const esp_err_t error = step(stage, user);
    if (error == ESP_ERR_NOT_FINISHED) {
        if ((uint32_t)(now_ms - stage_started_ms) >= SHUTDOWN_STAGE_TIMEOUT_MS) {
            solar_os_shutdown_note_failure(ESP_ERR_TIMEOUT);
        }
    } else if (error != ESP_OK) {
        solar_os_shutdown_note_failure(error);
    } else if (solar_os_shutdown_error() == ESP_OK) {
        stage++;
        stage_started_ms = now_ms;
    }
}

const char *solar_os_shutdown_stage_name(solar_os_shutdown_stage_t value)
{
    static const char *const names[] = {
        "wake source", "port shells", "apps", "jobs", "radios",
        "storage", "display", "power-down", "complete",
    };
    return (unsigned)value <= SOLAR_OS_SHUTDOWN_COMPLETE ? names[value] : "unknown";
}
