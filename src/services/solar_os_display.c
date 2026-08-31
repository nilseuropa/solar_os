#include "solar_os_display.h"

#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_board_caps.h"
#include "solar_os_gfx_internal.h"
#include "solar_os_memory.h"
#include "solar_os_terminal_preferences.h"

#if SOLAR_OS_BOARD_HAS_DISPLAY
#include "solar_os_board_display.h"
#endif

#define DISPLAY_NVS_NAMESPACE "display"
#define DISPLAY_NVS_BRIGHTNESS_KEY "brightness"
#define DISPLAY_NVS_FOREGROUND_KEY "foreground"
#define DISPLAY_NVS_BACKGROUND_KEY "background"
#define DISPLAY_DEFAULT_BRIGHTNESS 100U
#define DISPLAY_DEFAULT_FOREGROUND_RGB888 0x000000U
#define DISPLAY_DEFAULT_BACKGROUND_RGB888 0xffffffU
#define DISPLAY_BOARD_TARGET_NAME SOLAR_OS_DISPLAY_PRIMARY_TARGET
#define DISPLAY_BOARD_SOURCE "board"
#define DISPLAY_BOARD_ROLE "primary"

typedef struct {
    bool active;
    uint32_t generation;
    size_t refs;
    size_t claim_refs;
    SemaphoreHandle_t present_mutex;
    solar_os_display_target_t target;
    solar_os_gfx_t gfx;
    uint8_t *export_buffer;
    size_t export_buffer_size;
    size_t export_readers;
    uint32_t export_frame_id;
    uint16_t export_native_width;
    uint16_t export_native_height;
    uint16_t export_native_stride;
    solar_os_display_rotation_t export_rotation;
    bool export_enabled;
    bool export_publishing;
    bool overlay_active;
    bool overlay_pending;
    uint8_t *overlay_buffer;
    uint8_t overlay_tile_x;
    uint8_t overlay_tile_y;
    uint8_t overlay_tile_width;
    uint8_t overlay_tile_height;
    bool palette_inverted;
    solar_os_terminal_profile_t terminal_profile;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    solar_os_board_display_t *board_display;
#endif
} display_target_slot_t;

static display_target_slot_t display_targets[SOLAR_OS_DISPLAY_TARGET_MAX];
static portMUX_TYPE display_targets_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t display_foreground_rgb888 = DISPLAY_DEFAULT_FOREGROUND_RGB888;
static uint32_t display_background_rgb888 = DISPLAY_DEFAULT_BACKGROUND_RGB888;
static bool display_colors_loaded;

static bool display_snapshot_slot(size_t slot_index, solar_os_display_target_t *target);
static void display_publish_frame(u8g2_t *u8g2);
static void display_publish_surface(u8g2_t *u8g2,
                                    const solar_os_display_surface_t *surface);

static void display_release_slot_ref(size_t slot_index, uint32_t generation)
{
    portENTER_CRITICAL(&display_targets_lock);
    display_target_slot_t *slot = &display_targets[slot_index];
    if (slot->generation == generation && slot->refs > 0U) {
        slot->refs--;
    }
    portEXIT_CRITICAL(&display_targets_lock);
}

static bool display_overlay_active(size_t slot_index, uint32_t generation)
{
    bool active = false;
    portENTER_CRITICAL(&display_targets_lock);
    const display_target_slot_t *slot = &display_targets[slot_index];
    if (slot->active && slot->generation == generation) {
        active = slot->overlay_active;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return active;
}

static void display_draw_overlay_locked(const display_target_slot_t *slot)
{
    if (slot == NULL || !slot->overlay_active || slot->overlay_buffer == NULL ||
        slot->target.u8g2 == NULL || slot->overlay_tile_width == 0U ||
        slot->overlay_tile_height == 0U) {
        return;
    }

    u8x8_t *u8x8 = u8g2_GetU8x8(slot->target.u8g2);
    const size_t row_size = (size_t)slot->overlay_tile_width * 8U;
    for (uint8_t row = 0U; row < slot->overlay_tile_height; row++) {
        u8x8_DrawTile(u8x8,
                      slot->overlay_tile_x,
                      (uint8_t)(slot->overlay_tile_y + row),
                      slot->overlay_tile_width,
                      slot->overlay_buffer + (size_t)row * row_size);
    }
    u8x8_RefreshDisplay(u8x8);
}

static void display_activate_pending_overlay_locked(display_target_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    bool activate = false;
    portENTER_CRITICAL(&display_targets_lock);
    if (slot->active && slot->overlay_pending) {
        slot->overlay_pending = false;
        slot->overlay_active = true;
        activate = true;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    if (activate) {
        display_draw_overlay_locked(slot);
    }
}

static solar_os_display_rotation_t display_rotation(const u8g2_t *u8g2)
{
    if (u8g2 != NULL) {
        if (u8g2->cb == U8G2_R1) {
            return SOLAR_OS_DISPLAY_ROTATION_90;
        }
        if (u8g2->cb == U8G2_R2) {
            return SOLAR_OS_DISPLAY_ROTATION_180;
        }
        if (u8g2->cb == U8G2_R3) {
            return SOLAR_OS_DISPLAY_ROTATION_270;
        }
    }
    return SOLAR_OS_DISPLAY_ROTATION_0;
}

static void display_frame_dimensions(solar_os_display_rotation_t rotation,
                                     uint16_t native_width,
                                     uint16_t native_height,
                                     uint16_t *width,
                                     uint16_t *height)
{
    const bool swap_axes =
        rotation == SOLAR_OS_DISPLAY_ROTATION_90 ||
        rotation == SOLAR_OS_DISPLAY_ROTATION_270;
    if (width != NULL) {
        *width = swap_axes ? native_height : native_width;
    }
    if (height != NULL) {
        *height = swap_axes ? native_width : native_height;
    }
}

static void display_logical_to_native(solar_os_display_rotation_t rotation,
                                      uint16_t native_width,
                                      uint16_t native_height,
                                      uint16_t x,
                                      uint16_t y,
                                      uint16_t *native_x,
                                      uint16_t *native_y)
{
    switch (rotation) {
    case SOLAR_OS_DISPLAY_ROTATION_90:
        *native_x = (uint16_t)(native_width - 1U - y);
        *native_y = x;
        break;
    case SOLAR_OS_DISPLAY_ROTATION_180:
        *native_x = (uint16_t)(native_width - 1U - x);
        *native_y = (uint16_t)(native_height - 1U - y);
        break;
    case SOLAR_OS_DISPLAY_ROTATION_270:
        *native_x = y;
        *native_y = (uint16_t)(native_height - 1U - x);
        break;
    case SOLAR_OS_DISPLAY_ROTATION_0:
    default:
        *native_x = x;
        *native_y = y;
        break;
    }
}

static uint8_t *display_detach_export_buffer_locked(display_target_slot_t *slot)
{
    if (slot == NULL ||
        slot->export_enabled ||
        slot->export_publishing ||
        slot->export_readers != 0) {
        return NULL;
    }

    uint8_t *buffer = slot->export_buffer;
    slot->export_buffer = NULL;
    slot->export_buffer_size = 0;
    slot->export_native_width = 0;
    slot->export_native_height = 0;
    slot->export_native_stride = 0;
    slot->export_rotation = SOLAR_OS_DISPLAY_ROTATION_0;
    return buffer;
}

static esp_err_t display_save_color(const char *key, uint32_t rgb888)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, key, rgb888);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void display_load_colors(void)
{
    if (display_colors_loaded) {
        return;
    }
    display_colors_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    uint32_t stored = 0;
    if (nvs_get_u32(nvs, DISPLAY_NVS_FOREGROUND_KEY, &stored) == ESP_OK &&
        stored <= 0xffffffU) {
        display_foreground_rgb888 = stored;
    }
    if (nvs_get_u32(nvs, DISPLAY_NVS_BACKGROUND_KEY, &stored) == ESP_OK &&
        stored <= 0xffffffU) {
        display_background_rgb888 = stored;
    }
    nvs_close(nvs);
}

#if SOLAR_OS_BOARD_HAS_DISPLAY
static solar_os_board_display_t *display_handle;
static uint8_t display_brightness = DISPLAY_DEFAULT_BRIGHTNESS;
static bool display_primary_suspended;

static esp_err_t display_board_present_surface(
    void *context,
    const solar_os_display_surface_t *surface)
{
    return solar_os_board_display_present_surface(
        (solar_os_board_display_t *)context, surface);
}

static esp_err_t display_board_present_frame(
    void *context,
    const solar_os_display_raster_t *frame)
{
    return solar_os_board_display_present_frame(
        (solar_os_board_display_t *)context, frame);
}

static esp_err_t display_save_brightness(uint8_t percent)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, DISPLAY_NVS_BRIGHTNESS_KEY, percent);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}
static uint8_t display_load_brightness(void)
{
    nvs_handle_t nvs;
    uint8_t percent = DISPLAY_DEFAULT_BRIGHTNESS;

    if (nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return percent;
    }

    uint8_t stored = DISPLAY_DEFAULT_BRIGHTNESS;
    if (nvs_get_u8(nvs, DISPLAY_NVS_BRIGHTNESS_KEY, &stored) == ESP_OK && stored <= 100) {
        percent = stored;
    }
    nvs_close(nvs);
    return percent;
}
#endif

