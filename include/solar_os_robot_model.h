#pragma once

#include <stdint.h>

/* Lengths include the trailing NUL. All model units are metres and radians.
 * These bounds are independent of the hardware joint-controller limit. */
#define SOLAR_OS_ROBOT_MODEL_NAME_MAX 64U
#define SOLAR_OS_ROBOT_MODEL_LINK_MAX 64U
#define SOLAR_OS_ROBOT_MODEL_JOINT_MAX 63U
#define SOLAR_OS_ROBOT_MODEL_XML_MAX (128U * 1024U)
#define SOLAR_OS_ROBOT_MODEL_EXTENSION_MAX (32U * 1024U)
#define SOLAR_OS_ROBOT_MODEL_MAX 4U
#define SOLAR_OS_ROBOT_MODEL_DEVICE_MAX 16U

/* A read-only association. Geometry is defined by the URDF link tree;
 * attaching hardware and producing samples remain OS service responsibilities. */
typedef struct {
    char name[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
    char frame[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
} solar_os_robot_model_device_t;

/* T_target_source maps coordinates expressed in source into target.
 * Quaternion order is x,y,z,w; rotations are right-handed and unit length. */
typedef struct {
    float translation[3];
    float rotation[4];
} solar_os_robot_transform_t;

typedef enum {
    SOLAR_OS_ROBOT_MODEL_FIXED = 0,
    SOLAR_OS_ROBOT_MODEL_REVOLUTE,
    SOLAR_OS_ROBOT_MODEL_CONTINUOUS,
    SOLAR_OS_ROBOT_MODEL_PRISMATIC,
} solar_os_robot_model_joint_kind_t;

typedef struct {
    char name[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
    char root[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
    uint32_t links, joints, known_joints, extension_bytes;
} solar_os_robot_model_info_t;

typedef struct {
    char name[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
    char parent[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
    char child[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
    uint32_t kind, position_known, has_limits;
    solar_os_robot_transform_t origin;
    float axis[3], lower, upper, effort, velocity, position;
} solar_os_robot_model_joint_info_t;
