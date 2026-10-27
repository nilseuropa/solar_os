#pragma once
#include "esp_err.h"
#include "solar_os_gfx.h"
#include "solar_os_jet_abi.h"
typedef struct solar_os_jet solar_os_jet_t;
typedef struct {
    const char *display; /* NULL uses the calling foreground. */
    uint16_t fps; /* 0 follows the target, capped at 30. */
} solar_os_jet_options_t;
typedef struct {
    uint32_t frames, presented, dropped;
    uint64_t render_us, present_us;
    uint16_t width, height;
    esp_err_t error;
} solar_os_jet_status_t;

/* Serialize all calls, including lifecycle, on a scene. Rendering runs on the
 * caller; the shared presenter owns its display worker and three color buffers.
 * A failed close preserves the instance and lease for retry. Failed open may
 * return an instance when cleanup cannot join the presenter; retain and close it.
 * One live scene across all consumers, matching the upstream shared caches. */
esp_err_t solar_os_jet_open(const solar_os_jet_options_t *options,
    solar_os_gfx_t *foreground, solar_os_jet_t **out, char *detail, size_t size);
esp_err_t solar_os_jet_close(solar_os_jet_t **scene);
esp_err_t solar_os_jet_suspend(solar_os_jet_t *scene);
esp_err_t solar_os_jet_resume(solar_os_jet_t *scene);
bool solar_os_jet_uses_gfx(const solar_os_jet_t *scene, const solar_os_gfx_t *gfx);
esp_err_t solar_os_jet_background(solar_os_jet_t *scene, uint32_t rgb888);
esp_err_t solar_os_jet_camera(solar_os_jet_t *scene, const solar_os_jet_pose_t *pose, uint16_t fov);
esp_err_t solar_os_jet_light(solar_os_jet_t *scene, uint32_t ambient, uint32_t rgb888,
    uint16_t intensity, int32_t azimuth, int32_t elevation);
esp_err_t solar_os_jet_cube(solar_os_jet_t *scene, uint16_t width, uint16_t height,
    uint16_t depth, uint32_t rgb888, uint32_t *object);
esp_err_t solar_os_jet_mesh(solar_os_jet_t *scene, const solar_os_jet_vertex_t *vertices,
    size_t count, const solar_os_jet_triangle_t *triangles, size_t triangle_count,
    uint32_t rgb888, uint32_t *object);
esp_err_t solar_os_jet_transform(solar_os_jet_t *scene, uint32_t object, const solar_os_jet_pose_t *pose);
esp_err_t solar_os_jet_remove(solar_os_jet_t *scene, uint32_t object);
esp_err_t solar_os_jet_render(solar_os_jet_t *scene, solar_os_native_cancel_fn cancel, void *user);
void solar_os_jet_status(solar_os_jet_t *scene, solar_os_jet_status_t *status);
esp_err_t solar_os_jet_execute(solar_os_jet_t *scene,const solar_os_jet_command_t *command,
    solar_os_jet_reply_t *reply);
