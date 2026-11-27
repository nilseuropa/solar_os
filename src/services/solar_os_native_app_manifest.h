#pragma once
#include <string.h>
#include "solar_os_native_app_abi.h"

static inline bool solar_os_native_app_manifest_valid(const solar_os_native_app_manifest_v2_t *app)
{
    if (!app || app->magic != SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC ||
        app->lifecycle_abi != SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI ||
        app->struct_size != sizeof(*app) ||
        (app->app_class != SOLAR_OS_NATIVE_APP_CLASS_TUI &&
         app->app_class != SOLAR_OS_NATIVE_APP_CLASS_GUI) ||
        (app->flags & ~(SOLAR_OS_NATIVE_APP_RESUMABLE | SOLAR_OS_NATIVE_APP_POINTER_EVENTS |
                       SOLAR_OS_NATIVE_APP_AXIS_EVENTS | SOLAR_OS_NATIVE_APP_CONSOLE)) != 0U ||
        ((app->flags & SOLAR_OS_NATIVE_APP_CONSOLE) && app->app_class != SOLAR_OS_NATIVE_APP_CLASS_TUI) ||
        app->worker_stack_bytes < 2048U || app->worker_stack_bytes > 65536U ||
        app->worker_priority < 1U || app->worker_priority > 4U ||
        app->minimum_internal_bytes > 512U * 1024U ||
        app->minimum_psram_bytes > 16U * 1024U * 1024U ||
        app->tick_interval_ms == 0U || app->tick_interval_ms > 1000U ||
        app->id[0] == '\0' || memchr(app->id, '\0', sizeof(app->id)) == NULL ||
        app->summary[0] == '\0' ||
        memchr(app->summary, '\0', sizeof(app->summary)) == NULL ||
        memchr(app->file_extensions, '\0', sizeof(app->file_extensions)) == NULL) {
        return false;
    }
    for (const char *p = app->id; *p != '\0'; ++p) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') &&
            *p != '-' && *p != '_') return false;
    }
    const char *extension = app->file_extensions;
    while (*extension) {
        if (*extension++ != '.') return false;
        size_t length = 0U;
        while (*extension && *extension != ' ') {
            if (!(*extension >= 'a' && *extension <= 'z') &&
                !(*extension >= '0' && *extension <= '9')) return false;
            extension++;
            length++;
        }
        if (!length) return false;
        if (*extension == ' ') {
            extension++;
            if (!*extension) return false;
        }
    }
    return true;
}
