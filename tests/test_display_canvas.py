from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class DisplayCanvasTest(unittest.TestCase):
    def test_native_discovery_copies_geometry_and_routes_layout_to_session_owner(self):
        run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_display.h"
#include "solar_os_gfx_internal.h"
#include "solar_os_sessions.h"
#include "solar_os_native_display.h"
#include "solar_os_native_display_abi.h"
#include "solar_os_native_display_layer_abi.h"
#include "solar_os_native_display_layout_abi.h"
#include "solar_os_native_display_lease_abi.h"
#include "solar_os_frame_presenter.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"

static solar_os_display_target_t target = {
    .name = "display0", .driver = "fixture", .ready = true, .epaper = true,
    .panel = {0, 0, 400, 300}, .canvas = {20, 30, 360, 240},
    .width = 400, .height = 300, .native_width = 800, .native_height = 600,
};
static solar_os_terminal_profile_t current = {.scale = 2};
static bool busy, board_primary;
static bool exporting, leased, fail_allocation;
static unsigned allocations, stops;
static bool split_created;
static solar_os_display_theme_v1_t theme = {.foreground_rgb888 = 0x112233, .background_rgb888 = 0xffffff};
static char display_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
static solar_os_gfx_t fixture_gfx;
static uint32_t last_mask, last_values;
static unsigned claim_releases;
static bool fail_release;
static bool fail_frames_open, fail_frames_close, have_frames;
static char layer_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
static uint8_t layer_pixels[4];
static bool fail_layer_clear, replace_layer_during_clear;
static u8g2_t layer_canvas;
static struct solar_os_frame_presenter {
    solar_os_frame_presenter_options_v1_t options;
    uint8_t storage[2][4];
    unsigned slot;
    bool active, borrowed, submitted;
} frames;

