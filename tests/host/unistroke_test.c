#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "solar_os_unistroke.h"

int main(void)
{
    const solar_os_unistroke_point_t vertical[] = {{10, 0}, {10, 100}};
    const solar_os_unistroke_point_t horizontal[] = {{0, 10}, {100, 10}};
    solar_os_unistroke_config_t config = solar_os_unistroke_default_config();
    config.rotate_to_indicative_angle = false;
    config.angle_range_radians = 0.0f;
    config.minimum_score = 0.75f;

    solar_os_unistroke_template_t templates[2] = {
        {.value = 1, .flags = 1U},
        {.value = 2, .flags = 2U},
    };
    assert(solar_os_unistroke_prepare(vertical, 2, &config,
                                      &templates[0].path) == SOLAR_OS_UNISTROKE_OK);
    assert(solar_os_unistroke_prepare(horizontal, 2, &config,
                                      &templates[1].path) == SOLAR_OS_UNISTROKE_OK);

    const solar_os_unistroke_point_t noisy_vertical[] = {
        {50, 10}, {49, 30}, {51, 55}, {50, 95},
    };
    solar_os_unistroke_result_t result;
    assert(solar_os_unistroke_recognize(noisy_vertical, 4, templates, 2, 0,
                                        &config, &result) == SOLAR_OS_UNISTROKE_OK);
    assert(result.matched);
    assert(result.value == 1);
    assert(result.score > 0.75f);

    assert(solar_os_unistroke_recognize(noisy_vertical, 4, templates, 2, 2U,
                                        &config, &result) == SOLAR_OS_UNISTROKE_OK);
    assert(result.value == 2);
    assert(!result.matched);

    const solar_os_unistroke_point_t dot[] = {{1, 1}, {1, 1}};
    assert(solar_os_unistroke_prepare(dot, 2, &config, &templates[0].path) ==
           SOLAR_OS_UNISTROKE_TOO_SHORT);
    puts("unistroke_test: ok");
    return 0;
}
