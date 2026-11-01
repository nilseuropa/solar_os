#pragma once
#include "esp_err.h"
#include "solar_os_robot_abi.h"

/* Thin frontend: all joint state, calibration, control and workers live in
 * the optional robot ELF. Each entry retains its own library lease. */
esp_err_t solar_os_robot_joint_create(const char *name, uint32_t kind, const char *owner);
esp_err_t solar_os_robot_joint_configure(const char *name, const char *key, float value);
esp_err_t solar_os_robot_joint_bind_motor(const char *name, const char *motor, const char *encoder);
esp_err_t solar_os_robot_joint_bind_servo(const char *name, const solar_os_native_servo_config_v1_t *config);
esp_err_t solar_os_robot_joint_command(const char *name, float target);
esp_err_t solar_os_robot_joint_enable(const char *name);
esp_err_t solar_os_robot_joint_disable(const char *name);
esp_err_t solar_os_robot_joint_status(const char *name, solar_os_robot_joint_info_v3_t *info);
esp_err_t solar_os_robot_joint_close(const char *name);
size_t solar_os_robot_joint_count(void);
bool solar_os_robot_joint_get_info(size_t index, solar_os_robot_joint_info_v3_t *info);
esp_err_t solar_os_robot_release_owner(const char *owner);