static const solar_os_app_t app = {.name = "capture"};
const solar_os_app_t *solar_os_application_descriptor(void) { return &app; }
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag) {
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    assert(!strcmp(tag, "native.display.export") || !strcmp(tag, "native.display.lease") ||
           !strcmp(tag, "native.display.layer"));
    if (fail_allocation) return NULL;
    allocations++;
    return calloc(count, size);
}
void solar_os_memory_free(void *ptr) { if (ptr) allocations--; free(ptr); }
esp_err_t solar_os_display_set_layer(const char *name, const char *owner, const uint8_t *bits,
                                     size_t size, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    assert(!strcmp(name, target.name) && !display_owner[0]);
    assert(size >= 4 && x == 3 && y == 4 && w == 9 && h == 2);
    if (layer_owner[0] && strcmp(owner, layer_owner)) return ESP_ERR_INVALID_STATE;
    strcpy(layer_owner, owner);
    memcpy(layer_pixels, bits, sizeof(layer_pixels));
    return ESP_OK;
}
bool solar_os_display_has_layer(const char *name, const char *owner) {
    assert(!strcmp(name, target.name));
    return !strcmp(owner, layer_owner);
}
esp_err_t solar_os_display_clear_layer(const char *name, const char *owner) {
    assert(solar_os_display_has_layer(name, owner));
    if (replace_layer_during_clear) {
        strcpy(layer_owner, "screen-keyboard");
        return ESP_ERR_INVALID_STATE;
    }
    if (fail_layer_clear) return ESP_ERR_TIMEOUT;
    layer_owner[0] = '\0';
    return ESP_OK;
}
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t cls, const char *tag) {
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "native.display.palette"));
    allocations++;
    return malloc(bytes);
}
esp_err_t solar_os_frame_presenter_init(solar_os_frame_presenter_t **out, solar_os_gfx_t *gfx,
                                       const solar_os_frame_presenter_options_v1_t *options) {
    assert(gfx == &fixture_gfx && !*out && !have_frames);
    assert(options->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 && options->stride == 2);
    assert(options->width == 8 && options->height == 2 && options->fullscreen);
    assert(options->palette_size == 4 && options->palette_rgb565[1] == 0x1234);
    frames = (solar_os_frame_presenter_t){.options = *options, .active = !fail_frames_open};
    *out = &frames;
    have_frames = true;
    return fail_frames_open ? ESP_ERR_NO_MEM : ESP_OK;
}
esp_err_t solar_os_frame_presenter_deinit(solar_os_frame_presenter_t *value) {
    if (!value) return ESP_OK;
    assert(value == &frames && have_frames && !value->borrowed);
    if (fail_frames_close) return ESP_ERR_TIMEOUT;
    have_frames = false;
    return ESP_OK;
}
esp_err_t solar_os_frame_presenter_suspend_checked(solar_os_frame_presenter_t *value) {
    assert(value == &frames && have_frames);
    value->active = false;
    return ESP_OK;
}
esp_err_t solar_os_frame_presenter_resume(solar_os_frame_presenter_t *value) {
    assert(value == &frames && have_frames);
    value->active = true;
    return ESP_OK;
}
bool solar_os_frame_presenter_submit(solar_os_frame_presenter_t *value, const uint8_t *data, size_t bytes) {
    assert(value == &frames && have_frames && !value->borrowed && data && bytes == 4);
    assert(value->options.palette_rgb565[1] == 0x1234);
    return value->submitted = value->active;
}
esp_err_t solar_os_frame_presenter_acquire(solar_os_frame_presenter_t *value, uint8_t **data, size_t *bytes) {
    assert(value == &frames && have_frames && !value->borrowed && data && bytes);
    if (!value->active) return ESP_ERR_INVALID_STATE;
    value->borrowed = true;
    *data = value->storage[value->slot];
    *bytes = 4;
    return ESP_OK;
}
esp_err_t solar_os_frame_presenter_exchange(solar_os_frame_presenter_t *value, uint8_t **data) {
    assert(value == &frames && have_frames);
    if (!value->active || !value->borrowed || !data || *data != value->storage[value->slot])
        return ESP_ERR_INVALID_STATE;
    value->submitted = true;
    value->slot ^= 1;
    *data = value->storage[value->slot];
    return ESP_OK;
}
esp_err_t solar_os_frame_presenter_release(solar_os_frame_presenter_t *value, const uint8_t *data) {
    assert(value == &frames && have_frames);
    if (!value->borrowed || data != value->storage[value->slot]) return ESP_ERR_INVALID_STATE;
    value->borrowed = false;
    return ESP_OK;
}
bool solar_os_frame_presenter_redraw(solar_os_frame_presenter_t *value) {
    assert(value == &frames && have_frames);
    return value->active && value->submitted;
}
void solar_os_frame_presenter_take_stats(solar_os_frame_presenter_t *value, solar_os_frame_presenter_stats_t *out) {
    assert(value == &frames && have_frames);
    *out = (solar_os_frame_presenter_stats_t){.present_us = 123456, .presented_frames = 8,
        .replaced_frames = 2, .last_error = ESP_ERR_TIMEOUT};
}
esp_err_t solar_os_display_claim(const char *name, const char *owner, char *busy, size_t capacity) {
    if (strcmp(name, target.name)) return ESP_ERR_NOT_FOUND;
    if (display_owner[0]) {
        assert(strcmp(owner, display_owner) && busy && capacity >= sizeof(display_owner));
        strcpy(busy, display_owner);
        return ESP_ERR_INVALID_STATE;
    }
    assert(strlen(owner) < sizeof(display_owner));
    strcpy(display_owner, owner);
    return ESP_OK;
}
esp_err_t solar_os_display_open_gfx(const char *name, const char *owner, solar_os_gfx_t **out,
                                   char *busy, size_t capacity) {
    const esp_err_t error = solar_os_display_claim(name, owner, busy, capacity);
    *out = error == ESP_OK ? &fixture_gfx : NULL;
    return error;
}
esp_err_t solar_os_display_release(const char *name, const char *owner) {
    assert(!strcmp(name, target.name) && !strcmp(owner, display_owner));
    if (fail_release) return ESP_ERR_TIMEOUT;
    display_owner[0] = 0;
    last_mask = last_values = 0;
    ++claim_releases;
    return ESP_OK;
}
esp_err_t solar_os_display_set_claim_policy(const char *name, const char *owner,
                                           uint32_t mask, uint32_t values) {
    assert(!strcmp(name, target.name) && !strcmp(owner, display_owner));
    last_mask = mask;
    last_values = values;
    return ESP_OK;
}
static const void *refresh_holder;
static uint16_t refresh_rate;
static bool fail_refresh_restore;
esp_err_t solar_os_display_set_high_refresh_owned(const char *name, const void *owner,
                                                  bool enabled, uint16_t rate) {
    assert(!strcmp(name, target.name) && display_owner[0] && owner);
    if (enabled) {
        assert(!refresh_holder || refresh_holder == owner);
        if (rate != 255) return ESP_ERR_INVALID_ARG;
        refresh_holder = owner;
        refresh_rate = rate;
    } else {
        assert(refresh_holder == owner && !rate);
        if (fail_refresh_restore) return ESP_ERR_TIMEOUT;
        refresh_holder = NULL;
        refresh_rate = 0;
    }
    return ESP_OK;
}
esp_err_t solar_os_display_relax(const char *name, const char *owner, unsigned seconds,
                                 bool (*cancelled)(void *), void *user,
                                 solar_os_display_relax_result_t *result, char *busy, size_t capacity) {
    assert(!strcmp(name, target.name) && !strcmp(owner, display_owner) && !busy && !capacity);
    if (!seconds || seconds > 86400) return ESP_ERR_INVALID_ARG;
    *result = (solar_os_display_relax_result_t){.cycles = 2, .cancelled = cancelled && cancelled(user)};
    return ESP_OK;
}
static bool cancel_maintenance(void *user) {
    size_t *calls = user;
    ++*calls;
    return true;
}
esp_err_t solar_os_display_start_frame_export(const char *name) {
    if (strcmp(name, target.name)) return ESP_ERR_NOT_FOUND;
    if (exporting) return ESP_ERR_INVALID_STATE;
    exporting = true; return ESP_OK;
}
void solar_os_display_stop_frame_export(const char *name) {
    assert(!strcmp(name, target.name) && exporting && !leased);
    exporting = false; stops++;
}
static const uint8_t mono[16] = {0x42}, rgb[16 * 8 * 2] = {0x12, 0x34};
esp_err_t solar_os_display_acquire_frame(const char *name, solar_os_display_frame_t *frame) {
    assert(!strcmp(name, target.name) && exporting && !leased);
    leased = true;
    *frame = (solar_os_display_frame_t){
        .data = mono, .rgb565_data = rgb, .data_size = sizeof(mono), .rgb565_size = sizeof(rgb),
        .frame_id = 19, .width = 8, .height = 16, .native_width = 16, .native_height = 8,
        .native_stride = 16, .rotation = SOLAR_OS_DISPLAY_ROTATION_90, .black_is_one = true,
    };
    return ESP_OK;
}
void solar_os_display_release_frame(solar_os_display_frame_t *frame) {
    assert(frame->data == mono && leased);
    leased = false; memset(frame, 0, sizeof(*frame));
}
esp_err_t solar_os_display_get_controller_mode(const char *name, char *mode, size_t mode_capacity,
                                                char *options, size_t options_capacity) {
    assert(!strcmp(name, target.name) && mode_capacity == 16 && !options && !options_capacity);
    strcpy(mode, "fixture");
    return ESP_OK;
}
esp_err_t solar_os_display_set_controller_mode(const char *name, const char *mode) {
    assert(!strcmp(name, target.name) && !strcmp(mode, "refresh=auto"));
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t solar_os_display_get_theme(solar_os_display_theme_v1_t *out) {
    *out = theme;
    return ESP_OK;
}
esp_err_t solar_os_display_set_theme_color(solar_os_display_theme_role_t role, uint32_t rgb888) {
    assert(role == SOLAR_OS_DISPLAY_THEME_BACKGROUND && rgb888 == 0x445566);
    theme.background_rgb888 = rgb888;
    return ESP_OK;
}
size_t solar_os_display_layout_count(void) { return split_created ? 1 : 0; }
bool solar_os_display_layout_get(size_t index, solar_os_display_layout_info_t *info) {
    *info = (solar_os_display_layout_info_t){0};
    if (index || !split_created) return false;
    info->kind = SOLAR_OS_DISPLAY_LAYOUT_SPLIT;
    info->axis = SOLAR_OS_DISPLAY_LAYOUT_VERTICAL;
    info->backing_count = 1;
    strcpy(info->backing[0], "display0");
    strcpy(info->name, "display0");
    return true;
}
esp_err_t solar_os_display_layout_find(solar_os_display_layout_kind_t kind, const char *name,
                                       solar_os_display_layout_info_t *info) {
    assert(kind == SOLAR_OS_DISPLAY_LAYOUT_SPLIT && !strcmp(name, "display0"));
    return solar_os_display_layout_get(0, info) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
const char *solar_os_display_layout_axis_name(solar_os_display_layout_axis_t axis) {
    return axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL ? "vertical" : "horizontal";
}
esp_err_t solar_os_sessions_create_display_layout(const solar_os_display_layout_request_v1_t *request,
                                                    char *busy_owner, size_t capacity) {
    assert(request->kind == SOLAR_OS_DISPLAY_LAYOUT_SPLIT && request->axis == SOLAR_OS_DISPLAY_LAYOUT_VERTICAL);
    assert(!strcmp(request->name, "display0") && request->member_count == 2 && !busy_owner && !capacity);
    split_created = true;
    return ESP_OK;
}
esp_err_t solar_os_sessions_remove_display_layout(solar_os_display_layout_kind_t kind, const char *name) {
    assert(kind == SOLAR_OS_DISPLAY_LAYOUT_SPLIT && !strcmp(name, "display0") && split_created);
    split_created = false;
    return ESP_OK;
}

size_t solar_os_display_target_count(void) { return 1; }
bool solar_os_display_get_target(size_t index, solar_os_display_target_t *out) {
    if (index) return false;
    *out = target;
    return true;
}
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *out) {
    return strcmp(name, target.name) == 0 && solar_os_display_get_target(0, out);
}
bool solar_os_display_target_is_board_primary(const char *name) {
    assert(strcmp(name, target.name) == 0);
    return board_primary;
}
esp_err_t solar_os_display_get_terminal_profile(const char *name, solar_os_terminal_profile_t *out) {
    if (!name) return ESP_ERR_INVALID_ARG;
    if (strcmp(name, target.name)) return ESP_ERR_NOT_FOUND;
    *out = current;
    return ESP_OK;
}
esp_err_t solar_os_sessions_set_display_terminal_profile(const char *name,
    const solar_os_terminal_profile_t *profile) {
    if (!name || !profile) return ESP_ERR_INVALID_ARG;
    if (strcmp(name, target.name)) return ESP_ERR_NOT_FOUND;
    if (busy) return ESP_ERR_INVALID_STATE;
    current = *profile;
    return ESP_OK;
}
esp_err_t solar_os_sessions_set_display_canvas(const char *name,
    const solar_os_display_rect_t *canvas) {
    if (!name) return ESP_ERR_INVALID_ARG;
    if (strcmp(name, target.name)) return ESP_ERR_NOT_FOUND;
    if (busy) return ESP_ERR_INVALID_STATE;
    target.canvas = canvas ? *canvas : target.panel;
    return ESP_OK;
}
int main(void) {
    const solar_os_native_display_api_v1_t *api =
        solar_os_native_display_get_service("display", 1, sizeof(*api));
    assert(api && api->count() == 1);
    assert(!solar_os_native_display_get_service("display", 2, 8));
    assert(!solar_os_native_display_get_service("display", 1, sizeof(*api) + 1));
    solar_os_native_display_info_v1_t info;
    assert(api->info(0, &info) == 0 && info.canvas.x == 20 && info.native_width == 800);
    assert(info.flags == (SOLAR_OS_NATIVE_DISPLAY_READY | SOLAR_OS_NATIVE_DISPLAY_EPAPER));
    assert(api->set_canvas("display0", NULL) == 0 && target.canvas.x == 0 && info.canvas.x == 20);
    board_primary = true;
    assert(api->find("display0", &info) == 0 && (info.flags & SOLAR_OS_NATIVE_DISPLAY_BOARD_PRIMARY));
    assert(api->find("removed", &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.name[0] && !info.flags);
    solar_os_terminal_profile_t profile;
    assert(api->get_profile("display0", &profile) == 0 && profile.scale == 2);
    profile.scale = 3;
    busy = true;
    assert(api->set_profile("display0", &profile) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && current.scale == 2);
    busy = false;
    assert(api->set_profile("display0", &profile) == 0 && current.scale == 3);
    assert(api->get_profile("removed", &profile) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !profile.scale);
    solar_os_native_handle_t handle, duplicate;
    fail_allocation = true;
    assert(api->export_open("display0", &handle) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !handle);
    fail_allocation = false;
    assert(api->export_open("removed", &handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !handle && !allocations);
    assert(api->export_open("display0", &handle) == SOLAR_OS_NATIVE_OK && exporting && allocations == 1);
    assert(api->export_open("display0", &duplicate) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE &&
           !duplicate && exporting && allocations == 1);
    solar_os_native_display_frame_v1_t frame;
    assert(api->frame_acquire(handle, &frame) == SOLAR_OS_NATIVE_OK && frame.frame_id == 19);
    assert(frame.mono == mono && frame.rgb565 == rgb && frame.mono_stride == 16);
    assert(frame.black_is_one && frame.rotation == SOLAR_OS_DISPLAY_ROTATION_90 &&
           frame.width == 8 && frame.native_height == 8);
    assert(api->frame_acquire(handle, &frame) == SOLAR_OS_NATIVE_ERROR_BUSY && !frame.mono);
    assert(api->export_close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(solar_os_native_handle_close_owner(&app) == SOLAR_OS_NATIVE_ERROR_BUSY && !stops);
    assert(api->frame_release(handle) == SOLAR_OS_NATIVE_OK && !leased);
    assert(api->frame_release(handle) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(solar_os_native_handle_close_owner(&app) == SOLAR_OS_NATIVE_OK && !exporting && stops == 1);
    assert(api->export_close(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !allocations);
    char mode[16];
    assert(api->get_mode("display0", mode, sizeof(mode), NULL, 0) == SOLAR_OS_NATIVE_OK);
    assert(!strcmp(mode, "fixture"));
    assert(api->set_mode("display0", "refresh=auto") == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    solar_os_display_theme_v1_t theme_snapshot;
    assert(!api->get_theme(&theme_snapshot) && theme_snapshot.foreground_rgb888 == 0x112233);
    assert(!api->set_theme_color(SOLAR_OS_DISPLAY_THEME_BACKGROUND, 0x445566));
    assert(theme_snapshot.background_rgb888 == 0xffffff);
    assert(!api->get_theme(&theme_snapshot) && theme_snapshot.background_rgb888 == 0x445566);
    const solar_os_native_display_layout_api_v1_t *layouts =
        solar_os_native_display_get_service("display.layout", 1, sizeof(*layouts));
    assert(layouts && layouts->count() == 0);
    const char *const members[] = {"top", "bottom"};
    const solar_os_display_layout_request_v1_t request = {
        .kind = SOLAR_OS_DISPLAY_LAYOUT_SPLIT, .axis = SOLAR_OS_DISPLAY_LAYOUT_VERTICAL,
        .name = "display0", .member_count = 2, .members = members,
    };
    assert(layouts->create(&request, NULL, 0) == SOLAR_OS_NATIVE_OK && layouts->count() == 1);
    solar_os_display_layout_info_t layout;
    assert(layouts->info(0, &layout) == SOLAR_OS_NATIVE_OK && layout.backing_count == 1);
    assert(!strcmp(layouts->axis_name(layout.axis), "vertical"));
    assert(layouts->find(request.kind, request.name, &layout) == SOLAR_OS_NATIVE_OK);
    assert(layouts->remove(request.kind, request.name) == SOLAR_OS_NATIVE_OK && layouts->count() == 0);
    assert(!strcmp(layout.name, "display0"));
    assert(layouts->info(0, &layout) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !layout.name[0]);
    const solar_os_native_display_lease_api_v1_t *display_lease =
        solar_os_native_display_get_service("display.lease", 1, sizeof(*display_lease));
    assert(display_lease);
    fail_allocation = true;
    assert(display_lease->open("display0", "draw", true, &handle, NULL, 0) ==
           SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !handle && !allocations);
    fail_allocation = false;
    assert(display_lease->open("missing", "draw", true, &handle, NULL, 0) ==
           SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !handle && !allocations);
    assert(!display_lease->open("display0", "draw", true, &handle, NULL, 0));
    char busy_owner[32];
    assert(display_lease->open("display0", "draw", true, &duplicate, busy_owner, sizeof(busy_owner)) ==
           SOLAR_OS_NATIVE_ERROR_BUSY && !duplicate && !strcmp(busy_owner, display_owner));
    const uint32_t mask = SOLAR_OS_NATIVE_DISPLAY_LEASE_BLOCK_TERMINAL |
        SOLAR_OS_NATIVE_DISPLAY_LEASE_LOCK_ORIENTATION;
    assert(!display_lease->set_policy(handle, mask, SOLAR_OS_NATIVE_DISPLAY_LEASE_BLOCK_TERMINAL));
    assert(last_mask == mask && last_values == SOLAR_OS_NATIVE_DISPLAY_LEASE_BLOCK_TERMINAL);
    solar_os_gfx_t *gfx;
    assert(!display_lease->gfx_acquire(handle, &gfx) && gfx == &fixture_gfx);
    assert(display_lease->gfx_acquire(handle, &gfx) == SOLAR_OS_NATIVE_ERROR_BUSY && !gfx);
    assert(display_lease->close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    solar_os_native_display_maintenance_v1_t maintenance;
    size_t cancellation_checks = 0;
    assert(display_lease->relax(handle, 5, cancel_maintenance, &cancellation_checks, &maintenance) ==
           SOLAR_OS_NATIVE_ERROR_BUSY && !cancellation_checks && !maintenance.cycles);

    assert(solar_os_native_handle_close_owner(&app) == SOLAR_OS_NATIVE_ERROR_BUSY && !claim_releases);
    assert(!display_lease->gfx_release(handle));
    assert(display_lease->gfx_release(handle) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!display_lease->relax(handle, 5, cancel_maintenance, &cancellation_checks, &maintenance));
    assert(maintenance.cycles == 2 && maintenance.cancelled && cancellation_checks == 1);
    assert(display_lease->relax(handle, 0, NULL, NULL, &maintenance) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!maintenance.cycles && !maintenance.cancelled);

    assert(display_lease->set_high_refresh(handle, 65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(display_lease->set_high_refresh(handle, 60) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!display_lease->set_high_refresh(handle, 255) && refresh_rate == 255);
    fail_refresh_restore = true;
    assert(display_lease->close(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(display_owner[0] && !claim_releases && allocations == 1 && refresh_rate == 255);
    fail_refresh_restore = false;
    fail_release = true;
    assert(display_lease->close(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(display_owner[0] && !claim_releases && allocations == 1);
    fail_release = false;
    assert(!solar_os_native_handle_close_owner(&app) && claim_releases == 1);
    assert(!display_owner[0] && !last_mask && !last_values && !allocations && !refresh_rate);
    assert(!display_lease->open("display0", "without-gfx", false, &handle, NULL, 0));
    assert(display_lease->gfx_acquire(handle, &gfx) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !gfx);
    assert(!display_lease->close(handle) && claim_releases == 2 && !allocations);

    assert(!display_lease->open("display0", "present", true, &handle, NULL, 0));
    uint16_t palette[] = {0, 0x1234, 0x4567, 0x89ab};
    solar_os_frame_presenter_options_v1_t options = {.format = SOLAR_OS_DISPLAY_FORMAT_INDEX2,
        .width = 8, .height = 2, .stride = 2, .palette_rgb565 = palette, .palette_size = 3, .fullscreen = true};
    assert(display_lease->frames_open(handle, &options) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    options.palette_size = 4;
    assert(!display_lease->gfx_acquire(handle, &gfx));
    assert(display_lease->frames_open(handle, &options) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!display_lease->gfx_release(handle));
    assert(!display_lease->set_high_refresh(handle, 255));
    options.request_high_refresh = true;
    assert(display_lease->frames_open(handle, &options) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!display_lease->set_high_refresh(handle, 0));
    assert(!display_lease->frames_open(handle, &options) && allocations == 2 && have_frames);
    assert(display_lease->set_high_refresh(handle, 255) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(display_lease->gfx_acquire(handle, &gfx) == SOLAR_OS_NATIVE_ERROR_BUSY && !gfx);
    assert(display_lease->relax(handle, 5, NULL, NULL, &maintenance) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(frames.options.palette_rgb565 != palette);
    palette[1] = 0;
    uint8_t image[4] = {1, 2, 3, 4};
    assert(display_lease->frames_submit(handle, image, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(!display_lease->frames_submit(handle, image, sizeof(image)));
    uint8_t *writing;
    size_t bytes;
    assert(!display_lease->frames_acquire(handle, &writing, &bytes) && bytes == sizeof(image));
    uint8_t *previous = writing;
    assert(!display_lease->frames_exchange(handle, &writing) && writing != previous);
    assert(display_lease->frames_submit(handle, image, 4) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(display_lease->frames_close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(solar_os_native_handle_close_owner(&app) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!display_lease->frames_suspend(handle));
    assert(display_lease->frames_release(handle, previous) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!display_lease->frames_release(handle, writing));
    assert(!display_lease->frames_resume(handle) && !display_lease->frames_redraw(handle));
    solar_os_native_frame_presenter_stats_v1_t stats;
    assert(!display_lease->frames_take_stats(handle, &stats));
    assert(stats.present_us == 123456 && stats.presented_frames == 8 && stats.replaced_frames == 2);
    assert(stats.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    fail_frames_close = true;
    assert(display_lease->close(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && have_frames && allocations == 2);
    assert(display_lease->frames_resume(handle) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    fail_frames_close = false;
    assert(!display_lease->frames_close(handle) && !have_frames && allocations == 1);
    fail_frames_open = fail_frames_close = true;
    palette[1] = 0x1234;
    assert(display_lease->frames_open(handle, &options) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && have_frames);
    assert(display_lease->frames_open(handle, &options) == SOLAR_OS_NATIVE_ERROR_BUSY);
    fail_frames_close = false;
    assert(!solar_os_native_handle_close_owner(&app) && !have_frames && !allocations && !display_owner[0]);

    const solar_os_native_display_layer_api_v1_t *layers = solar_os_native_display_get_service(
        "display.layer", 1, sizeof(*layers));
    assert(layers);
    solar_os_native_handle_t first, second;
    assert(layers->open("missing", "keyboard", &first) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !first);
    assert(layers->open("display0", "keyboard", &first) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !first);
    target.u8g2 = &layer_canvas;
    assert(!layers->open("display0", "keyboard", &first));
    assert(!layers->open("display0", "keyboard", &second) && first != second);
    const solar_os_display_rect_t rectangle = {3, 4, 9, 2};
    uint8_t bits[4] = {0xf0, 0x01, 0xf0, 0x01};
    assert(layers->show(first, &rectangle, bits, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(!layers->show(first, &rectangle, bits, sizeof(bits)) && !memcmp(bits, layer_pixels, 4));
    bits[0] = 0;
    assert(layer_pixels[0] == 0xf0);
    bool visible;
    assert(!layers->visible(first, &visible) && visible);
    assert(!layers->visible(second, &visible) && !visible);
    assert(layers->show(second, &rectangle, bits, sizeof(bits)) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!layers->clear(second) && layer_owner[0]);
    fail_layer_clear = true;
    assert(layers->close(first) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && allocations == 2);
    fail_layer_clear = false;
    assert(!layers->clear(first) && !layer_owner[0]);
    assert(!layers->show(second, &rectangle, bits, sizeof(bits)));
    replace_layer_during_clear = true;
    assert(!solar_os_native_handle_close_owner(&app) && !allocations);
    assert(!strcmp(layer_owner, "screen-keyboard"));

}
''', "-D_GNU_SOURCE", "-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1", "-I", str(ROOT / "src"),
              "-I", str(ROOT / "src/shell"), "-I", str(ROOT / "src/services"),
              "-I", str(ROOT / "components/u8g2/src/clib"),
              "-include", str(ROOT / "tests/host/compat.h"),
              str(ROOT / "src/services/solar_os_native_display.c"),
              str(ROOT / "src/services/solar_os_native_handles.c"),
              "-fsanitize=address,undefined", "-no-pie")

    def test_runtime_command_validation_reset_and_no_persistence(self):
        commands = production_functions((ROOT / "src/services/solar_os_sessions.c").read_text(),
            ("session_set_display_canvas_on_owner", "solar_os_sessions_set_display_canvas"))
        commands += production_functions((ROOT / "src/shell/solar_os_shell_commands.c").read_text(),
                                        ("display_cmd_canvas",))
        run_c(r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_display_geometry.h"
typedef struct { char name[16]; solar_os_display_rect_t panel,canvas; } solar_os_display_target_t;
typedef struct { unsigned scale; } solar_os_terminal_profile_t;
typedef struct { solar_os_display_rect_t native_canvas; uint32_t generation; } solar_os_display_canvas_snapshot_t;
typedef struct { const char *name; const solar_os_display_rect_t *canvas; } session_display_canvas_request_t;
typedef struct { char text[512]; unsigned errors; } solar_os_shell_io_t;
static solar_os_display_geometry_t geometry;
static unsigned mutations,reflows;
static bool owner,busy,fail_reflow,shutting_down;
static bool solar_os_display_find_target(const char *name,solar_os_display_target_t *target) {
    if(strcmp(name,"display0")) return false;
    strcpy(target->name,name);target->panel=geometry.panel;target->canvas=geometry.canvas;return true;
}
static esp_err_t solar_os_display_get_terminal_profile(const char *name,solar_os_terminal_profile_t *profile) {
    if (strcmp(name, "display0")) return ESP_ERR_NOT_FOUND;
    profile->scale=geometry.scale;return ESP_OK;
}
static esp_err_t solar_os_display_set_canvas(const char *name,const solar_os_display_rect_t *canvas,
    solar_os_display_canvas_snapshot_t *previous) {
    (void)name;assert(owner);if(busy)return ESP_ERR_INVALID_STATE;
    previous->native_canvas=geometry.native_canvas;
    previous->generation=1;
    if(!solar_os_display_geometry_set_canvas(&geometry,canvas))return ESP_ERR_INVALID_ARG;
    mutations++;return ESP_OK;
}
static esp_err_t solar_os_display_restore_canvas(const char *name,
    const solar_os_display_canvas_snapshot_t *previous) {
    (void)name;assert(owner && previous->generation==1);
    geometry.native_canvas=previous->native_canvas;
    solar_os_display_geometry_update(&geometry,geometry.rotation,geometry.scale);
    mutations++;return ESP_OK;
}
static esp_err_t session_set_display_terminal_profile_internal(const char *name,const solar_os_terminal_profile_t *p) {
    (void)name;assert(owner && p->scale==geometry.scale);reflows++;
    if (fail_reflow) { fail_reflow=false; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
static bool solar_os_shutdown_requested(void) { return shutting_down; }
static esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*fn)(void *),void *ctx) {
    assert(!owner);owner=true;esp_err_t err=fn(ctx);owner=false;return err;
}
static bool parse_size_arg(const char *s,size_t lo,size_t hi,size_t *out) {
    char *end;unsigned long value=strtoul(s,&end,10);
    if(!*s||*s=='-'||*end||value<lo||value>hi)return false;
    *out=value;return true;
}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...) {
    va_list args;va_start(args,fmt);vsnprintf(io->text,sizeof(io->text),fmt,args);va_end(args);
}
static void solar_os_shell_diag_esp(solar_os_shell_io_t *io,const char *command,int error,void *a,void *b) {
    (void)command;(void)error;(void)a;(void)b;io->errors++;
}
''' + commands + r'''
int main(void) {
    solar_os_shell_io_t io={0};solar_os_display_geometry_init(&geometry,412,412,true);
    char *query[]={"display","canvas","display0"};display_cmd_canvas(&io,3,query);
    assert(!mutations && !reflows && strstr(io.text,"412x412") && strstr(io.text,"width=291"));
    char *custom[]={"display","canvas","display0","61","62","280","270"};
    display_cmd_canvas(&io,7,custom);assert(mutations==1 && reflows==1 && geometry.canvas.y==62);
    char *full[]={"display","canvas","display0","full"};display_cmd_canvas(&io,4,full);
    assert(mutations==2 && reflows==2 && geometry.canvas.width==412 && !geometry.canvas.x);
    char *reset[]={"display","canvas","display0","default"};display_cmd_canvas(&io,4,reset);
    assert(mutations==3 && reflows==3 && geometry.canvas.width==291);
    custom[3]="400";display_cmd_canvas(&io,7,custom);assert(mutations==3 && io.errors==1);
    custom[3]="-1";display_cmd_canvas(&io,7,custom);assert(mutations==3 && strstr(io.text,"usage:"));
    query[2]="missing";display_cmd_canvas(&io,3,query);assert(io.errors==2 && mutations==3);
    busy=true;display_cmd_canvas(&io,4,full);assert(io.errors==3 && mutations==3);
    busy=false;custom[3]="10";display_cmd_canvas(&io,7,custom);assert(geometry.canvas.x==10);
    fail_reflow=true;display_cmd_canvas(&io,4,full);
    assert(mutations==6 && reflows==6 && geometry.canvas.x==10 && io.errors==4);
    shutting_down=true;display_cmd_canvas(&io,4,full);
    assert(mutations==6 && reflows==6 && io.errors==5);
    solar_os_display_geometry_init(&geometry,412,412,true);assert(geometry.canvas.x==60 && geometry.canvas.width==291);
    solar_os_display_geometry_update(&geometry,90,2);
    shutting_down=false;fail_reflow=true;display_cmd_canvas(&io,4,full);
    assert(mutations==8 && reflows==8 && io.errors==6 && geometry.native_canvas.width==291);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "components/u8g2/src/clib"),
              str(ROOT / "src/services/solar_os_display_geometry.c"), "-fsanitize=address,undefined", "-no-pie")

    def test_geometry_and_pixels_with_real_gfx_and_u8g2(self):
        subprocess.run([sys.executable, str(ROOT / "scripts/pack_icon_fonts.py"), "--check"], check=True)
        component = ROOT / "components/u8g2"
        cmake = (component / "CMakeLists.txt").read_text().split("\n)", 1)[0]
        sources = [component / name for name in re.findall(r'"([^"]+\.c)"', cmake)]
        sources += sorted((ROOT / "fonts/build/u8g2").glob("u8g2_font_solar_os_default_*_tf.c"))
        sources += [ROOT / "src/services/solar_os_display_geometry.c"]
        with tempfile.TemporaryDirectory() as directory:
            decoder = Path(directory) / "utf8.c"
            decoder.write_text("#include <stdint.h>\n#include <stddef.h>\n" + production_functions(
                (ROOT / "src/services/solar_os_fonts.c").read_text(), ("solar_os_utf8_next",)))
            binary = Path(directory) / "canvas"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-ffunction-sections", "-fdata-sections",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src"), "-idirafter", str(ROOT / "include"),
                "-I", str(component / "src/clib"),
                str(ROOT / "tests/host/display_canvas_test.c"), str(decoder), *map(str, sources),
                "-Wl,--gc-sections", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
