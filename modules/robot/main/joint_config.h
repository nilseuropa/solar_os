#pragma once
/* Shared by declarative model validation and manually configured joints. */
#include "solar_os_robot_abi.h"
#include <math.h>
#include <string.h>
static bool solar_os_robot_config_equal(const char *a, const char *b)
{
    if(!a || !b) return false;
    while(*a && *a==*b) { ++a; ++b; }
    return *a==*b;
}
static void solar_os_robot_config_defaults(solar_os_robot_joint_config_v3_t *c, uint32_t kind)
{
    memset(c,0,sizeof(*c));
    c->kind = kind; c->gear_ratio = kind == SOLAR_OS_ROBOT_SERVO_POSITION ? 1 : 0;
    c->motor_polarity = c->encoder_polarity = 1;
    c->position_min = -(float)3.14159265358979323846; c->position_max = (float)3.14159265358979323846;
    c->velocity_limit = 10; c->acceleration_limit = 10; c->max_drive = 0.2f; c->velocity_tau = 0.1f;
    c->kp = 0.02f; c->ki = 0.01f; c->derivative_tau = 0.05f;
    c->period_ms = 20; c->command_timeout_ms = 300; c->feedback_timeout_ms = 100; c->max_gap_ms = 200;
}
static bool solar_os_robot_config_valid(const solar_os_robot_joint_config_v3_t *c, bool ppr_configured, bool ratio_configured)
{
    return isfinite(c->kp) && c->kp>=0 && isfinite(c->ki) && c->ki>=0 &&
        isfinite(c->kd) && c->kd>=0 && isfinite(c->derivative_tau) && c->derivative_tau>=0 &&
        (c->kd==0 || c->derivative_tau>0) && isfinite(c->ppr) &&
        (c->ppr > 0 || (!ppr_configured && c->ppr == 0)) && isfinite(c->gear_ratio) &&
        (c->gear_ratio > 0 || (!ratio_configured && c->kind == SOLAR_OS_ROBOT_MOTOR_VELOCITY && c->gear_ratio == 0)) &&
        (c->motor_polarity == 1 || c->motor_polarity == -1) &&
        (c->encoder_polarity == 1 || c->encoder_polarity == -1) && c->encoder_on_output <= 1 &&
        isfinite(c->position_min) && isfinite(c->position_max) && c->position_min < c->position_max &&
        isfinite(c->position_zero) && isfinite(c->velocity_limit) && c->velocity_limit > 0 &&
        isfinite(c->acceleration_limit) && c->acceleration_limit > 0 && c->max_drive > 0 && c->max_drive <= 1 &&
        isfinite(c->velocity_tau) && c->velocity_tau >= 0 &&
        isfinite(c->friction_bias_forward) && c->friction_bias_forward >= 0 && c->friction_bias_forward <= c->max_drive &&
        isfinite(c->friction_bias_reverse) && c->friction_bias_reverse >= 0 && c->friction_bias_reverse <= c->max_drive &&
        isfinite(c->velocity_deadband) && c->velocity_deadband >= 0 && c->velocity_deadband < c->velocity_limit &&
        c->period_ms >= 10 && c->period_ms <= 1000 && c->command_timeout_ms >= c->period_ms &&
        c->command_timeout_ms <= 60000 && c->feedback_timeout_ms >= c->period_ms && c->feedback_timeout_ms <= 60000 &&
        c->max_gap_ms >= c->period_ms && c->max_gap_ms <= 60000 && c->brake <= 1;
}

static int solar_os_robot_config_set(solar_os_robot_joint_config_v3_t *c, const char *key, float value)
{
    if (!key || !isfinite(value)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int ret = SOLAR_OS_NATIVE_OK;
    if (solar_os_robot_config_equal(key, "ppr") && value > 0) c->ppr = value;
    else if (solar_os_robot_config_equal(key, "gear_ratio") && value > 0) c->gear_ratio = value;
    else if (solar_os_robot_config_equal(key, "motor_polarity") && (value == 1 || value == -1)) c->motor_polarity = (int)value;
    else if (solar_os_robot_config_equal(key, "encoder_polarity") && (value == 1 || value == -1)) c->encoder_polarity = (int)value;
    else if (solar_os_robot_config_equal(key, "position_min")) c->position_min = value;
    else if (solar_os_robot_config_equal(key, "position_max")) c->position_max = value;
    else if (solar_os_robot_config_equal(key, "position_zero")) c->position_zero = value;
    else if (solar_os_robot_config_equal(key, "velocity_limit")) c->velocity_limit = value;
    else if (solar_os_robot_config_equal(key, "acceleration_limit")) c->acceleration_limit = value;
    else if (solar_os_robot_config_equal(key, "max_drive")) c->max_drive = value;
    else if (solar_os_robot_config_equal(key, "velocity_tau")) c->velocity_tau = value;
    else if (solar_os_robot_config_equal(key, "friction_bias_forward")) c->friction_bias_forward = value;
    else if (solar_os_robot_config_equal(key, "friction_bias_reverse")) c->friction_bias_reverse = value;
    else if (solar_os_robot_config_equal(key, "velocity_deadband")) c->velocity_deadband = value;
    else if (solar_os_robot_config_equal(key, "kp")) c->kp = value;
    else if (solar_os_robot_config_equal(key, "ki")) c->ki = value;
    else if (solar_os_robot_config_equal(key, "kd")) c->kd = value;
    else if (solar_os_robot_config_equal(key, "tau")) c->derivative_tau = value;
    else if (value >= 0 && value <= 60000 && (float)(uint32_t)value == value) {
        if (solar_os_robot_config_equal(key, "encoder_on_output") && value <= 1) c->encoder_on_output = (uint32_t)value;
        else if (solar_os_robot_config_equal(key, "brake") && value <= 1) c->brake = (uint32_t)value;
        else if (solar_os_robot_config_equal(key, "period_ms")) c->period_ms = (uint32_t)value;
        else if (solar_os_robot_config_equal(key, "command_timeout_ms")) c->command_timeout_ms = (uint32_t)value;
        else if (solar_os_robot_config_equal(key, "feedback_timeout_ms")) c->feedback_timeout_ms = (uint32_t)value;
        else if (solar_os_robot_config_equal(key, "max_gap_ms")) c->max_gap_ms = (uint32_t)value;
        else ret = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    } else ret = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return ret;
}