static bool display_target_name_valid(const char *name, size_t max_len)
{
    return name != NULL && name[0] != '\0' && strnlen(name, max_len) < max_len;
}

static bool display_owner_valid(const char *owner)
{
    return owner != NULL &&
        owner[0] != '\0' &&
        strnlen(owner, SOLAR_OS_DISPLAY_TARGET_OWNER_MAX) < SOLAR_OS_DISPLAY_TARGET_OWNER_MAX;
}

static int display_find_slot_locked(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (display_targets[i].active && strcmp(display_targets[i].target.name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int display_find_slot_by_u8g2_locked(const u8g2_t *u8g2)
{
    if (u8g2 == NULL) {
        return -1;
    }
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (display_targets[i].active && display_targets[i].target.u8g2 == u8g2) {
            return (int)i;
        }
    }
    return -1;
}

static int display_find_slot_by_buffer_locked(const uint8_t *buffer)
{
    if (buffer == NULL) {
        return -1;
    }
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (display_targets[i].active &&
            display_targets[i].target.u8g2 != NULL &&
            u8g2_GetBufferPtr(display_targets[i].target.u8g2) == buffer) {
            return (int)i;
        }
    }
    return -1;
}

static int display_alloc_slot_locked(void)
{
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (!display_targets[i].active) {
            return (int)i;
        }
    }
    return -1;
}

static bool display_snapshot_slot(size_t slot_index, solar_os_display_target_t *target)
{
    if (target == NULL || slot_index >= SOLAR_OS_DISPLAY_TARGET_MAX) {
        return false;
    }

    uint32_t generation = 0;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    solar_os_board_display_t *board_display = NULL;
#endif
    portENTER_CRITICAL(&display_targets_lock);
    display_target_slot_t *slot = &display_targets[slot_index];
    if (!slot->active) {
        portEXIT_CRITICAL(&display_targets_lock);
        return false;
    }
    slot->refs++;
    generation = slot->generation;
    *target = slot->target;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    board_display = slot->board_display;
#endif
    portEXIT_CRITICAL(&display_targets_lock);

#if SOLAR_OS_BOARD_HAS_DISPLAY
    if (board_display != NULL) {
        strlcpy(target->driver,
                solar_os_board_display_driver_name(board_display),
                sizeof(target->driver));
        strlcpy(target->controller,
                solar_os_board_display_controller(board_display),
                sizeof(target->controller));
        target->width = solar_os_board_display_width(board_display);
        target->height = solar_os_board_display_height(board_display);
        target->ready = solar_os_board_display_ready(board_display);
        target->brightness_supported = solar_os_board_display_brightness_supported(board_display);
        target->u8g2 = solar_os_board_display_u8g2(board_display);
    }
#endif

    bool valid = false;
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (slot->active && slot->generation == generation) {
#if SOLAR_OS_BOARD_HAS_DISPLAY
        if (board_display != NULL) {
            strlcpy(slot->target.driver, target->driver, sizeof(slot->target.driver));
            strlcpy(slot->target.controller, target->controller, sizeof(slot->target.controller));
            slot->target.width = target->width;
            slot->target.height = target->height;
            slot->target.ready = target->ready;
            slot->target.brightness_supported = target->brightness_supported;
            slot->target.u8g2 = target->u8g2;
        }
#endif
        *target = slot->target;
        valid = true;
    }
    if (slot->generation == generation && slot->refs > 0) {
        slot->refs--;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return valid;
}

static void display_init_slot_gfx(display_target_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }

    solar_os_gfx_init(&slot->gfx, slot->target.u8g2);
    solar_os_gfx_set_black_is_one(&slot->gfx, slot->target.black_is_one);
    solar_os_gfx_set_palette_inverted(&slot->gfx, slot->palette_inverted);
}

#if SOLAR_OS_BOARD_HAS_DISPLAY
static esp_err_t display_register_board_target(solar_os_board_display_t *display)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, DISPLAY_BOARD_TARGET_NAME, sizeof(target.name));
    strlcpy(target.source, DISPLAY_BOARD_SOURCE, sizeof(target.source));
    strlcpy(target.driver, solar_os_board_display_driver_name(display), sizeof(target.driver));
    strlcpy(target.controller, solar_os_board_display_controller(display), sizeof(target.controller));
    strlcpy(target.role, DISPLAY_BOARD_ROLE, sizeof(target.role));
    target.width = solar_os_board_display_width(display);
    target.height = solar_os_board_display_height(display);
    target.ready = solar_os_board_display_ready(display);
    target.brightness_supported = solar_os_board_display_brightness_supported(display);
    target.surface_formats = solar_os_board_display_surface_formats(display);
    target.frame_formats = solar_os_board_display_frame_formats(display);
    target.preferred_stream_fps =
        solar_os_board_display_preferred_stream_fps(display);
    target.max_stream_pixels_per_second =
        solar_os_board_display_max_stream_pixels_per_second(display);
    target.u8g2 = solar_os_board_display_u8g2(display);
    if (target.surface_formats != 0U) {
        target.surface_context = display;
        target.present_surface = display_board_present_surface;
    }
    if (target.frame_formats != 0U) {
        target.frame_context = display;
        target.present_frame = display_board_present_frame;
    }

    const esp_err_t err = solar_os_display_register_target(&target);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(DISPLAY_BOARD_TARGET_NAME);
    if (slot_index >= 0) {
        display_targets[slot_index].board_display = display;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
}
#endif

