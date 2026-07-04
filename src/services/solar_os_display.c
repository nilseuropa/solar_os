#include "solar_os_display.h"

#include <string.h>

#include "esp_check.h"
#include "solar_os_board_caps.h"
#include "solar_os_gfx_internal.h"

#if SOLAR_OS_BOARD_HAS_DISPLAY
#include "nvs.h"
#include "solar_os_board_display.h"
#endif

#define DISPLAY_NVS_NAMESPACE "display"
#define DISPLAY_NVS_BRIGHTNESS_KEY "brightness"
#define DISPLAY_DEFAULT_BRIGHTNESS 100U
#define DISPLAY_BOARD_TARGET_NAME "display0"
#define DISPLAY_BOARD_SOURCE "board"
#define DISPLAY_BOARD_ROLE "primary"
#define DISPLAY_CONTROLLER_TEXT_MODE "default"
#define DISPLAY_CONTROLLER_LOW_SHIMMER_MODE "lpm"

typedef struct {
    bool active;
    solar_os_display_target_t target;
    solar_os_gfx_t gfx;
#if SOLAR_OS_BOARD_HAS_DISPLAY
    solar_os_board_display_t *board_display;
#endif
} display_target_slot_t;

static display_target_slot_t display_targets[SOLAR_OS_DISPLAY_TARGET_MAX];

static void display_sync_slot(display_target_slot_t *slot);

#if SOLAR_OS_BOARD_HAS_DISPLAY
static solar_os_board_display_t *display_handle;
static uint8_t display_brightness = DISPLAY_DEFAULT_BRIGHTNESS;

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

static display_target_slot_t *display_find_slot(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (display_targets[i].active && strcmp(display_targets[i].target.name, name) == 0) {
            return &display_targets[i];
        }
    }
    return NULL;
}

static display_target_slot_t *display_find_slot_by_u8g2(u8g2_t *u8g2)
{
    if (u8g2 == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        display_target_slot_t *slot = &display_targets[i];
        if (!slot->active) {
            continue;
        }
        display_sync_slot(slot);
        if (slot->target.u8g2 == u8g2) {
            return slot;
        }
    }
    return NULL;
}

static display_target_slot_t *display_alloc_slot(void)
{
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (!display_targets[i].active) {
            return &display_targets[i];
        }
    }
    return NULL;
}

static bool display_controller_mode_is_auto_pair(const char *mode)
{
    return mode == NULL ||
        strcmp(mode, DISPLAY_CONTROLLER_TEXT_MODE) == 0 ||
        strcmp(mode, DISPLAY_CONTROLLER_LOW_SHIMMER_MODE) == 0;
}

