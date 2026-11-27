#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_application_types_v1.h"

#define SOLAR_OS_NATIVE_APPLICATION_SERVICE "application"
#define SOLAR_OS_NATIVE_APPLICATION_ABI 1U

/* The table is globally discoverable. Context operations act only on the
 * calling task's current application scope. Mutating context operations return
 * INVALID_STATE without a context; getters return NULL, zero or false. Nested
 * callbacks restore their caller. Discovery queries need no scope.
 * Returned strings, state and rendering references are borrowed: do not retain
 * them beyond the owning lifecycle/callback or use them on another task.
 * The host owns context, cold-state allocation and rendering object lifetime. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*active)(void);
    const char *(*name)(void);
    const char *(*summary)(void);
    uint32_t (*app_class)(void);
    uint32_t (*flags)(void);
    int32_t (*argc)(void);
    const char *(*argv)(uint32_t index);
    const char *(*cwd)(void);
    void *(*state)(void);
    solar_os_terminal_t *(*terminal)(void);
    solar_os_gfx_t *(*gfx)(void);
    bool (*graphics_active)(void);
    int (*set_app_class)(uint32_t app_class);
    int (*set_graphics_active)(bool active);
    int (*set_streaming_graphics_active)(bool active);
    int (*preserve_terminal)(void);
    /* The first completion result wins, as for built-in applications. */
    int (*finish)(int32_t exit_code, const char *message);
    int (*sleep)(void);
    int (*deep_sleep)(void);
    int (*suspend)(void);
    /* Arguments exclude the application name and are copied before return.
     * launch accepts REPLACE or CHILD_RETURN; open uses CHILD_RETURN. */
    int (*launch)(const char *name, uint32_t argument_count,
                   const char *const arguments[], uint32_t policy);
    int (*open)(const char *path_or_url);
    bool (*can_open)(const char *path_or_url);
    int (*print_sessions)(void);
    int (*request_session_list)(void);
    int (*request_session_fg)(uint32_t session_id);
    int (*request_session_close)(uint32_t session_id);
    int (*reboot)(const char *status);
    int (*write_utf8)(const char *text, size_t length);
    uint32_t (*discovery_count)(bool include_playground);
    int (*discovery_info)(uint32_t index, bool include_playground,
                           solar_os_app_discovery_info_t *info);
    int (*discovery_find)(const char *name, bool include_playground,
                           solar_os_app_discovery_info_t *info);
    /* Resolve the default application for a file or supported URL. */
    int (*find_opener)(const char *path_or_url, solar_os_app_discovery_info_t *info);
    /* An empty owner means the named application is idle. */
    int (*get_owner)(const char *name, char *owner, size_t capacity);
    /* Visit installed foreground and command modules. A copied ID is borrowed
     * for the duration of each callback. Return false to finish early. */
    int (*module_visit)(bool (*visit)(const char *id, void *user), void *user);
    bool (*file_type_matches)(const char *extensions, const char *path);
} solar_os_native_application_api_v1_t;
