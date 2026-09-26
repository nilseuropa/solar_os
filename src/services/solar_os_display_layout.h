#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display.h"
#include "solar_os_display_layout_core.h"

#define SOLAR_OS_DISPLAY_LAYOUT_MAX 3U
#define SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX 4U

typedef enum {
    SOLAR_OS_DISPLAY_LAYOUT_JOIN,
    SOLAR_OS_DISPLAY_LAYOUT_SPLIT,
} solar_os_display_layout_kind_t;

typedef struct {
    solar_os_display_layout_kind_t kind;
    solar_os_display_layout_axis_t axis;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    uint16_t width;
    uint16_t height;
    size_t backing_count;
    char backing[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX]
                [SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    size_t logical_count;
    char logical[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX]
                [SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
} solar_os_display_layout_info_t;

esp_err_t solar_os_display_layout_join(
    const char *name,
    solar_os_display_layout_axis_t axis,
    size_t target_count,
    const char *const *targets,
    char *busy_owner,
    size_t busy_owner_len);

esp_err_t solar_os_display_layout_split(
    const char *target,
    solar_os_display_layout_axis_t axis,
    const char *first,
    const char *second,
    char *busy_owner,
    size_t busy_owner_len);

esp_err_t solar_os_display_layout_unjoin(const char *name);
esp_err_t solar_os_display_layout_unsplit(const char *target);
size_t solar_os_display_layout_count(void);
bool solar_os_display_layout_get(size_t index,
                                 solar_os_display_layout_info_t *info);
const char *solar_os_display_layout_axis_name(
    solar_os_display_layout_axis_t axis);
