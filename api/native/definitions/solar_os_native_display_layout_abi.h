#pragma once

#include "solar_os_native_service_abi.h"
#include "solar_os_display_layout_types_v1.h"

#define SOLAR_OS_NATIVE_DISPLAY_LAYOUT_SERVICE "display.layout"
#define SOLAR_OS_NATIVE_DISPLAY_LAYOUT_ABI 1U

/* Layout records own their values. create and remove act synchronously on
 * the session owner. The request and its strings are borrowed for the call.
 * The built-in shell follows a changed layout when its target is affected.
 * A target still in use can prevent removal. Runtime layouts are temporary. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_display_layout_info_t *info);
    int (*find)(solar_os_display_layout_kind_t kind, const char *name,
                solar_os_display_layout_info_t *info);
    int (*create)(const solar_os_display_layout_request_v1_t *request,
                  char *busy_owner, size_t capacity);
    int (*remove)(solar_os_display_layout_kind_t kind, const char *name);
    const char *(*axis_name)(solar_os_display_layout_axis_t axis);
} solar_os_native_display_layout_api_v1_t;