#if SOLAR_OS_BOARD_HAS_DISPLAY
static esp_err_t display_set_slot_controller_mode(display_target_slot_t *slot, const char *mode)
{
    if (slot == NULL || mode == NULL || mode[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot->board_display == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *current = solar_os_board_display_controller_mode(slot->board_display);
    if (current != NULL && strcmp(current, mode) == 0) {
        return ESP_OK;
    }
    return solar_os_board_display_set_controller_mode(slot->board_display, mode);
}
#endif

static void display_sync_slot(display_target_slot_t *slot)
{
#if SOLAR_OS_BOARD_HAS_DISPLAY
    if (slot == NULL || slot->board_display == NULL) {
        return;
    }

    solar_os_board_display_t *display = slot->board_display;
    strlcpy(slot->target.driver,
            solar_os_board_display_driver_name(display),
            sizeof(slot->target.driver));
    strlcpy(slot->target.controller,
            solar_os_board_display_controller(display),
            sizeof(slot->target.controller));
    slot->target.width = solar_os_board_display_width(display);
    slot->target.height = solar_os_board_display_height(display);
    slot->target.ready = solar_os_board_display_ready(display);
    slot->target.brightness_supported = solar_os_board_display_brightness_supported(display);
    slot->target.u8g2 = solar_os_board_display_u8g2(display);
#else
    (void)slot;
#endif
}

static void display_init_slot_gfx(display_target_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }

    solar_os_gfx_init(&slot->gfx, slot->target.u8g2);
    solar_os_gfx_set_black_is_one(&slot->gfx, slot->target.black_is_one);
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
    target.u8g2 = solar_os_board_display_u8g2(display);

    const esp_err_t err = solar_os_display_register_target(&target);
    if (err != ESP_OK) {
        return err;
    }

    display_target_slot_t *slot = display_find_slot(DISPLAY_BOARD_TARGET_NAME);
    if (slot != NULL) {
        slot->board_display = display;
    }
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
    if (target == NULL ||
        !display_target_name_valid(target->name, sizeof(target->name)) ||
        !display_target_name_valid(target->source, sizeof(target->source)) ||
        !display_target_name_valid(target->driver, sizeof(target->driver)) ||
        target->width == 0 ||
        target->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display_find_slot(target->name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    display_target_slot_t *slot = display_alloc_slot();
    if (slot == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->target = *target;
    slot->target.name[sizeof(slot->target.name) - 1] = '\0';
    slot->target.source[sizeof(slot->target.source) - 1] = '\0';
    slot->target.driver[sizeof(slot->target.driver) - 1] = '\0';
    slot->target.controller[sizeof(slot->target.controller) - 1] = '\0';
    slot->target.role[sizeof(slot->target.role) - 1] = '\0';
    slot->target.owner[0] = '\0';
    display_init_slot_gfx(slot);
    return ESP_OK;
}

esp_err_t solar_os_display_unregister_target(const char *name)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    display_target_slot_t *slot = display_find_slot(name);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->target.owner[0] != '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

size_t solar_os_display_target_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        if (display_targets[i].active) {
            count++;
        }
    }
    return count;
}

bool solar_os_display_get_target(size_t index, solar_os_display_target_t *target)
{
    size_t current = 0;
    if (target == NULL) {
        return false;
    }

    for (size_t i = 0; i < SOLAR_OS_DISPLAY_TARGET_MAX; i++) {
        display_target_slot_t *slot = &display_targets[i];
        if (!slot->active) {
            continue;
        }
        if (current++ == index) {
            display_sync_slot(slot);
            *target = slot->target;
            return true;
        }
    }
    return false;
}

bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target)
{
    if (target == NULL) {
        return false;
    }

    display_target_slot_t *slot = display_find_slot(name);
    if (slot == NULL) {
        return false;
    }

    display_sync_slot(slot);
    *target = slot->target;
    return true;
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

    display_target_slot_t *slot = display_find_slot(name);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    display_sync_slot(slot);
    if (!slot->target.ready || slot->target.u8g2 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (slot->target.owner[0] != '\0' && strcmp(slot->target.owner, owner) != 0) {
        if (busy_owner != NULL && busy_owner_len > 0) {
            strlcpy(busy_owner, slot->target.owner, busy_owner_len);
        }
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(slot->target.owner, owner, sizeof(slot->target.owner));
    display_init_slot_gfx(slot);
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

    display_target_slot_t *slot = display_find_slot(name);
    if (slot == NULL) {
        (void)solar_os_display_release(name, owner);
        return ESP_ERR_NOT_FOUND;
    }
    *gfx = &slot->gfx;
    return ESP_OK;
}

esp_err_t solar_os_display_release(const char *name, const char *owner)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        !display_owner_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }

    display_target_slot_t *slot = display_find_slot(name);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->target.owner[0] == '\0') {
        return ESP_OK;
    }
    if (strcmp(slot->target.owner, owner) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    slot->target.owner[0] = '\0';
    display_init_slot_gfx(slot);
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

    display_target_slot_t *slot = display_find_slot(name);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

#if !SOLAR_OS_BOARD_HAS_DISPLAY
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (slot->board_display == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *mode_value = solar_os_board_display_controller_mode(slot->board_display);
    const char *mode_values = solar_os_board_display_controller_mode_values(slot->board_display);
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
#endif
}

esp_err_t solar_os_display_set_controller_mode(const char *name, const char *mode)
{
    if (!display_target_name_valid(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) ||
        mode == NULL ||
        mode[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    display_target_slot_t *slot = display_find_slot(name);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

#if !SOLAR_OS_BOARD_HAS_DISPLAY
    (void)slot;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (slot->board_display == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return display_set_slot_controller_mode(slot, mode);
#endif
}

esp_err_t solar_os_display_request_present_mode(u8g2_t *u8g2,
                                                solar_os_display_present_mode_t mode)
{
    display_target_slot_t *slot = display_find_slot_by_u8g2(u8g2);
    if (slot == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *controller_mode = NULL;
    switch (mode) {
    case SOLAR_OS_DISPLAY_PRESENT_TEXT:
    case SOLAR_OS_DISPLAY_PRESENT_GRAPHICS:
        controller_mode = DISPLAY_CONTROLLER_TEXT_MODE;
        break;
    case SOLAR_OS_DISPLAY_PRESENT_LOW_SHIMMER:
        controller_mode = DISPLAY_CONTROLLER_LOW_SHIMMER_MODE;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_DISPLAY
    (void)controller_mode;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (slot->board_display == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *current = solar_os_board_display_controller_mode(slot->board_display);
    if (!display_controller_mode_is_auto_pair(current)) {
        return ESP_OK;
    }
    return display_set_slot_controller_mode(slot, controller_mode);
#endif
}
