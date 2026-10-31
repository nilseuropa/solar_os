#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_controller_math.h"

static void near(float a, float b) { assert(fabsf(a - b) < 0.0001f); }

int main(void)
{
    solar_os_controller_math_config_t c = solar_os_controller_math_defaults(SOLAR_OS_CONTROLLER_PID);
    solar_os_controller_math_state_t s;
    c.output_min = -10; c.output_max = 10; c.kp = 2; c.ki = 0;
    assert(solar_os_controller_math_reset(&c, &s, 0));
    assert(solar_os_controller_math_step(&c, &s, 2, 1, 0.1f)); near(s.output, 2);
    c.reverse = true;
    assert(solar_os_controller_math_step(&c, &s, 2, 1, 0.1f)); near(s.output, -2);
    c.reverse = false; c.kd = 1; c.derivative_tau = 0.1f;
    assert(solar_os_controller_math_reset(&c, &s, 0));
    assert(solar_os_controller_math_step(&c, &s, 0, 0, 0.1f));
    assert(solar_os_controller_math_step(&c, &s, 3, 0, 0.1f)); near(s.d, 0); /* no setpoint kick */
    assert(solar_os_controller_math_step(&c, &s, 3, 1, 0.1f)); near(s.d, -5);
    assert(solar_os_controller_math_step(&c, &s, 3, 1, 0.1f)); near(s.d, -2.5f);

    c.kp = 1; c.ki = 1; c.kd = 0; c.output_min = 0; c.output_max = 1;
    assert(solar_os_controller_math_reset(&c, &s, 0));
    for (int i = 0; i < 1000; i++) assert(solar_os_controller_math_step(&c, &s, 100, 0, 0.1f));
    near(s.i, 0); near(s.output, 1); assert(s.saturated);
    assert(solar_os_controller_math_step(&c, &s, 0, 0, 0.1f)); near(s.output, 0);
    assert(solar_os_controller_math_manual(&c, &s, 0.4f));
    assert(solar_os_controller_math_step(&c, &s, 1, 0.2f, 0.1f)); near(s.output, 0.4f);
    solar_os_controller_math_auto(&s);
    assert(solar_os_controller_math_step(&c, &s, 1, 0.3f, 0.1f)); near(s.output, 0.4f);

    /* First-order plant: setpoint step, load disturbance, then setpoint reduction. */
    c.output_max = 10; c.kp = 2; c.ki = 1;
    assert(solar_os_controller_math_reset(&c, &s, 0));
    float y = 0;
    for (int i = 0; i < 9000; i++) {
        float setpoint = i < 6000 ? 3 : 1;
        float load = i >= 3000 ? 0.5f : 0;
        assert(solar_os_controller_math_step(&c, &s, setpoint, y, 0.01f));
        y += (s.output - load - y) * 0.01f / 2;
        assert(isfinite(y));
        if (i == 2999 || i == 5999) assert(fabsf(y - 3) < 0.01f);
    }
    assert(fabsf(y - 1) < 0.01f);

    solar_os_controller_math_state_t before = s;
    assert(!solar_os_controller_math_step(&c, &s, NAN, 0, 0.1f));
    assert(!solar_os_controller_math_step(&c, &s, 1, INFINITY, 0.1f));
    assert(!solar_os_controller_math_step(&c, &s, 1, 0, 0));
    assert(!solar_os_controller_math_step(&c, &s, 1, 0, -1));
    assert(!solar_os_controller_math_step(&c, &s, 1, 0, 2));
    assert(!solar_os_controller_math_step(&c, &s, FLT_MAX, -FLT_MAX, 0.1f));
    assert(memcmp(&before, &s, sizeof(s)) == 0);
    c.ki = NAN; assert(!solar_os_controller_math_valid(&c));
    c.ki = 1; c.kd = 1; c.derivative_tau = 0; assert(!solar_os_controller_math_valid(&c));

    c = solar_os_controller_math_defaults(SOLAR_OS_CONTROLLER_HYSTERESIS);
    c.hysteresis = 2;
    assert(solar_os_controller_math_reset(&c, &s, 0));
    assert(solar_os_controller_math_step(&c, &s, 20, 18, 0.1f)); near(s.output, 1);
    for (int i = 0; i < 100; i++) {
        assert(solar_os_controller_math_step(&c, &s, 20, 20 + (i % 2 ? 0.5f : -0.5f), 0.1f));
        near(s.output, 1);
    }
    assert(solar_os_controller_math_step(&c, &s, 20, 21, 0.1f)); near(s.output, 0);
    c.reverse = true;
    assert(solar_os_controller_math_step(&c, &s, 20, 22, 0.1f)); near(s.output, 1);
    assert(solar_os_controller_math_manual(&c, &s, 0.4f));
    solar_os_controller_math_auto(&s);
    assert(solar_os_controller_math_step(&c, &s, 20, 20, 0.1f)); near(s.output, 0);
    puts("controller math tests passed");
}
