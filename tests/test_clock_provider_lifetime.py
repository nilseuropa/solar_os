from pathlib import Path
import unittest

from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


def clock_registry_source():
    source = (ROOT / "src/services/solar_os_time.c").read_text()
    state = source[source.index("static solar_os_time_provider_t time_provider;"):
                   source.index("static void datetime_from_tm(")]
    functions = production_functions(source, (
        "datetime_from_tm", "datetime_is_valid", "days_from_civil", "epoch_from_utc_datetime",
        "datetime_from_utc_epoch", "solar_os_time_register_provider", "time_unregister_provider",
        "solar_os_time_unregister_provider_owned", "solar_os_time_unregister_provider",
        "solar_os_time_has_provider", "time_provider_acquire", "time_provider_release",
        "solar_os_time_get_utc_datetime", "solar_os_time_set_utc_datetime",
        "solar_os_time_datetime_is_valid",
    ))
    return r'''
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "solar_os_time.h"
#include "freertos/FreeRTOS.h"

#define SECONDS_PER_DAY 86400LL
#define TIME_VALID_EPOCH_SECONDS 946684800LL

static size_t fixture_strlcpy(char *out, const char *value, size_t capacity)
{
    const size_t size = strlen(value);
    if (capacity) {
        const size_t copy = size < capacity - 1 ? size : capacity - 1;
        memcpy(out, value, copy);
        out[copy] = 0;
    }
    return size;
}

#define strlcpy fixture_strlcpy
''' + state + functions + "\n"


class ClockProviderLifetimeTest(unittest.TestCase):
    def test_backend_stays_registered_until_its_callbacks_return(self):
        run_c(clock_registry_source() + r'''
#include <assert.h>

static solar_os_datetime_t rtc = {
    .year = 2026, .month = 11, .day = 26, .weekday = 4, .clock_integrity = true,
};
static solar_os_datetime_t replacement;

static esp_err_t read_clock(void *user, solar_os_datetime_t *out)
{
    assert(solar_os_time_unregister_provider_owned("rtc", user) == ESP_ERR_INVALID_STATE);
    assert(solar_os_time_has_provider());
    *out = *(solar_os_datetime_t *)user;
    return ESP_OK;
}

static esp_err_t write_clock(void *user, const solar_os_datetime_t *value)
{
    assert(solar_os_time_unregister_provider("rtc") == ESP_ERR_INVALID_STATE);
    *(solar_os_datetime_t *)user = *value;
    return ESP_OK;
}

int main(void)
{
    solar_os_time_provider_t provider = {read_clock, write_clock, &rtc};
    assert(solar_os_time_unregister_provider("") == ESP_ERR_INVALID_ARG);
    assert(!solar_os_time_register_provider("rtc", &provider));
    assert(solar_os_time_unregister_provider_owned("rtc", NULL) == ESP_ERR_NOT_FOUND);
    assert(solar_os_time_unregister_provider_owned("rtc", &replacement) == ESP_ERR_NOT_FOUND);
    solar_os_datetime_t value;
    assert(!solar_os_time_get_utc_datetime(&value) && value.year == 2026);
    value.hour = 8;
    assert(!solar_os_time_set_utc_datetime(&value) && rtc.hour == 8);
    assert(!solar_os_time_unregister_provider_owned("rtc", &rtc));
    provider.user = &replacement;
    assert(!solar_os_time_register_provider("rtc", &provider));
    assert(solar_os_time_unregister_provider_owned("rtc", &rtc) == ESP_ERR_NOT_FOUND);
    assert(solar_os_time_has_provider());
    assert(!solar_os_time_unregister_provider("rtc") && !solar_os_time_has_provider());
}
''', "-D_GNU_SOURCE", "-I", str(ROOT / "src/services"),
              "-fsanitize=address,undefined", "-no-pie")