esp_err_t solar_os_display_init(solar_os_board_display_t *display)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    (void)display;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    display_handle = display;
    ESP_RETURN_ON_ERROR(display_register_board_target(display), "display", "register board target failed");

    display_load_colors();
    ESP_RETURN_ON_ERROR(solar_os_board_display_set_colors(display_handle,
                                                          display_foreground_rgb888,
                                                          display_background_rgb888),
                        "display",
                        "apply colors failed");

    display_brightness = display_load_brightness();
    const esp_err_t err = solar_os_board_display_set_brightness(display_handle, display_brightness);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    return err;
#endif
}

esp_err_t solar_os_display_register_target(const solar_os_display_target_t *target)
{
    const bool has_any_mode_callback = target != NULL &&
        (target->controller_mode != NULL ||
         target->controller_mode_values != NULL ||
         target->set_controller_mode != NULL);
    const bool has_any_surface_callback = target != NULL &&
        (target->surface_formats != 0U || target->surface_context != NULL ||
         target->present_surface != NULL);
    const bool has_any_frame_callback = target != NULL &&
        (target->frame_formats != 0U || target->frame_context != NULL ||
         target->present_frame != NULL);
    if (target == NULL ||
        !display_target_name_valid(target->name, sizeof(target->name)) ||
        !display_target_name_valid(target->source, sizeof(target->source)) ||
        !display_target_name_valid(target->driver, sizeof(target->driver)) ||
        target->width == 0 ||
        target->height == 0 ||
        target->u8g2 == NULL ||
        u8g2_GetBufferPtr(target->u8g2) == NULL ||
        (has_any_surface_callback &&
         (target->surface_formats == 0U || target->surface_context == NULL ||
          target->present_surface == NULL)) ||
        (has_any_frame_callback &&
         (target->frame_formats == 0U || target->frame_context == NULL ||
          target->present_frame == NULL || target->preferred_stream_fps == 0U)) ||
        (has_any_mode_callback &&
         (target->controller_context == NULL ||
          target->controller_mode == NULL ||
          target->controller_mode_values == NULL ||
          target->set_controller_mode == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }
    SemaphoreHandle_t present_mutex = xSemaphoreCreateMutex();
    if (present_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_terminal_profile_t terminal_profile;
    solar_os_terminal_profile_load_preferences(&terminal_profile);

    portENTER_CRITICAL(&display_targets_lock);
    if (display_find_slot_locked(target->name) >= 0 ||
        display_find_slot_by_u8g2_locked(target->u8g2) >= 0 ||
        display_find_slot_by_buffer_locked(u8g2_GetBufferPtr(target->u8g2)) >= 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        vSemaphoreDelete(present_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const int slot_index = display_alloc_slot_locked();
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        vSemaphoreDelete(present_mutex);
        return ESP_ERR_NO_MEM;
    }

    display_target_slot_t *slot = &display_targets[slot_index];
    const uint32_t generation = slot->generation + 1U;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation != 0 ? generation : 1U;
    slot->active = true;
    slot->present_mutex = present_mutex;
    slot->target = *target;
    slot->target.name[sizeof(slot->target.name) - 1] = '\0';
    slot->target.source[sizeof(slot->target.source) - 1] = '\0';
    slot->target.driver[sizeof(slot->target.driver) - 1] = '\0';
    slot->target.controller[sizeof(slot->target.controller) - 1] = '\0';
    slot->target.role[sizeof(slot->target.role) - 1] = '\0';
    slot->target.owner[0] = '\0';
    slot->target.base_rotation = target->u8g2->cb;
    slot->terminal_profile = terminal_profile;
    slot->palette_inverted = terminal_profile.palette_inverted;
    display_init_slot_gfx(slot);
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
}

esp_err_t solar_os_display_unregister_target(const char *name)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    display_target_slot_t *slot = &display_targets[slot_index];
    if (slot->claim_refs != 0 ||
        slot->refs != 0 ||
        slot->target.owner[0] != '\0' ||
        slot->export_buffer != NULL) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t present_mutex = slot->present_mutex;
    uint8_t *overlay_buffer = slot->overlay_buffer;
    const uint32_t generation = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
    portEXIT_CRITICAL(&display_targets_lock);
    if (present_mutex != NULL) {
        vSemaphoreDelete(present_mutex);
    }
    solar_os_memory_free(overlay_buffer);
    return ESP_OK;
}

size_t solar_os_display_target_count(void)
{
    size_t count = 0;
    portENTER_CRITICAL(&display_targets_lock);
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (display_targets[i].active) {
            count++;
        }
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return count;
}

bool solar_os_display_get_target(size_t index, solar_os_display_target_t *target)
{
    size_t current = 0;
    if (target == NULL) {
        return false;
    }

    size_t slot_index = SOLAR_OS_DISPLAY_TARGET_MAX;
    portENTER_CRITICAL(&display_targets_lock);
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (!display_targets[i].active) {
            continue;
        }
        if (current++ == index) {
            slot_index = i;
            break;
        }
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return slot_index < SOLAR_OS_DISPLAY_TARGET_MAX && display_snapshot_slot(slot_index, target);
}

bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target)
{
    if (target == NULL) {
        return false;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    portEXIT_CRITICAL(&display_targets_lock);
    if (slot_index < 0) {
        return false;
    }
    return display_snapshot_slot((size_t)slot_index, target);
}

bool solar_os_display_target_name_for_u8g2(const u8g2_t *u8g2,
                                           char *name,
                                           size_t name_len)
{
    if (u8g2 == NULL || name == NULL || name_len == 0) {
        return false;
    }

    name[0] = '\0';
    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_by_u8g2_locked(u8g2);
    if (slot_index >= 0) {
        strlcpy(name, display_targets[slot_index].target.name, name_len);
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return slot_index >= 0;
}

esp_err_t solar_os_display_get_terminal_profile(
    const char *name,
    solar_os_terminal_profile_t *profile)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *profile = display_targets[slot_index].terminal_profile;
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
}

esp_err_t solar_os_display_set_terminal_profile(
    const char *name,
    const solar_os_terminal_profile_t *profile)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        !solar_os_terminal_profile_is_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    display_target_slot_t *slot = &display_targets[slot_index];
    slot->terminal_profile = *profile;
    slot->palette_inverted = profile->palette_inverted;
    solar_os_gfx_set_palette_inverted(&slot->gfx, profile->palette_inverted);
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
}

esp_err_t solar_os_display_claim(const char *name,
                                 const char *owner,
                                 char *busy_owner,
                                 size_t busy_owner_len)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        !display_owner_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (busy_owner != NULL && busy_owner_len > 0) {
        busy_owner[0] = '\0';
    }

    solar_os_display_target_t target;
    if (!solar_os_display_find_target(name, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!target.ready || target.u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    display_target_slot_t *slot = &display_targets[slot_index];
    if (slot->target.owner[0] != '\0' && strcmp(slot->target.owner, owner) != 0) {
        if (busy_owner != NULL && busy_owner_len > 0) {
            strlcpy(busy_owner, slot->target.owner, busy_owner_len);
        }
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const bool first_claim = slot->claim_refs == 0;
    strlcpy(slot->target.owner, owner, sizeof(slot->target.owner));
    slot->claim_refs++;
    if (first_claim) {
        display_init_slot_gfx(slot);
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
}

esp_err_t solar_os_display_open_gfx(const char *name,
                                    const char *owner,
                                    solar_os_gfx_t **gfx,
                                    char *busy_owner,
                                    size_t busy_owner_len)
{
    if (gfx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *gfx = NULL;

    esp_err_t err = solar_os_display_claim(name, owner, busy_owner, busy_owner_len);
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        (void)solar_os_display_release(name, owner);
        return ESP_ERR_NOT_FOUND;
    }
    display_target_slot_t *slot = &display_targets[slot_index];
    solar_os_gfx_t *opened_gfx = &slot->gfx;
    const uint32_t surface_formats = slot->target.surface_formats;
    *gfx = opened_gfx;
    portEXIT_CRITICAL(&display_targets_lock);
    if ((surface_formats & SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT) != 0U) {
        (void)solar_os_gfx_enable_index8(opened_gfx);
    }
    return ESP_OK;
}

esp_err_t solar_os_display_release(const char *name, const char *owner)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        !display_owner_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }

    void *surface_storage = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    display_target_slot_t *slot = &display_targets[slot_index];
    if (slot->target.owner[0] == '\0') {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_OK;
    }
    if (strcmp(slot->target.owner, owner) != 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (slot->claim_refs > 0) {
        slot->claim_refs--;
    }
    if (slot->claim_refs == 0) {
        slot->target.owner[0] = '\0';
        surface_storage = solar_os_gfx_detach_surface_storage(&slot->gfx);
        display_init_slot_gfx(slot);
    }
    portEXIT_CRITICAL(&display_targets_lock);
    solar_os_memory_free(surface_storage);
    return ESP_OK;
}

bool solar_os_display_brightness_supported(void)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY || !SOLAR_OS_BOARD_HAS_DISPLAY_BRIGHTNESS
    return false;
#else
    return display_handle != NULL &&
        solar_os_board_display_brightness_supported(display_handle);
#endif
}

esp_err_t solar_os_display_get_brightness(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    *percent = 0;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display_handle == NULL) {
        *percent = 0;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = solar_os_board_display_get_brightness(display_handle, percent);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        *percent = display_brightness;
    }
    return err;
#endif
}

esp_err_t solar_os_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = solar_os_board_display_set_brightness(display_handle, percent);
    if (ret != ESP_OK) {
        return ret;
    }

    display_brightness = percent;
    ret = display_save_brightness(percent);
    return ret;
#endif
}

esp_err_t solar_os_display_get_colors(uint32_t *foreground_rgb888,
                                      uint32_t *background_rgb888)
{
    if (foreground_rgb888 == NULL && background_rgb888 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    display_load_colors();
    if (foreground_rgb888 != NULL) {
        *foreground_rgb888 = display_foreground_rgb888;
    }
    if (background_rgb888 != NULL) {
        *background_rgb888 = display_background_rgb888;
    }
    return ESP_OK;
}

esp_err_t solar_os_display_set_foreground_color(uint32_t rgb888)
{
    if (rgb888 > 0xffffffU) {
        return ESP_ERR_INVALID_ARG;
    }
    display_load_colors();

#if SOLAR_OS_BOARD_HAS_DISPLAY
    if (display_handle != NULL) {
        const esp_err_t err = solar_os_board_display_set_colors(display_handle,
                                                                rgb888,
                                                                display_background_rgb888);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif
    display_foreground_rgb888 = rgb888;
    return display_save_color(DISPLAY_NVS_FOREGROUND_KEY, rgb888);
}

esp_err_t solar_os_display_set_background_color(uint32_t rgb888)
{
    if (rgb888 > 0xffffffU) {
        return ESP_ERR_INVALID_ARG;
    }
    display_load_colors();

#if SOLAR_OS_BOARD_HAS_DISPLAY
    if (display_handle != NULL) {
        const esp_err_t err = solar_os_board_display_set_colors(display_handle,
                                                                display_foreground_rgb888,
                                                                rgb888);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif
    display_background_rgb888 = rgb888;
    return display_save_color(DISPLAY_NVS_BACKGROUND_KEY, rgb888);
}

esp_err_t solar_os_display_suspend_primary(void)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display_handle == NULL || display_handle->u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&display_targets_lock);
    const bool already_suspended = display_primary_suspended;
    display_primary_suspended = true;
    portEXIT_CRITICAL(&display_targets_lock);
    if (already_suspended) {
        return ESP_OK;
    }

    u8g2_SetPowerSave(display_handle->u8g2, 1);
    return ESP_OK;
#endif
}

esp_err_t solar_os_display_resume_primary(void)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (display_handle == NULL || display_handle->u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&display_targets_lock);
    const bool suspended = display_primary_suspended;
    portEXIT_CRITICAL(&display_targets_lock);
    if (!suspended) {
        return ESP_OK;
    }

    u8g2_SetPowerSave(display_handle->u8g2, 0);
    portENTER_CRITICAL(&display_targets_lock);
    display_primary_suspended = false;
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
#endif
}

bool solar_os_display_primary_suspended(void)
{
#if !SOLAR_OS_BOARD_HAS_DISPLAY
    return false;
#else
    portENTER_CRITICAL(&display_targets_lock);
    const bool suspended = display_primary_suspended;
    portEXIT_CRITICAL(&display_targets_lock);
    return suspended;
#endif
}

esp_err_t solar_os_display_set_palette_inverted(const char *name, bool inverted)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }

    display_target_slot_t *slot = &display_targets[slot_index];
    slot->palette_inverted = inverted;
    solar_os_gfx_set_palette_inverted(&slot->gfx, inverted);
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
}

