#pragma once

#include "solar_os_native_jobs_abi.h"
#include "solar_os_native_service_abi.h"

struct solar_os_application_event_v1;

#define SOLAR_OS_NATIVE_SESSIONS_SERVICE "sessions"
#define SOLAR_OS_NATIVE_SESSIONS_ABI 1U

#define SOLAR_OS_NATIVE_SESSION_DISPLAY_SHELL (1U << 0)
#define SOLAR_OS_NATIVE_SESSION_DISPLAY_APP (1U << 1)
#define SOLAR_OS_NATIVE_SESSION_PORT_SHELL (1U << 2)
#define SOLAR_OS_NATIVE_SESSION_PORT_APP (1U << 3)
#define SOLAR_OS_NATIVE_SESSION_ALL ((1U << 4) - 1U)

#define SOLAR_OS_NATIVE_SESSION_FOCUS_RIGHT 0U
#define SOLAR_OS_NATIVE_SESSION_FOCUS_LEFT 1U
#define SOLAR_OS_NATIVE_SESSION_FOCUS_DOWN 2U
#define SOLAR_OS_NATIVE_SESSION_FOCUS_UP 3U

typedef enum {
    SOLAR_OS_NATIVE_SESSION_READY = 0,
    SOLAR_OS_NATIVE_SESSION_ACTIVE,
    SOLAR_OS_NATIVE_SESSION_SUSPENDED,
    SOLAR_OS_NATIVE_SESSION_DETACHED,
    SOLAR_OS_NATIVE_SESSION_STARTING,
    SOLAR_OS_NATIVE_SESSION_STOPPING,
} solar_os_native_session_state_t;

typedef struct {
    uint32_t id, parent_id, kind;
    solar_os_native_session_state_t state;
    bool has_tick;
    char title[48], app[48], target[24], terminal_profile[12];
    solar_os_job_tick_stats_v1_t tick_stats;
} solar_os_native_session_info_v1_t;

typedef struct {
    uint32_t kind;
    const char *target;
    /* Port shells only. NULL selects automatic terminal and UTF-8. */
    const char *terminal_profile, *charset;
    uint16_t columns, rows;
    bool run_startup;
} solar_os_native_shell_session_request_v1_t;

typedef struct {
    uint32_t id;
    char busy_owner[32];
} solar_os_native_shell_session_result_v1_t;

typedef struct {
    const char *name;
    const char *display;
    const char *cwd;
    const char *const *arguments;
    size_t argument_count;
} solar_os_native_display_app_request_v1_t;

typedef struct {
    uint32_t id;
    bool deferred;
    char busy_owner[32];
} solar_os_native_display_app_result_v1_t;

/* Queries read display state on the session owner and copy port state under
 * the port registry lock. total receives the number of matches, including
 * entries beyond capacity. Use capacity=0 for
 * a count. Kind masks select one or more session kinds. IDs can be reused,
 * and records outlive their session. target is the display or port name.
 * Queries and changes can wait on the session scheduler. Call from its own
 * task or a task it is free to wait for. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*snapshot)(uint32_t kinds, solar_os_native_session_info_v1_t *entries,
        size_t capacity, size_t *total);
    int (*find)(uint32_t id, solar_os_native_session_info_v1_t *entry);
    int (*get_display_focus)(char *target, size_t capacity);
    int (*focus_display)(const char *target);
    /* Opens a detached display shell or a port shell. Both dimensions zero
     * select the port terminal's normal size discovery. Inputs are borrowed
     * until return. The result ID is set after successful creation. */
    int (*open_shell)(const solar_os_native_shell_session_request_v1_t *request,
        solar_os_native_shell_session_result_v1_t *result);
    int (*resize_port)(uint32_t id, uint16_t columns, uint16_t rows);
    /* Fields are SOLAR_OS_NATIVE_TERMINAL_PREFERENCE_* from the terminal ABI.
     * Named targets accept orientation and scale. The save flag persists the
     * updated setting after successfully applying it to the target. */
    int (*set_display_preference)(const char *target, uint32_t field, uint32_t value, bool save);
    /* Uses the current application's display terminal. Shells on a port can
     * update the global palette and status-bar defaults. */
    int (*set_terminal_preference)(uint32_t field, uint32_t value);
    int (*active_on_display)(const char *target, uint32_t *session);
    /* Move within a split display or cycle through available displays. A
     * boundary without a neighbor succeeds with moved=false. */
    int (*move_display_focus)(uint32_t direction, bool *moved);
    bool (*application_uses_display)(const char *target);
    /* Cycle the session ring of the display with keyboard focus. */
    int (*cycle_focus)(bool forward, bool *handled);
    /* Run a command in a display shell. Output stays in that session. */
    int (*submit_command)(uint32_t id, const char *command);
    /* Application callbacks defer closure until they return. Other callers
     * wait for the requested display or port session to close. */
    int (*request_close)(uint32_t id);
    /* Arguments exclude the app name. An explicit cwd is a resolved storage
     * path. NULL uses the current application directory or default volume.
     * Launch on the caller's display is deferred and has no immediate ID. */
    int (*open_display_app)(const solar_os_native_display_app_request_v1_t *request,
        solar_os_native_display_app_result_v1_t *result);
    /* Deliver input directly to an active display session, preserving other
     * displays' focus. delivered=false means the ID is absent or suspended.
     * Call from a job or task outside an application callback. */
    int (*dispatch_input)(uint32_t id, const struct solar_os_application_event_v1 *event, bool *delivered);
    /* Stop attached sessions in child-first order. A failure leaves the
     * remaining sessions and their display live for retry. The primary
     * shell is preserved. Call outside an application callback. */
    int (*close_display)(const char *target);
    /* Request that a port worker stop. Use snapshot to track completion. */
    int (*stop_port_shell)(uint32_t id);
    bool (*port_session_is_local)(uint32_t id);
    /* Switch the active app of its port shell. Call from outside a port
     * app callback. The last-app operation uses the invoking port shell. */
    int (*foreground_port_app)(uint32_t id);
    int (*foreground_last_port_app)(uint32_t *id);
} solar_os_native_sessions_api_v1_t;
