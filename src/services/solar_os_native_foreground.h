#pragma once
#include "solar_os.h"
#include "solar_os_app_registry.h"

esp_err_t solar_os_native_foreground_resolve(const char *path,
    const solar_os_app_t **app, char *detail, size_t detail_size);
esp_err_t solar_os_native_foreground_launch(solar_os_context_t *ctx,
    const solar_os_app_t *app, int argc, char **argv);
const solar_os_app_registry_entry_t *solar_os_native_foreground_find(const char *id);
bool solar_os_native_foreground_active(const char *id);
solar_os_gfx_t *solar_os_native_foreground_console_gfx(void);

const solar_os_app_registry_entry_t *solar_os_native_foreground_entry(const solar_os_app_t *app);
bool solar_os_native_foreground_owner(const solar_os_app_t *app, char *owner, size_t size);
esp_err_t solar_os_native_foreground_claim(const solar_os_app_t *app,
    const char *owner, char *busy, size_t size);
void solar_os_native_foreground_release(const solar_os_app_t *app, const char *owner);
