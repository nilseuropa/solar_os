/* Compile on SolarOS: cc main.c -o /modules/apps/jet-cube.elf */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include "solar_os_native_library_abi.h"
#include <jet/solar_os_jet_abi.h>

#define WIDTH 160
#define HEIGHT 120
#define PIXELS (WIDTH * HEIGHT)

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
    .worker_stack_bytes = 8192, .worker_priority = 4,
    .minimum_internal_bytes = 16384, .minimum_psram_bytes = 524288,
    .tick_interval_ms = 5,
    .id = "jet-cube", .summary = "C SDK cube: arrows rotate, Space pauses, Q exits",
};

static bool cancelled(void *context)
{
    const solar_os_native_foreground_api_v1_t *app = context;
    return app->stop_requested() || app->suspend_requested();
}

int main(int argc, char **argv)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    const solar_os_native_foreground_api_v1_t *app = host->get_service("foreground", 1,
        offsetof(solar_os_native_foreground_api_v1_t, video_open_index2));
    const solar_os_native_library_service_api_v1_t *libraries = host->get_service("library", 1, sizeof(*libraries));
    if (!app || !libraries) return 2;
    const void *table = NULL;
    uint32_t lease = 0, cube = 0;
    int code = libraries->acquire("jet", 1, SOLAR_OS_JET_BASE_SIZE, &lease, &table);
    if (code) return code;
    const solar_os_jet_api_v1_t *jet = table;
    uint16_t *color = malloc(PIXELS * 2), *depth = malloc(PIXELS * 2);
    uint8_t *indexed = malloc(PIXELS);
    void *scene = NULL;
    if (!color || !depth || !indexed) { code = -4; goto done; }
    code = jet->create(WIDTH, HEIGHT, color, depth, PIXELS * 2, &scene);
    if (code) goto done;
    solar_os_jet_pose_t camera = { .z = -220 };
    solar_os_jet_pose_t pose = { .rx = 25, .ry = 35 };
    if ((code = jet->background(scene, 0xffffff)) ||
        (code = jet->camera(scene, &camera, 60)) ||
        (code = jet->light(scene, 0x505050, 0xffffff, 180, -30, 45)) ||
        (code = jet->cube(scene, 85, 85, 85, 0x3878b8, &cube))) goto done;
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; ++i)
        palette[i] = ((i >> 3) << 11) | ((i >> 2) << 5) | (i >> 3);
    if ((code = app->video_open_ex(WIDTH, HEIGHT, palette, 10, 255)) ||
        (code = app->enable_suspend())) goto done;
    bool paused = argc > 1 && !strcmp(argv[1], "--paused");
    bool dirty = true, quit = false;
    while (!quit && !app->stop_requested()) {
        if (app->suspend_requested()) {
            code = app->suspend();
            if (code || app->stop_requested()) break;
            dirty = true;
        }
        solar_os_native_foreground_key_v1_t key;
        bool available = false;
        while (!(code = app->read_key(&key, &available)) && available) {
            if (key.reset || key.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE) continue;
            if (key.key == 'q' || key.key == 'Q' || key.key == SOLAR_OS_KEY_ESCAPE || key.key == SOLAR_OS_KEY_APP_EXIT)
                quit = true;
            if (key.key == ' ' && key.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS) paused = !paused;
            if (key.key == SOLAR_OS_KEY_LEFT) { pose.ry = (pose.ry + 345) % 360; dirty = true; }
            if (key.key == SOLAR_OS_KEY_RIGHT) { pose.ry = (pose.ry + 15) % 360; dirty = true; }
            if (key.key == SOLAR_OS_KEY_UP) { pose.rx = (pose.rx + 345) % 360; dirty = true; }
            if (key.key == SOLAR_OS_KEY_DOWN) { pose.rx = (pose.rx + 15) % 360; dirty = true; }
        }
        if (code || quit) break;
        if (!paused) { pose.ry = (pose.ry + 3) % 360; dirty = true; }
        if (dirty) {
            if ((code = jet->transform(scene, cube, &pose))) break;
            code = jet->render(scene, color, PIXELS * 2, cancelled, (void *)app);
            if (cancelled((void *)app)) { code = 0; continue; }
            if (code) break;
            /* Jet returns MSB-first RGB565. The existing foreground API accepts
             * palette indices, so convert to luminance without changing Jet. */
            const uint8_t *wire = (const uint8_t *)color;
            for (unsigned i = 0; i < PIXELS; ++i) {
                unsigned rgb = ((unsigned)wire[i*2] << 8) | wire[i*2+1];
                unsigned r = ((rgb >> 11) & 31) * 255 / 31;
                unsigned g = ((rgb >> 5) & 63) * 255 / 63;
                unsigned b = (rgb & 31) * 255 / 31;
                indexed[i] = (r*77 + g*150 + b*29) >> 8;
            }
            if ((code = app->present(indexed, PIXELS))) break;
            dirty = false;
        }
        app->sleep_ms(100);
    }
done:
    if (scene) {
        int cleanup = jet->destroy(scene);
        if (!code) code = cleanup;
    }
    free(indexed); free(depth); free(color);
    int cleanup = libraries->release(lease);
    return code ? code : cleanup;
}
