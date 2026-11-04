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

/* One resident model shared by shell and scripts. Its lease is independent of
 * joint controllers and interpreter teardown. Reload commits only after parse
 * and validation succeed; unload is explicit. Paths are already resolved. */
esp_err_t solar_os_robot_model_load_file(const char *path, char *error, size_t error_bytes);
esp_err_t solar_os_robot_model_load_xml(const char *xml, size_t bytes, char *error, size_t error_bytes);
esp_err_t solar_os_robot_model_unload(void);
esp_err_t solar_os_robot_model_get_info(solar_os_robot_model_info_t *out);
esp_err_t solar_os_robot_model_get_link(uint32_t index, char *out, size_t bytes);
esp_err_t solar_os_robot_model_get_joint(uint32_t index, solar_os_robot_model_joint_info_t *out);
esp_err_t solar_os_robot_model_update(const char *const *names, const float *positions, uint32_t count);
esp_err_t solar_os_robot_model_transform(const char *target, const char *source, solar_os_robot_transform_t *out);
esp_err_t solar_os_robot_model_apply(const char *target, const char *source, const float in[3], bool direction, float out[3]);
esp_err_t solar_os_robot_model_get_extension(char *out, size_t bytes);
/* Explicit lifecycle for URDF-declared velocity controllers. Stop releases
 * only model-owned joints. Failed cleanup can be retried and pins the model. */
esp_err_t solar_os_robot_start(void);
esp_err_t solar_os_robot_stop(void);
