#pragma once

#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_DISPLAY_LAYOUT_NAME_MAX 16U
#define SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX 4U

typedef enum {
    SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL,
    SOLAR_OS_DISPLAY_LAYOUT_VERTICAL,
} solar_os_display_layout_axis_t;

typedef enum {
    SOLAR_OS_DISPLAY_LAYOUT_JOIN,
    SOLAR_OS_DISPLAY_LAYOUT_SPLIT,
} solar_os_display_layout_kind_t;

typedef struct {
    solar_os_display_layout_kind_t kind;
    solar_os_display_layout_axis_t axis;
    char name[SOLAR_OS_DISPLAY_LAYOUT_NAME_MAX];
    uint16_t width;
    uint16_t height;
    size_t backing_count;
    char backing[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX][SOLAR_OS_DISPLAY_LAYOUT_NAME_MAX];
    size_t logical_count;
    char logical[SOLAR_OS_DISPLAY_LAYOUT_MEMBER_MAX][SOLAR_OS_DISPLAY_LAYOUT_NAME_MAX];
} solar_os_display_layout_info_t;

/* JOIN uses 2–4 physical targets as members and creates a target named name.
 * SPLIT uses name as the backing target and creates two logical members. */
typedef struct {
    solar_os_display_layout_kind_t kind;
    solar_os_display_layout_axis_t axis;
    const char *name;
    size_t member_count;
    const char *const *members;
} solar_os_display_layout_request_v1_t;
