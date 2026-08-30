#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_input.h"

#define SOLAR_OS_APP_ARG_MAX 8
#define SOLAR_OS_APP_ARG_LEN 160
#define SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX 160

typedef struct solar_os_terminal solar_os_terminal_t;
typedef struct solar_os_gfx solar_os_gfx_t;
typedef struct solar_os_app solar_os_app_t;
typedef struct solar_os_job solar_os_job_t;
typedef struct solar_os_shell_io solar_os_shell_io_t;
typedef struct solar_os_shell_session solar_os_shell_session_t;

typedef void (*solar_os_session_list_fn)(solar_os_shell_io_t *io, void *user);

#define SOLAR_OS_APP_FLAG_RESUMABLE (1U << 0)
/* Reuse a launching display shell's terminal instead of allocating a new one. */
#define SOLAR_OS_APP_FLAG_SHELL_INLINE (1U << 1)
/* Receive structured local key events instead of their legacy character form. */
#define SOLAR_OS_APP_FLAG_KEY_EVENTS (1U << 2)
/* Receive generic absolute or relative pointer events. */
#define SOLAR_OS_APP_FLAG_POINTER_EVENTS (1U << 3)
/* Receive normalized analog axis events. */
#define SOLAR_OS_APP_FLAG_AXIS_EVENTS (1U << 4)

/*
 * Foreground-app mutable state is cold by default: the shared app lifecycle
 * allocates it immediately before start(), retains it across suspend/resume,
 * and releases it after stop() or a failed start. File-scope mutable objects
 * are forbidden unless a narrowly reviewed SRAM exception is declared.
 */
typedef enum {
    SOLAR_OS_APP_STATE_NONE = 0,
    SOLAR_OS_APP_STATE_TRANSIENT,
    SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    SOLAR_OS_APP_STATE_EXTERNAL_REQUIRED,
    SOLAR_OS_APP_STATE_INTERNAL_REQUIRED,
} solar_os_app_state_storage_t;

#define SOLAR_OS_APP_STATIC_SRAM_EXCEPTION(reason)

typedef enum {
    SOLAR_OS_LAUNCH_REPLACE,
    SOLAR_OS_LAUNCH_CHILD_RETURN,
} solar_os_launch_policy_t;

typedef enum {
    SOLAR_OS_SESSION_REQUEST_NONE,
    SOLAR_OS_SESSION_REQUEST_LIST,
    SOLAR_OS_SESSION_REQUEST_FG,
    SOLAR_OS_SESSION_REQUEST_CLOSE,
} solar_os_session_request_type_t;

typedef struct {
    solar_os_terminal_t *terminal;
    solar_os_gfx_t *gfx;
    solar_os_shell_io_t *shell_io;
    solar_os_shell_session_t *shell_session;
    const solar_os_app_t *requested_app;
    solar_os_launch_policy_t launch_policy;
    bool exit_requested;
    bool sleep_requested;
    bool suspend_requested;
    solar_os_session_request_type_t session_request;
    uint8_t session_request_id;
    solar_os_session_list_fn session_list_fn;
    void *session_list_user;
    bool graphics_active;
    bool preserve_terminal;
    bool status_message_pending;
    char status_message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    int argc;
    char argv[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
} solar_os_context_t;

typedef enum {
    SOLAR_OS_EVENT_CHAR,
    SOLAR_OS_EVENT_KEY,
    SOLAR_OS_EVENT_POINTER,
    SOLAR_OS_EVENT_AXIS,
    SOLAR_OS_EVENT_TICK,
    SOLAR_OS_EVENT_RESUME,
} solar_os_event_type_t;

typedef struct {
    solar_os_event_type_t type;
    union {
        char ch;
        solar_os_input_key_event_t key;
        solar_os_input_pointer_event_t pointer;
        solar_os_input_axis_event_t axis;
        uint32_t tick_ms;
    } data;
} solar_os_event_t;

struct solar_os_app {
    const char *name;
    const char *summary;
    uint32_t flags;
    esp_err_t (*start)(solar_os_context_t *ctx);
    void (*suspend)(solar_os_context_t *ctx);
    void (*resume)(solar_os_context_t *ctx);
    void (*stop)(solar_os_context_t *ctx);
    bool (*event)(solar_os_context_t *ctx, const solar_os_event_t *event);
    void (*title)(solar_os_context_t *ctx, char *buffer, size_t buffer_len);
    /* Optional lifecycle-managed cold state. state_slot must point to a NULL
     * file-scope pointer before the first launch. */
    void **state_slot;
    size_t state_size;
    solar_os_app_state_storage_t state_storage;
    /* Optional guard for asynchronous stop paths. Return true only after no
     * worker can access the cold state. */
    bool (*state_release_ready)(void);
    /* Optional idempotent cleanup for resources referenced by cold state.
     * Called after state_release_ready and before the state block is freed. */
    void (*state_release_cleanup)(void);
    /* Declarative launch admission for an app-owned worker. */
    uint32_t worker_stack_bytes;
    bool worker_stack_external;
    /* Zero selects the 25 ms default; smaller intervals raise runtime cadence. */
    /* Deadlines are execution-time budgets, not preemptive limits. */
    uint32_t tick_interval_ms;
    uint32_t tick_deadline_ms;
    /* Optional runtime override; zero keeps tick_interval_ms/default policy. */
    uint32_t (*requested_tick_interval_ms)(void);
};

/* All foreground launchers must use these lifecycle functions instead of
 * calling descriptor callbacks directly. */
esp_err_t solar_os_app_start(const solar_os_app_t *app,
                             solar_os_context_t *ctx);
void solar_os_app_stop(const solar_os_app_t *app, solar_os_context_t *ctx);

typedef enum {
    SOLAR_OS_JOB_KIND_BACKGROUND,
    SOLAR_OS_JOB_KIND_INTERACTIVE,
} solar_os_job_kind_t;

struct solar_os_job {
    const char *name;
    const char *summary;
    solar_os_job_kind_t kind;
    esp_err_t (*start)(solar_os_context_t *ctx, int argc, char **argv);
    void (*stop)(solar_os_context_t *ctx);
    bool (*event)(solar_os_context_t *ctx, const solar_os_event_t *event);
    /* Declarative start admission for a job-owned worker. */
    uint32_t worker_stack_bytes;
    bool worker_stack_external;
    /* Zero selects the 25 ms default; smaller intervals raise runtime cadence. */
    /* Deadlines are execution-time budgets, not preemptive limits. */
    uint32_t tick_interval_ms;
    uint32_t tick_deadline_ms;
    /* Optional job-specific lines appended by `job status <name>`. */
    void (*detail)(solar_os_context_t *ctx);
    /* Optional detail for the most recent start failure. */
    void (*error_detail)(char *buffer, size_t buffer_len);
};

void solar_os_context_init(solar_os_context_t *ctx,
                           solar_os_terminal_t *terminal,
                           solar_os_gfx_t *gfx);
solar_os_terminal_t *solar_os_context_terminal(solar_os_context_t *ctx);
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx);
void solar_os_context_set_gfx(solar_os_context_t *ctx, solar_os_gfx_t *gfx);
void solar_os_context_set_shell_io(solar_os_context_t *ctx, solar_os_shell_io_t *io);
solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx);
void solar_os_context_set_shell_session(solar_os_context_t *ctx, solar_os_shell_session_t *session);
solar_os_shell_session_t *solar_os_context_shell_session(solar_os_context_t *ctx);
void solar_os_context_detach_shell_session(solar_os_context_t *ctx,
                                           solar_os_shell_session_t *session);
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active);
void solar_os_context_set_streaming_graphics_active(solar_os_context_t *ctx,
                                                    bool active);
