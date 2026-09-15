#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_haptic.h"

typedef struct {
    uint16_t effect;
    unsigned plays;
    unsigned stops;
} fake_haptic_t;

static esp_err_t fake_play(void *ctx, uint16_t effect)
{
    fake_haptic_t *fake = ctx;
    fake->effect = effect;
    fake->plays++;
    return ESP_OK;
}

static esp_err_t fake_stop(void *ctx)
{
    fake_haptic_t *fake = ctx;
    fake->stops++;
    return ESP_OK;
}

int main(void)
{
    static const solar_os_haptic_ops_t ops = {
        .play_effect = fake_play,
        .stop = fake_stop,
    };
    fake_haptic_t first = {0};
    fake_haptic_t second = {0};
    const solar_os_haptic_registration_t one = {
        .name = "haptic0",
        .driver = "fake",
        .effect_count = 7U,
        .ops = &ops,
        .ctx = &first,
    };
    const solar_os_haptic_registration_t two = {
        .name = "haptic1",
        .driver = "fake2",
        .effect_count = 3U,
        .ops = &ops,
        .ctx = &second,
    };

    assert(solar_os_haptic_register(&one) == ESP_OK);
    assert(solar_os_haptic_register(&one) == ESP_ERR_INVALID_STATE);
    assert(solar_os_haptic_register(&two) == ESP_OK);
    assert(solar_os_haptic_count() == 2U);

    solar_os_haptic_info_t info;
    assert(solar_os_haptic_get(0U, &info));
    assert(strcmp(info.name, "haptic0") == 0);
    assert(strcmp(info.driver, "fake") == 0);
    assert(info.effect_count == 7U);
    assert(solar_os_haptic_get(1U, &info));
    assert(strcmp(info.name, "haptic1") == 0);
    assert(!solar_os_haptic_get(2U, &info));

    assert(solar_os_haptic_play_effect("haptic0", 0U) == ESP_ERR_INVALID_ARG);
    assert(solar_os_haptic_play_effect("haptic0", 8U) == ESP_ERR_INVALID_ARG);
    assert(solar_os_haptic_play_effect("haptic0", 6U) == ESP_OK);
    assert(first.plays == 1U && first.effect == 6U);
    assert(solar_os_haptic_stop("haptic0") == ESP_OK);
    assert(first.stops == 1U);
    assert(solar_os_haptic_play_effect("missing", 1U) == ESP_ERR_NOT_FOUND);

    assert(solar_os_haptic_unregister("haptic0") == ESP_OK);
    assert(solar_os_haptic_unregister("haptic0") == ESP_ERR_NOT_FOUND);
    assert(solar_os_haptic_count() == 1U);
    assert(solar_os_haptic_unregister("haptic1") == ESP_OK);
    assert(solar_os_haptic_count() == 0U);

    puts("haptic service registry tests: ok");
    return 0;
}
