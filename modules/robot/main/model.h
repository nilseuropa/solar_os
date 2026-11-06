#pragma once

#include "solar_os_robot_model.h"
#include "solar_os_robot_abi.h"
#include "solar_os_native_service_abi.h"

typedef struct solar_os_robot_model solar_os_robot_model_t;
bool solar_os_robot_normalize_vector(float *values, unsigned count);

int solar_os_robot_transform_from_rpy(const float xyz[3], const float rpy[3], solar_os_robot_transform_t *out);
int solar_os_robot_transform_compose(const solar_os_robot_transform_t *a, const solar_os_robot_transform_t *b, solar_os_robot_transform_t *out);
int solar_os_robot_transform_inverse(const solar_os_robot_transform_t *in, solar_os_robot_transform_t *out);
int solar_os_robot_transform_apply(const solar_os_robot_transform_t *t, const float in[3], uint32_t direction, float out[3]);

int solar_os_robot_model_parse(const solar_os_native_memory_api_v1_t *memory,
    const char *xml, size_t bytes, solar_os_robot_model_t **out, char *error, size_t error_bytes);
void solar_os_robot_model_destroy(solar_os_robot_model_t *model);
int solar_os_robot_model_info(const solar_os_robot_model_t *model, solar_os_robot_model_info_t *out);
int solar_os_robot_model_link(const solar_os_robot_model_t *model, uint32_t index, char *out, size_t bytes);
int solar_os_robot_model_joint(const solar_os_robot_model_t *model, uint32_t index, solar_os_robot_model_joint_info_t *out);
int solar_os_robot_model_set_positions(solar_os_robot_model_t *model, const char *const *names, const float *positions, uint32_t count);
int solar_os_robot_model_lookup(const solar_os_robot_model_t *model, const char *target, const char *source, solar_os_robot_transform_t *out);
int solar_os_robot_model_extension(const solar_os_robot_model_t *model, char *out, size_t bytes);
int solar_os_robot_model_binding(const solar_os_robot_model_t *model, uint32_t index, solar_os_robot_joint_config_v3_t *out);
int solar_os_robot_model_diff_drive(const solar_os_robot_model_t *model, uint32_t index, solar_os_robot_diff_drive_config_t *out);
