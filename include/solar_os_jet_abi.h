#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_jet_commands.h"
#include "solar_os_jet_scene_commands.h"
#include "solar_os_jet_effect_commands.h"
#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_JET_LIBRARY "jet"
#define SOLAR_OS_JET_ABI 1U
#define SOLAR_OS_JET_MAX_OBJECTS 32U
#define SOLAR_OS_JET_MAX_VERTICES 1024U
#define SOLAR_OS_JET_MAX_TRIANGLES 2048U
typedef struct { int32_t x, y, z, rx, ry, rz; } solar_os_jet_pose_t;
typedef struct { int32_t x, y, z; } solar_os_jet_vertex_t;
typedef struct { uint16_t a, b, c; } solar_os_jet_triangle_t;
typedef struct {
    uint32_t abi_version, struct_size;
    int (*execute)(void *scene, const solar_os_jet_command_t *command, solar_os_jet_reply_t *reply);
} solar_os_jet_extensions_v1_t;

/* One live scene per loaded provider. Serialize all calls; concurrent/reentrant
 * calls return INVALID_STATE without waiting. Geometry is copied;
 * color/depth buffers are borrowed until destroy. RGB565 output is MSB-first,
 * full-width, with a matching native 16-bit depth buffer. Coordinates are design
 * units in -4096..4096; angles are integer degrees. Colors are RGB888. The
 * renderer owns no display or tasks. Hold the library lease through destroy.
 * Cancel is synchronous and borrowed only for render; cancelled frames must
 * not be displayed. No C++ object or exception crosses this table. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(uint16_t width, uint16_t height, uint16_t *color,
                  uint16_t *depth, size_t bytes, void **scene);
    int (*destroy)(void *scene);
    int (*background)(void *scene, uint32_t rgb888);
    int (*camera)(void *scene, const solar_os_jet_pose_t *pose, uint16_t fov);
    int (*light)(void *scene, uint32_t ambient, uint32_t color,
                 uint16_t intensity, int32_t azimuth, int32_t elevation);
    int (*cube)(void *scene, uint16_t width, uint16_t height, uint16_t depth,
                uint32_t rgb888, uint32_t *object);
    int (*mesh)(void *scene, const solar_os_jet_vertex_t *vertices, size_t count,
                const solar_os_jet_triangle_t *triangles, size_t triangle_count,
                uint32_t rgb888, uint32_t *object);
    int (*transform)(void *scene, uint32_t object, const solar_os_jet_pose_t *pose);
    int (*remove)(void *scene, uint32_t object);
    int (*render)(void *scene, uint16_t *color, size_t bytes,
                  solar_os_native_cancel_fn cancel, void *user);
    /* Optional append-only extension. Consumers check struct_size before use. */
    const solar_os_jet_extensions_v1_t *extensions;
} solar_os_jet_api_v1_t;
#define SOLAR_OS_JET_BASE_SIZE offsetof(solar_os_jet_api_v1_t,extensions)

#ifdef __cplusplus
}
#endif
