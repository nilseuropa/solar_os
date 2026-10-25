#include "solar_os_controller_math.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

static float limit(double value, const solar_os_controller_math_config_t *c)
{
    return (float)fmin(fmax(value, c->output_min), c->output_max);
}

static bool representable(double value)
{
    return isfinite(value) && fabs(value) <= FLT_MAX;
}

solar_os_controller_math_config_t solar_os_controller_math_defaults(
    solar_os_controller_kind_t kind)
{
    return (solar_os_controller_math_config_t) {
        .kind = kind, .kp = 1.0f, .ki = 0.1f, .derivative_tau = 0.05f,
        .output_max = 1.0f, .hysteresis = 1.0f, .max_dt = 1.0f,
    };
}

bool solar_os_controller_math_valid(const solar_os_controller_math_config_t *c)
{
    return c != NULL &&
        (c->kind == SOLAR_OS_CONTROLLER_PID || c->kind == SOLAR_OS_CONTROLLER_HYSTERESIS) &&
        isfinite(c->kp) && c->kp >= 0 && isfinite(c->ki) && c->ki >= 0 &&
        isfinite(c->kd) && c->kd >= 0 && isfinite(c->derivative_tau) &&
        c->derivative_tau >= 0 && (c->kd == 0 || c->derivative_tau > 0) &&
        isfinite(c->output_min) && isfinite(c->output_max) && c->output_min < c->output_max &&
        isfinite(c->hysteresis) && c->hysteresis > 0 &&
        isfinite(c->max_dt) && c->max_dt > 0;
}

bool solar_os_controller_math_reset(const solar_os_controller_math_config_t *c,
    solar_os_controller_math_state_t *s, float output)
{
    if (!solar_os_controller_math_valid(c) || s == NULL || !isfinite(output) ||
        output < c->output_min || output > c->output_max) return false;
    *s = (solar_os_controller_math_state_t) {
        .automatic = true, .output = output,
    };
    return true;
}

bool solar_os_controller_math_manual(const solar_os_controller_math_config_t *c,
    solar_os_controller_math_state_t *s, float output)
{
    if (!solar_os_controller_math_valid(c) || s == NULL || !isfinite(output) ||
        output < c->output_min || output > c->output_max) return false;
    s->automatic = false;
    s->output = output;
    s->tracking = true;
    return true;
}

void solar_os_controller_math_auto(solar_os_controller_math_state_t *s)
{
    if (s != NULL && !s->automatic) { s->automatic = true; s->tracking = true; }
}

bool solar_os_controller_math_step(const solar_os_controller_math_config_t *c,
    solar_os_controller_math_state_t *s, float setpoint, float measurement, float dt)
{
    if (!solar_os_controller_math_valid(c) || s == NULL || !isfinite(setpoint) ||
        !isfinite(measurement) || !isfinite(dt) || dt <= 0 || dt > c->max_dt ||
        !isfinite(s->output) || !isfinite(s->i) || !isfinite(s->d)) return false;
    solar_os_controller_math_state_t next = *s;
    const double direction = c->reverse ? -1.0 : 1.0;
    const double error = direction * ((double)setpoint - measurement);
    if (!representable(error)) return false;
    next.error = (float)error;
    if (c->kind == SOLAR_OS_CONTROLLER_HYSTERESIS) {
        if (s->automatic) {
            next.output = s->output >= ((double)c->output_min + c->output_max) / 2 ?
                c->output_max : c->output_min;
            if (error >= (double)c->hysteresis / 2) next.output = c->output_max;
            else if (error <= -(double)c->hysteresis / 2) next.output = c->output_min;
        }
        next.p = next.i = next.d = 0;
        next.tracking = false;
        next.saturated = false;
    } else {
        const double p = c->kp * error;
        /* Backward-Euler low-pass derivative, on measurement only. */
        const double d = c->kd == 0 || !s->initialized ? 0 :
            ((double)c->derivative_tau * s->d - direction * c->kd *
             ((double)measurement - s->previous_measurement)) / (c->derivative_tau + (double)dt);
        double integral = c->ki == 0 ? 0 : s->i;
        if (c->ki > 0) {
            if (!s->automatic || s->tracking) integral = (double)s->output - p - d;
            else {
                const double delta = (double)c->ki * error * dt;
                const double candidate = p + integral + delta + d;
                if (!((candidate > c->output_max && delta > 0) ||
                      (candidate < c->output_min && delta < 0))) integral += delta;
            }
        }
        const double raw = p + integral + d;
        if (!representable(p) || !representable(d) || !representable(integral) ||
            !representable(raw)) return false;
        next.p = (float)p; next.i = (float)integral; next.d = (float)d;
        if (s->automatic) next.output = limit(raw, c);
        next.saturated = s->automatic && (raw < c->output_min || raw > c->output_max);
        next.tracking = !s->automatic;
    }
    next.previous_measurement = measurement;
    next.initialized = true;
    *s = next;
    return true;
}