esp_err_t solar_os_display_get_controller_mode(const char *name,
                                               const char **mode,
                                               const char **values)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode != NULL) {
        *mode = NULL;
    }
    if (values != NULL) {
        *values = NULL;
    }

    void *controller_context = NULL;
    solar_os_display_mode_getter_t controller_mode = NULL;
    solar_os_display_mode_getter_t controller_mode_values = NULL;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    solar_os_board_display_t *board_display = NULL;
#endif
    uint32_t generation = 0;
    size_t slot_index = 0;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_locked(name);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    controller_context = slot->target.controller_context;
    controller_mode = slot->target.controller_mode;
    controller_mode_values = slot->target.controller_mode_values;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    board_display = slot->board_display;
#endif
    if (controller_context == NULL &&
        controller_mode == NULL &&
        controller_mode_values == NULL
#if SOLAR_OS_BOARD_HAS_DISPLAY
        && board_display == NULL
#endif
    ) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    generation = slot->generation;
    slot->refs++;
    portEXIT_CRITICAL(&display_targets_lock);

    const char *mode_value = NULL;
    const char *mode_values = NULL;
    if (controller_context != NULL) {
        mode_value = controller_mode(controller_context);
        mode_values = controller_mode_values(controller_context);
#if SOLAR_OS_BOARD_HAS_DISPLAY
    } else {
        mode_value = solar_os_board_display_controller_mode(board_display);
        mode_values = solar_os_board_display_controller_mode_values(board_display);
#endif
    }
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (slot->generation == generation && slot->refs > 0) {
        slot->refs--;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    if (mode_value == NULL || mode_values == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (mode != NULL) {
        *mode = mode_value;
    }
    if (values != NULL) {
        *values = mode_values;
    }
    return ESP_OK;
}

esp_err_t solar_os_display_set_controller_mode(const char *name, const char *mode)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        mode == NULL ||
        mode[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    void *controller_context = NULL;
    solar_os_display_mode_getter_t controller_mode = NULL;
    solar_os_display_mode_setter_t set_controller_mode = NULL;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    solar_os_board_display_t *board_display = NULL;
#endif
    uint32_t generation = 0;
    size_t slot_index = 0;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_locked(name);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    controller_context = slot->target.controller_context;
    controller_mode = slot->target.controller_mode;
    set_controller_mode = slot->target.set_controller_mode;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    board_display = slot->board_display;
#endif
    if (controller_context == NULL &&
        controller_mode == NULL &&
        set_controller_mode == NULL
#if SOLAR_OS_BOARD_HAS_DISPLAY
        && board_display == NULL
#endif
    ) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    generation = slot->generation;
    slot->refs++;
    portEXIT_CRITICAL(&display_targets_lock);

    const char *current = NULL;
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    if (controller_context != NULL) {
        current = controller_mode(controller_context);
        ret = current != NULL && strcmp(current, mode) == 0 ?
            ESP_OK : set_controller_mode(controller_context, mode);
#if SOLAR_OS_BOARD_HAS_DISPLAY
    } else {
        current = solar_os_board_display_controller_mode(board_display);
        ret = current != NULL && strcmp(current, mode) == 0 ?
            ESP_OK : solar_os_board_display_set_controller_mode(board_display, mode);
#endif
    }
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (slot->generation == generation && slot->refs > 0) {
        slot->refs--;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return ret;
}

esp_err_t solar_os_display_set_high_refresh_override(const char *name,
                                                     bool enabled,
                                                     uint16_t hz_tenths)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        (enabled && hz_tenths == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_DISPLAY
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_board_display_t *board_display = NULL;
    uint32_t generation = 0;
    size_t slot_index = 0;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_locked(name);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    board_display = slot->board_display;
    if (board_display == NULL) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    generation = slot->generation;
    slot->refs++;
    portEXIT_CRITICAL(&display_targets_lock);

    const esp_err_t ret = solar_os_board_display_set_high_refresh_override(
        board_display, enabled, hz_tenths);
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (slot->generation == generation && slot->refs > 0) {
        slot->refs--;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    return ret;
#endif
}

esp_err_t solar_os_display_request_present_mode(u8g2_t *u8g2,
                                                solar_os_display_present_mode_t mode)
{
    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_by_u8g2_locked(u8g2);
    portEXIT_CRITICAL(&display_targets_lock);
    if (slot_index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    switch (mode) {
    case SOLAR_OS_DISPLAY_PRESENT_TEXT:
    case SOLAR_OS_DISPLAY_PRESENT_GRAPHICS:
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t solar_os_display_set_overlay_active(u8g2_t *u8g2, bool active)
{
    if (u8g2 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t slot_index = 0U;
    uint32_t generation = 0U;
    SemaphoreHandle_t present_mutex = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    generation = slot->generation;
    present_mutex = slot->present_mutex;
    slot->refs++;
    portEXIT_CRITICAL(&display_targets_lock);

    if (present_mutex == NULL ||
        xSemaphoreTake(present_mutex, portMAX_DELAY) != pdTRUE) {
        display_release_slot_ref(slot_index, generation);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *overlay_buffer = NULL;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (slot->active && slot->generation == generation) {
        slot->overlay_active = active;
        slot->overlay_pending = false;
        if (!active) {
            overlay_buffer = slot->overlay_buffer;
            slot->overlay_buffer = NULL;
            slot->overlay_tile_x = 0U;
            slot->overlay_tile_y = 0U;
            slot->overlay_tile_width = 0U;
            slot->overlay_tile_height = 0U;
        }
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&display_targets_lock);
    xSemaphoreGive(present_mutex);
    display_release_slot_ref(slot_index, generation);
    solar_os_memory_free(overlay_buffer);
    return ret;
}

esp_err_t solar_os_display_present_mono_xbm(u8g2_t *u8g2,
                                            const uint8_t *bitmap,
                                            size_t bitmap_size,
                                            uint16_t x,
                                            uint16_t y,
                                            uint16_t width,
                                            uint16_t height,
                                            uint16_t stride,
                                            bool palette_inverted)
{
    if (u8g2 == NULL || bitmap == NULL || width == 0 || height == 0 || stride == 0) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_DISPLAY
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_board_display_t *board_display = NULL;
    SemaphoreHandle_t present_mutex = NULL;
    uint32_t generation = 0;
    size_t slot_index = 0;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    board_display = slot->board_display;
    if (board_display == NULL) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (display_primary_suspended && board_display == display_handle) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_OK;
    }
    if (slot->overlay_active) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_OK;
    }
    generation = slot->generation;
    present_mutex = slot->present_mutex;
    slot->refs++;
    portEXIT_CRITICAL(&display_targets_lock);

    if (present_mutex == NULL ||
        xSemaphoreTake(present_mutex, portMAX_DELAY) != pdTRUE) {
        display_release_slot_ref(slot_index, generation);
        return ESP_ERR_INVALID_STATE;
    }
    if (display_overlay_active(slot_index, generation)) {
        xSemaphoreGive(present_mutex);
        display_release_slot_ref(slot_index, generation);
        return ESP_OK;
    }
    const esp_err_t ret = solar_os_board_display_present_mono_xbm(
        board_display,
        bitmap,
        bitmap_size,
        x,
        y,
        width,
        height,
        stride,
        palette_inverted);
    if (ret == ESP_OK) {
        display_activate_pending_overlay_locked(&display_targets[slot_index]);
    }
    xSemaphoreGive(present_mutex);
    if (ret == ESP_OK) {
        display_publish_frame(u8g2);
    }

    display_release_slot_ref(slot_index, generation);
    return ret;
#endif
}

esp_err_t solar_os_display_present_surface(
    u8g2_t *u8g2,
    const solar_os_display_surface_t *surface)
{
    if (u8g2 == NULL || surface == NULL || surface->data == NULL ||
        surface->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t slot_index = 0;
    uint32_t generation = 0;
    void *surface_context = NULL;
    solar_os_display_surface_presenter_t presenter = NULL;
    SemaphoreHandle_t present_mutex = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    if ((slot->target.surface_formats &
         SOLAR_OS_DISPLAY_FORMAT_BIT(surface->format)) == 0U ||
        slot->target.present_surface == NULL ||
        slot->target.surface_context == NULL) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
#if SOLAR_OS_BOARD_HAS_DISPLAY
    if (display_primary_suspended && slot->board_display == display_handle) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (slot->overlay_active) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_OK;
    }
    generation = slot->generation;
    surface_context = slot->target.surface_context;
    presenter = slot->target.present_surface;
    present_mutex = slot->present_mutex;
    slot->refs++;
    portEXIT_CRITICAL(&display_targets_lock);

    if (present_mutex == NULL ||
        xSemaphoreTake(present_mutex, portMAX_DELAY) != pdTRUE) {
        display_release_slot_ref(slot_index, generation);
        return ESP_ERR_INVALID_STATE;
    }
    if (display_overlay_active(slot_index, generation)) {
        xSemaphoreGive(present_mutex);
        display_release_slot_ref(slot_index, generation);
        return ESP_OK;
    }
    const esp_err_t ret = presenter(surface_context, surface);
    if (ret == ESP_OK) {
        display_activate_pending_overlay_locked(&display_targets[slot_index]);
    }
    xSemaphoreGive(present_mutex);
    if (ret == ESP_OK) {
        display_publish_surface(u8g2, surface);
    }

    display_release_slot_ref(slot_index, generation);
    return ret;
}

esp_err_t solar_os_display_present_frame(
    u8g2_t *u8g2,
    const solar_os_display_raster_t *frame)
{
    if (u8g2 == NULL || frame == NULL || frame->data == NULL ||
        frame->source_width == 0U || frame->source_height == 0U ||
        frame->source_stride == 0U || frame->width == 0U ||
        frame->height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t minimum_stride = 0U;
    size_t palette_entries = 0U;
    switch (frame->format) {
    case SOLAR_OS_DISPLAY_FORMAT_MONO1:
        minimum_stride = ((size_t)frame->source_width + 7U) / 8U;
        palette_entries = 2U;
        break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX2:
        minimum_stride = ((size_t)frame->source_width + 3U) / 4U;
        palette_entries = 4U;
        if (frame->palette_rgb565 == NULL || frame->palette_size < 4U) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    case SOLAR_OS_DISPLAY_FORMAT_INDEX8:
        minimum_stride = frame->source_width;
        palette_entries = 256U;
        if (frame->palette_rgb565 == NULL || frame->palette_size < 256U) {
            return ESP_ERR_INVALID_ARG;
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (frame->source_stride < minimum_stride ||
        frame->source_height > SIZE_MAX / frame->source_stride ||
        frame->data_size < (size_t)frame->source_height * frame->source_stride ||
        (frame->clear_background &&
         (size_t)frame->background_index >= palette_entries)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t slot_index = 0U;
    uint32_t generation = 0U;
    void *frame_context = NULL;
    solar_os_display_frame_presenter_t presenter = NULL;
    SemaphoreHandle_t present_mutex = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    if ((slot->target.frame_formats &
         SOLAR_OS_DISPLAY_FORMAT_BIT(frame->format)) == 0U ||
        slot->target.present_frame == NULL || slot->target.frame_context == NULL ||
        (uint32_t)frame->x + frame->width > u8g2_GetDisplayWidth(u8g2) ||
        (uint32_t)frame->y + frame->height > u8g2_GetDisplayHeight(u8g2)) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (slot->overlay_active) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_OK;
    }
#if SOLAR_OS_BOARD_HAS_DISPLAY
    if (display_primary_suspended && slot->board_display == display_handle) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    generation = slot->generation;
    frame_context = slot->target.frame_context;
    presenter = slot->target.present_frame;
    present_mutex = slot->present_mutex;
    slot->refs++;
    portEXIT_CRITICAL(&display_targets_lock);

    if (present_mutex == NULL ||
        xSemaphoreTake(present_mutex, portMAX_DELAY) != pdTRUE) {
        display_release_slot_ref(slot_index, generation);
        return ESP_ERR_INVALID_STATE;
    }
    if (display_overlay_active(slot_index, generation)) {
        xSemaphoreGive(present_mutex);
        display_release_slot_ref(slot_index, generation);
        return ESP_OK;
    }
    const esp_err_t ret = presenter(frame_context, frame);
    if (ret == ESP_OK) {
        display_activate_pending_overlay_locked(&display_targets[slot_index]);
    }
    xSemaphoreGive(present_mutex);
    display_release_slot_ref(slot_index, generation);
    return ret;
}

static void display_publish_frame(u8g2_t *u8g2)
{
    if (u8g2 == NULL || u8g2_GetBufferPtr(u8g2) == NULL) {
        return;
    }

    size_t slot_index = 0;
    uint32_t generation = 0;
    uint8_t *export_buffer = NULL;
    size_t export_buffer_size = 0;

    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    if (!slot->export_enabled ||
        slot->export_buffer == NULL ||
        slot->export_publishing ||
        slot->export_readers != 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return;
    }

    slot->export_publishing = true;
    slot->refs++;
    generation = slot->generation;
    export_buffer = slot->export_buffer;
    export_buffer_size = slot->export_buffer_size;
    portEXIT_CRITICAL(&display_targets_lock);

    memcpy(export_buffer, u8g2_GetBufferPtr(u8g2), export_buffer_size);

    uint8_t *free_buffer = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (slot->generation == generation &&
        slot->export_buffer == export_buffer &&
        slot->export_publishing) {
        slot->export_publishing = false;
        if (slot->export_enabled) {
            slot->export_frame_id++;
            if (slot->export_frame_id == 0) {
                slot->export_frame_id = 1;
            }
        }
        free_buffer = display_detach_export_buffer_locked(slot);
    }
    if (slot->generation == generation && slot->refs > 0) {
        slot->refs--;
    }
    portEXIT_CRITICAL(&display_targets_lock);

    solar_os_memory_free(free_buffer);
}

static void display_surface_logical_to_native(
    const solar_os_display_surface_t *surface,
    uint16_t x,
    uint16_t y,
    uint16_t *native_x,
    uint16_t *native_y)
{
    switch (surface->rotation) {
    case SOLAR_OS_DISPLAY_ROTATION_90:
        *native_x = (uint16_t)(surface->native_width - 1U - y);
        *native_y = x;
        break;
    case SOLAR_OS_DISPLAY_ROTATION_180:
        *native_x = (uint16_t)(surface->native_width - 1U - x);
        *native_y = (uint16_t)(surface->native_height - 1U - y);
        break;
    case SOLAR_OS_DISPLAY_ROTATION_270:
        *native_x = y;
        *native_y = (uint16_t)(surface->native_height - 1U - x);
        break;
    case SOLAR_OS_DISPLAY_ROTATION_0:
    default:
        *native_x = x;
        *native_y = y;
        break;
    }
}

static void display_publish_surface(u8g2_t *u8g2,
                                    const solar_os_display_surface_t *surface)
{
    static const uint8_t bayer4[4][4] = {
        {0U, 8U, 2U, 10U}, {12U, 4U, 14U, 6U},
        {3U, 11U, 1U, 9U}, {15U, 7U, 13U, 5U},
    };
    if (u8g2 == NULL || surface == NULL || surface->format !=
        SOLAR_OS_DISPLAY_FORMAT_INDEX8 || surface->palette_rgb565 == NULL) {
        return;
    }

    size_t slot_index = 0;
    uint32_t generation = 0;
    uint8_t *export_buffer = NULL;
    size_t export_buffer_size = 0;
    uint16_t native_stride = 0;
    bool black_is_one = false;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    if (!slot->export_enabled || slot->export_buffer == NULL ||
        slot->export_publishing || slot->export_readers != 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return;
    }
    slot->export_publishing = true;
    slot->refs++;
    generation = slot->generation;
    export_buffer = slot->export_buffer;
    export_buffer_size = slot->export_buffer_size;
    native_stride = slot->export_native_stride;
    black_is_one = slot->target.black_is_one;
    portEXIT_CRITICAL(&display_targets_lock);

    memset(export_buffer, 0, export_buffer_size);
    for (uint16_t y = 0; y < surface->height; y++) {
        for (uint16_t x = 0; x < surface->width; x++) {
            const uint8_t palette_index =
                surface->data[(size_t)y * surface->stride + x];
            const uint16_t rgb565 = surface->palette_rgb565[palette_index];
            const unsigned red = ((rgb565 >> 11U) & 0x1fU) * 255U / 31U;
            const unsigned green = ((rgb565 >> 5U) & 0x3fU) * 255U / 63U;
            const unsigned blue = (rgb565 & 0x1fU) * 255U / 31U;
            const unsigned luminance = (red * 77U + green * 150U + blue * 29U) >> 8U;
            const unsigned threshold = (luminance * 16U + 127U) / 255U;
            const bool white = bayer4[y & 3U][x & 3U] < threshold;
            const bool set = black_is_one ? !white : white;
            if (!set) {
                continue;
            }
            uint16_t native_x = 0;
            uint16_t native_y = 0;
            display_surface_logical_to_native(surface, x, y,
                                               &native_x, &native_y);
            const size_t offset = (size_t)(native_y / 8U) * native_stride +
                native_x;
            if (offset < export_buffer_size) {
                export_buffer[offset] |= (uint8_t)(1U << (native_y & 7U));
            }
        }
    }

    uint8_t *free_buffer = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (slot->generation == generation && slot->export_buffer == export_buffer &&
        slot->export_publishing) {
        slot->export_publishing = false;
        if (slot->export_enabled) {
            slot->export_frame_id++;
            if (slot->export_frame_id == 0) slot->export_frame_id = 1;
        }
        free_buffer = display_detach_export_buffer_locked(slot);
    }
    if (slot->generation == generation && slot->refs > 0) slot->refs--;
    portEXIT_CRITICAL(&display_targets_lock);
    solar_os_memory_free(free_buffer);
}

void solar_os_display_present(u8g2_t *u8g2, solar_os_display_present_mode_t mode)
{
    if (u8g2 == NULL) {
        return;
    }
    (void)solar_os_display_request_present_mode(u8g2, mode);
    display_publish_frame(u8g2);

    size_t slot_index = 0U;
    uint32_t generation = 0U;
    SemaphoreHandle_t present_mutex = NULL;
    bool suspended = false;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index >= 0) {
        slot_index = (size_t)found_index;
        display_target_slot_t *slot = &display_targets[slot_index];
#if SOLAR_OS_BOARD_HAS_DISPLAY
        suspended =
            display_targets[found_index].board_display == display_handle &&
            display_primary_suspended;
#endif
        if (slot->overlay_active) {
            portEXIT_CRITICAL(&display_targets_lock);
            return;
        }
        if (!suspended) {
            generation = slot->generation;
            present_mutex = slot->present_mutex;
            slot->refs++;
        }
    }
    portEXIT_CRITICAL(&display_targets_lock);
    if (suspended) {
        return;
    }

    if (present_mutex != NULL &&
        xSemaphoreTake(present_mutex, portMAX_DELAY) != pdTRUE) {
        display_release_slot_ref(slot_index, generation);
        return;
    }
    if (present_mutex != NULL &&
        display_overlay_active(slot_index, generation)) {
        xSemaphoreGive(present_mutex);
        display_release_slot_ref(slot_index, generation);
        return;
    }
    u8g2_SendBuffer(u8g2);
    if (present_mutex != NULL) {
        display_activate_pending_overlay_locked(&display_targets[slot_index]);
        xSemaphoreGive(present_mutex);
        display_release_slot_ref(slot_index, generation);
    }
}

void solar_os_display_present_overlay(u8g2_t *u8g2,
                                      uint16_t x,
                                      uint16_t y,
                                      uint16_t width,
                                      uint16_t height,
                                      bool after_next_frame)
{
    if (u8g2 == NULL || width == 0U || height == 0U ||
        (uint32_t)x + width > u8g2_GetDisplayWidth(u8g2) ||
        (uint32_t)y + height > u8g2_GetDisplayHeight(u8g2)) {
        return;
    }

    const uint16_t native_width =
        (uint16_t)u8g2_GetBufferTileWidth(u8g2) * 8U;
    const uint16_t native_height =
        (uint16_t)u8g2_GetBufferTileHeight(u8g2) * 8U;
    const solar_os_display_rotation_t rotation = display_rotation(u8g2);
    const uint16_t logical_x1 = (uint16_t)(x + width - 1U);
    const uint16_t logical_y1 = (uint16_t)(y + height - 1U);
    uint16_t native_x[4];
    uint16_t native_y[4];
    display_logical_to_native(rotation, native_width, native_height,
                              x, y, &native_x[0], &native_y[0]);
    display_logical_to_native(rotation, native_width, native_height,
                              logical_x1, y, &native_x[1], &native_y[1]);
    display_logical_to_native(rotation, native_width, native_height,
                              x, logical_y1, &native_x[2], &native_y[2]);
    display_logical_to_native(rotation, native_width, native_height,
                              logical_x1, logical_y1,
                              &native_x[3], &native_y[3]);
    uint16_t native_x0 = native_x[0];
    uint16_t native_y0 = native_y[0];
    uint16_t native_x1 = native_x[0];
    uint16_t native_y1 = native_y[0];
    for (size_t corner = 1U; corner < 4U; corner++) {
        if (native_x[corner] < native_x0) native_x0 = native_x[corner];
        if (native_y[corner] < native_y0) native_y0 = native_y[corner];
        if (native_x[corner] > native_x1) native_x1 = native_x[corner];
        if (native_y[corner] > native_y1) native_y1 = native_y[corner];
    }
    const uint8_t tile_x = (uint8_t)(native_x0 / 8U);
    const uint8_t tile_y = (uint8_t)(native_y0 / 8U);
    const uint8_t tile_width =
        (uint8_t)(native_x1 / 8U - tile_x + 1U);
    const uint8_t tile_height =
        (uint8_t)(native_y1 / 8U - tile_y + 1U);

    const size_t overlay_row_size = (size_t)tile_width * 8U;
    const size_t overlay_size = overlay_row_size * tile_height;
    uint8_t *overlay_buffer = solar_os_memory_alloc(
        overlay_size, SOLAR_OS_MEMORY_TRANSIENT, "display.overlay");
    if (overlay_buffer == NULL) {
        return;
    }
    const size_t source_row_size = (size_t)u8g2->pixel_buf_width;
    const uint8_t *source = u8g2_GetBufferPtr(u8g2) +
        (size_t)tile_y * source_row_size + (size_t)tile_x * 8U;
    for (uint8_t row = 0U; row < tile_height; row++) {
        memcpy(overlay_buffer + (size_t)row * overlay_row_size,
               source + (size_t)row * source_row_size,
               overlay_row_size);
    }

    (void)solar_os_display_request_present_mode(
        u8g2, SOLAR_OS_DISPLAY_PRESENT_TEXT);
    size_t slot_index = 0U;
    uint32_t generation = 0U;
    SemaphoreHandle_t present_mutex = NULL;
    bool suspended = false;
    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_by_u8g2_locked(u8g2);
    if (found_index >= 0) {
        slot_index = (size_t)found_index;
        display_target_slot_t *slot = &display_targets[slot_index];
#if SOLAR_OS_BOARD_HAS_DISPLAY
        suspended =
            slot->board_display == display_handle && display_primary_suspended;
#endif
        if (!suspended) {
            generation = slot->generation;
            present_mutex = slot->present_mutex;
            slot->refs++;
        }
    }
    portEXIT_CRITICAL(&display_targets_lock);
    if (suspended) {
        solar_os_memory_free(overlay_buffer);
        return;
    }
    if (present_mutex != NULL &&
        xSemaphoreTake(present_mutex, portMAX_DELAY) != pdTRUE) {
        display_release_slot_ref(slot_index, generation);
        solar_os_memory_free(overlay_buffer);
        return;
    }
    if (present_mutex != NULL) {
        uint8_t *previous_overlay = NULL;
        portENTER_CRITICAL(&display_targets_lock);
        display_target_slot_t *slot = &display_targets[slot_index];
        if (slot->active && slot->generation == generation) {
            previous_overlay = slot->overlay_buffer;
            slot->overlay_buffer = overlay_buffer;
            slot->overlay_tile_x = tile_x;
            slot->overlay_tile_y = tile_y;
            slot->overlay_tile_width = tile_width;
            slot->overlay_tile_height = tile_height;
            slot->overlay_active = !after_next_frame;
            slot->overlay_pending = after_next_frame;
            overlay_buffer = NULL;
        }
        portEXIT_CRITICAL(&display_targets_lock);
        solar_os_memory_free(previous_overlay);
        if (!after_next_frame) {
            display_draw_overlay_locked(&display_targets[slot_index]);
        }
        xSemaphoreGive(present_mutex);
        display_release_slot_ref(slot_index, generation);
    }
    solar_os_memory_free(overlay_buffer);
}

esp_err_t solar_os_display_start_frame_export(const char *name)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t slot_index = 0;
    uint32_t generation = 0;
    u8g2_t *u8g2 = NULL;
    bool black_is_one = false;

    portENTER_CRITICAL(&display_targets_lock);
    const int found_index = display_find_slot_locked(name);
    if (found_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    slot_index = (size_t)found_index;
    display_target_slot_t *slot = &display_targets[slot_index];
    if (!slot->target.ready || slot->target.u8g2 == NULL) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (slot->export_buffer != NULL) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }
    slot->refs++;
    generation = slot->generation;
    u8g2 = slot->target.u8g2;
    black_is_one = slot->target.black_is_one;
    portEXIT_CRITICAL(&display_targets_lock);

    const uint16_t tile_width = u8g2_GetBufferTileWidth(u8g2);
    const uint16_t tile_height = u8g2_GetBufferTileHeight(u8g2);
    const size_t buffer_size = (size_t)tile_width * (size_t)tile_height * 8U;
    const uint8_t *source_buffer = u8g2_GetBufferPtr(u8g2);
    const u8x8_display_info_t *display_info = u8g2_GetU8x8(u8g2)->display_info;
    uint8_t *buffer = buffer_size > 0 && source_buffer != NULL && display_info != NULL ?
        solar_os_memory_alloc(buffer_size,
                              SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                              "display.export") :
        NULL;
    if (buffer != NULL) {
        memcpy(buffer, source_buffer, buffer_size);
    }

    esp_err_t ret = buffer != NULL ?
        ESP_OK :
        (buffer_size == 0 || source_buffer == NULL || display_info == NULL ?
            ESP_ERR_INVALID_STATE :
            ESP_ERR_NO_MEM);
    portENTER_CRITICAL(&display_targets_lock);
    slot = &display_targets[slot_index];
    if (ret == ESP_OK &&
        slot->active &&
        slot->generation == generation &&
        slot->target.u8g2 == u8g2 &&
        slot->export_buffer == NULL) {
        slot->export_buffer = buffer;
        slot->export_buffer_size = buffer_size;
        slot->export_frame_id++;
        if (slot->export_frame_id == 0) {
            slot->export_frame_id = 1;
        }
        slot->export_native_width = display_info->pixel_width;
        slot->export_native_height = display_info->pixel_height;
        slot->export_native_stride = tile_width * 8U;
        slot->export_rotation = display_rotation(u8g2);
        slot->export_enabled = true;
        slot->target.black_is_one = black_is_one;
        buffer = NULL;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (slot->generation == generation && slot->refs > 0) {
        slot->refs--;
    }
    portEXIT_CRITICAL(&display_targets_lock);

    solar_os_memory_free(buffer);
    return ret;
}

void solar_os_display_stop_frame_export(const char *name)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX)) {
        return;
    }

    uint8_t *free_buffer = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index >= 0) {
        display_target_slot_t *slot = &display_targets[slot_index];
        slot->export_enabled = false;
        free_buffer = display_detach_export_buffer_locked(slot);
    }
    portEXIT_CRITICAL(&display_targets_lock);
    solar_os_memory_free(free_buffer);
}

esp_err_t solar_os_display_acquire_frame(const char *name, solar_os_display_frame_t *frame)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(frame, 0, sizeof(*frame));

    portENTER_CRITICAL(&display_targets_lock);
    const int slot_index = display_find_slot_locked(name);
    if (slot_index < 0) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_NOT_FOUND;
    }

    display_target_slot_t *slot = &display_targets[slot_index];
    if (!slot->export_enabled || slot->export_buffer == NULL) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (slot->export_publishing) {
        portEXIT_CRITICAL(&display_targets_lock);
        return ESP_ERR_TIMEOUT;
    }

    slot->export_readers++;
    uint16_t width = 0;
    uint16_t height = 0;
    display_frame_dimensions(slot->export_rotation,
                             slot->export_native_width,
                             slot->export_native_height,
                             &width,
                             &height);
    *frame = (solar_os_display_frame_t){
        .data = slot->export_buffer,
        .data_size = slot->export_buffer_size,
        .frame_id = slot->export_frame_id,
        .target_generation = slot->generation,
        .width = width,
        .height = height,
        .native_width = slot->export_native_width,
        .native_height = slot->export_native_height,
        .native_stride = slot->export_native_stride,
        .target_slot = (uint8_t)slot_index,
        .rotation = slot->export_rotation,
        .black_is_one = slot->target.black_is_one,
    };
    portEXIT_CRITICAL(&display_targets_lock);
    return ESP_OK;
}

void solar_os_display_release_frame(solar_os_display_frame_t *frame)
{
    if (frame == NULL || frame->data == NULL || frame->target_slot >= SOLAR_OS_DISPLAY_TARGET_MAX) {
        return;
    }

    uint8_t *free_buffer = NULL;
    portENTER_CRITICAL(&display_targets_lock);
    display_target_slot_t *slot = &display_targets[frame->target_slot];
    if (slot->generation == frame->target_generation &&
        slot->export_buffer == frame->data &&
        slot->export_readers > 0) {
        slot->export_readers--;
        free_buffer = display_detach_export_buffer_locked(slot);
    }
    portEXIT_CRITICAL(&display_targets_lock);

    memset(frame, 0, sizeof(*frame));
    solar_os_memory_free(free_buffer);
}
