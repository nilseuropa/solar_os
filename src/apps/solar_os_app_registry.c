#include "solar_os_app_registry.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_AUDIO
#include "solar_os_audio_apps.h"
#endif
#if SOLAR_OS_PACKAGE_NET
#include "solar_os_chat_app.h"
#include "solar_os_curl.h"
#include "solar_os_scp_app.h"
#include "solar_os_ssh_app.h"
#include "solar_os_web.h"
#endif
#if SOLAR_OS_PACKAGE_UTILS
#include "solar_os_clock.h"
#include "solar_os_com.h"
#include "solar_os_edit.h"
#include "solar_os_less.h"
#include "solar_os_plot.h"
#include "solar_os_sheet.h"
#endif
#if SOLAR_OS_PACKAGE_GAMES
#include "solar_os_invaders.h"
#endif
#if SOLAR_OS_PACKAGE_PYTHON
#include "solar_os_python.h"
#endif
#if SOLAR_OS_PACKAGE_LUA
#include "solar_os_lua.h"
#endif
#if SOLAR_OS_PACKAGE_MEDIA
#include "solar_os_view.h"
#endif

static const solar_os_app_registry_entry_t registered_apps[] = {
#if SOLAR_OS_PACKAGE_AUDIO
    {"aplay", "play WAV/MP3 audio", &solar_os_aplay_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY},
    {"arecord", "record WAV audio", &solar_os_arecord_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY},
#endif
#if SOLAR_OS_PACKAGE_NET
    {"chat", "gateway chat client", &solar_os_chat_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY},
    {"curl", "HTTP client", &solar_os_curl_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
    {"scp", "SCP file copy", &solar_os_scp_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
    {"ssh", "SSH client", &solar_os_ssh_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
    {"web", "simple web browser", &solar_os_web_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY},
#endif
#if SOLAR_OS_PACKAGE_UTILS
    {"clock", "clock, countdown alarm, stopwatch", &solar_os_clock_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY},
    {"com", "serial terminal", &solar_os_com_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY},
    {"edit", "text editor", &solar_os_edit_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
    {"less", "text file pager", &solar_os_less_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
    {"plot", "plot DAQ CSV files or scalar streams", &solar_os_plot_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY},
    {"reader", "resumable text reader", &solar_os_reader_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
    {"sheet", "CSV sheet viewer", &solar_os_sheet_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
#endif
#if SOLAR_OS_PACKAGE_GAMES
    {"invaders", "arcade shooter", &solar_os_invaders_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY},
#endif
#if SOLAR_OS_PACKAGE_PYTHON
    {"python", "MicroPython runtime", &solar_os_python_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
#endif
#if SOLAR_OS_PACKAGE_LUA
    {"lua", "Lua runtime", &solar_os_lua_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT},
#endif
#if SOLAR_OS_PACKAGE_MEDIA
    {"view", "image viewer", &solar_os_view_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY},
#endif
    {NULL, NULL, NULL, 0},
};

#define REGISTERED_APP_STORAGE_COUNT (sizeof(registered_apps) / sizeof(registered_apps[0]))

static const size_t registered_app_count = REGISTERED_APP_STORAGE_COUNT - 1U;
static char app_owners[sizeof(registered_apps) / sizeof(registered_apps[0])][SOLAR_OS_APP_OWNER_MAX];
static portMUX_TYPE app_owner_lock = portMUX_INITIALIZER_UNLOCKED;

static int app_registry_index_by_app(const solar_os_app_t *app)
{
    if (app == NULL) {
        return -1;
    }

    for (size_t i = 0; i < registered_app_count; i++) {
        if (registered_apps[i].app == app) {
            return (int)i;
        }
    }

    return -1;
}

size_t solar_os_app_registry_count(void)
{
    return registered_app_count;
}

const solar_os_app_registry_entry_t *solar_os_app_registry_get(size_t index)
{
    if (index >= registered_app_count) {
        return NULL;
    }

    return &registered_apps[index];
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < registered_app_count; i++) {
        if (registered_apps[i].name != NULL && strcmp(registered_apps[i].name, name) == 0) {
            return &registered_apps[i];
        }
    }

    return NULL;
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find_by_app(const solar_os_app_t *app)
{
    const int index = app_registry_index_by_app(app);
    return index >= 0 ? &registered_apps[index] : NULL;
}

bool solar_os_app_registry_owner(const solar_os_app_t *app, char *owner, size_t owner_len)
{
    const int index = app_registry_index_by_app(app);
    char current[SOLAR_OS_APP_OWNER_MAX] = "";

    if (owner != NULL && owner_len > 0) {
        owner[0] = '\0';
    }
    if (index < 0) {
        return false;
    }

    portENTER_CRITICAL(&app_owner_lock);
    const bool claimed = app_owners[index][0] != '\0';
    if (claimed) {
        strlcpy(current, app_owners[index], sizeof(current));
    }
    portEXIT_CRITICAL(&app_owner_lock);

    if (claimed && owner != NULL && owner_len > 0) {
        strlcpy(owner, current, owner_len);
    }
    return claimed;
}

esp_err_t solar_os_app_registry_claim(const solar_os_app_t *app,
                                      const char *owner,
                                      char *current_owner,
                                      size_t current_owner_len)
{
    const int index = app_registry_index_by_app(app);
    char busy_owner[SOLAR_OS_APP_OWNER_MAX] = "";

    if (current_owner != NULL && current_owner_len > 0) {
        current_owner[0] = '\0';
    }
    if (index < 0) {
        return ESP_OK;
    }
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&app_owner_lock);
    if (app_owners[index][0] == '\0' || strcmp(app_owners[index], owner) == 0) {
        strlcpy(app_owners[index], owner, sizeof(app_owners[index]));
        portEXIT_CRITICAL(&app_owner_lock);
        return ESP_OK;
    }
    strlcpy(busy_owner, app_owners[index], sizeof(busy_owner));
    portEXIT_CRITICAL(&app_owner_lock);

    if (current_owner != NULL && current_owner_len > 0) {
        strlcpy(current_owner, busy_owner, current_owner_len);
    }
    return ESP_ERR_INVALID_STATE;
}

void solar_os_app_registry_release(const solar_os_app_t *app, const char *owner)
{
    const int index = app_registry_index_by_app(app);

    if (index < 0 || owner == NULL || owner[0] == '\0') {
        return;
    }

    portENTER_CRITICAL(&app_owner_lock);
    if (strcmp(app_owners[index], owner) == 0) {
        app_owners[index][0] = '\0';
    }
    portEXIT_CRITICAL(&app_owner_lock);
}
