#pragma once
#include "solar_os_gfx.h"
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 32
#define SOLAR_OS_DISPLAY_TARGET_OWNER_MAX 32
typedef struct {
    uint32_t frame_formats;
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
} solar_os_display_target_t;
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target);
esp_err_t solar_os_display_claim(const char *name,const char *owner,char *detail,size_t size);
esp_err_t solar_os_display_release(const char *name,const char *owner);
esp_err_t solar_os_display_set_terminal_blocked(const char *name,const char *owner,bool blocked);
esp_err_t solar_os_display_open_gfx(const char *name,const char *owner,solar_os_gfx_t **gfx,char *detail,size_t size);