bool solar_os_context_graphics_active(const solar_os_context_t *ctx);
void solar_os_context_request_terminal_preserve(solar_os_context_t *ctx);
bool solar_os_context_take_terminal_preserve(solar_os_context_t *ctx);
void solar_os_context_set_status_message(solar_os_context_t *ctx, const char *message);
bool solar_os_context_take_status_message(solar_os_context_t *ctx,
                                          char *buffer,
                                          size_t buffer_len);
esp_err_t solar_os_context_request_launch(solar_os_context_t *ctx,
                                          const solar_os_app_t *app,
                                          int argc,
                                          char **argv);
esp_err_t solar_os_context_request_launch_ex(solar_os_context_t *ctx,
                                             const solar_os_app_t *app,
                                             int argc,
                                             char **argv,
                                             solar_os_launch_policy_t policy);
const solar_os_app_t *solar_os_context_take_launch_request(solar_os_context_t *ctx);
solar_os_launch_policy_t solar_os_context_take_launch_policy(solar_os_context_t *ctx);
void solar_os_context_request_exit(solar_os_context_t *ctx);
bool solar_os_context_take_exit_request(solar_os_context_t *ctx);
void solar_os_context_request_sleep(solar_os_context_t *ctx);
bool solar_os_context_take_sleep_request(solar_os_context_t *ctx);
void solar_os_context_request_suspend(solar_os_context_t *ctx);
bool solar_os_context_take_suspend_request(solar_os_context_t *ctx);
void solar_os_context_set_session_list_handler(solar_os_context_t *ctx,
                                               solar_os_session_list_fn fn,
                                               void *user);
void solar_os_context_copy_session_handlers(solar_os_context_t *dst,
                                            const solar_os_context_t *src);
esp_err_t solar_os_context_print_session_list(solar_os_context_t *ctx);
void solar_os_context_request_session_list(solar_os_context_t *ctx);
void solar_os_context_request_session_fg(solar_os_context_t *ctx, uint8_t session_id);
void solar_os_context_request_session_close(solar_os_context_t *ctx, uint8_t session_id);
bool solar_os_context_take_session_request(solar_os_context_t *ctx,
                                           solar_os_session_request_type_t *type,
                                           uint8_t *session_id);
void solar_os_context_reboot(solar_os_context_t *ctx, const char *status);
int solar_os_context_argc(const solar_os_context_t *ctx);
const char *solar_os_context_argv(const solar_os_context_t *ctx, int index);
uint32_t solar_os_app_tick_interval_ms(const solar_os_app_t *app,
                                       uint32_t default_interval_ms);
