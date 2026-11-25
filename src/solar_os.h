#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_input.h"
#include "solar_os_application_types_v1.h"
#include "solar_os_native_application_events_abi.h"
#include "solar_os_native_application_lifecycle_abi.h"

#define SOLAR_OS_APP_ARG_MAX 8
#define SOLAR_OS_APP_ARG_LEN 160
#define SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX 160

typedef solar_os_application_lifecycle_api_v1_t solar_os_app_t;
typedef struct solar_os_job solar_os_job_t;
typedef struct solar_os_shell_io solar_os_shell_io_t;
typedef struct solar_os_shell_session solar_os_shell_session_t;

typedef void (*solar_os_session_list_fn)(solar_os_shell_io_t *io, void *user);
typedef esp_err_t (*solar_os_context_output_fn)(const char *text,
                                                size_t len,
                                                void *user);

/*
 * Foreground-app mutable state is cold by default: the shared app lifecycle
 * allocates it immediately before start(), retains it across suspend/resume,
 * and releases it after stop() or a failed start. File-scope mutable objects
 * are forbidden unless a narrowly reviewed SRAM exception is declared.
 */
typedef solar_os_application_state_storage_v1_t solar_os_app_state_storage_t;
#define SOLAR_OS_APP_STATIC_SRAM_EXCEPTION(reason)

typedef enum {
    SOLAR_OS_SESSION_REQUEST_NONE,
    SOLAR_OS_SESSION_REQUEST_LIST,
    SOLAR_OS_SESSION_REQUEST_FG,
    SOLAR_OS_SESSION_REQUEST_CLOSE,
} solar_os_session_request_type_t;

typedef struct solar_os_context {
    solar_os_terminal_t *terminal;
    solar_os_gfx_t *gfx;
    solar_os_shell_io_t *shell_io;
    solar_os_shell_session_t *shell_session;
    solar_os_context_output_fn output_fn;
    void *output_user;
    const solar_os_app_t *requested_app;
    solar_os_launch_policy_t launch_policy;
    bool exit_requested;
    bool exit_result_pending;
    int exit_code;
    bool sleep_requested;
    bool deep_sleep_requested;
    bool suspend_requested;
    solar_os_session_request_type_t session_request;
    uint8_t session_request_id;
    solar_os_session_list_fn session_list_fn;
    void *session_list_user;
    bool graphics_active;
    bool preserve_terminal;
    bool status_message_pending;
    char status_message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    solar_os_app_class_t app_class;
    int argc;
    char argv[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    /* Optional launch metadata. Apps copy it at start, like argc/argv. */
    char launch_cwd[SOLAR_OS_APP_ARG_LEN];
    /* Descriptor of the app whose lifecycle callback owns this context. */
    const solar_os_app_t *app;
} solar_os_context_t;

typedef solar_os_application_event_type_v1_t solar_os_event_type_t;
typedef solar_os_application_event_v1_t solar_os_event_t;

/* All foreground launchers must use these lifecycle functions instead of
 * calling descriptor callbacks directly. */
esp_err_t solar_os_app_start(const solar_os_app_t *app,
                             solar_os_context_t *ctx);
void solar_os_app_stop(const solar_os_app_t *app, solar_os_context_t *ctx);
/* stop waits for teardown. stop_checked returns false while teardown remains
 * pending; the caller must retain the context, resources and code for retry. */
bool solar_os_app_stop_checked(const solar_os_app_t *app, solar_os_context_t *ctx);
bool solar_os_app_event(const solar_os_app_t *app, solar_os_context_t *ctx,
                         const solar_os_event_t *event);
void solar_os_app_suspend(const solar_os_app_t *app, solar_os_context_t *ctx);
void solar_os_app_resume(const solar_os_app_t *app, solar_os_context_t *ctx);
void solar_os_app_title(const solar_os_app_t *app, solar_os_context_t *ctx,
                        char *buffer, size_t size);

/* Private execution scopes expose only the calling application's services.
 * A worker may enter while its lifecycle owner guarantees the context remains
 * alive. Scopes are task-local and must be left before returning or exiting. */
typedef struct solar_os_application_scope {
    solar_os_context_t *context;
    const solar_os_app_t *app;
} solar_os_application_scope_t;
void solar_os_application_scope_enter(solar_os_application_scope_t *scope,
                                      const solar_os_app_t *app, solar_os_context_t *ctx);
void solar_os_application_scope_leave(solar_os_application_scope_t *scope);
solar_os_context_t *solar_os_application_context(void);
const solar_os_app_t *solar_os_application_descriptor(void);

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
    /* Optional context-aware callbacks used by dynamically registered jobs. */
    void *callback_user;
    esp_err_t (*start_with_user)(void *user,
                                 solar_os_context_t *ctx,
                                 int argc,
                                 char **argv);
    void (*stop_with_user)(void *user, solar_os_context_t *ctx);
    bool (*event_with_user)(void *user,
                            solar_os_context_t *ctx,
                            const solar_os_event_t *event);
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
void solar_os_context_set_output_handler(solar_os_context_t *ctx,
                                         solar_os_context_output_fn fn,
                                         void *user);
solar_os_context_output_fn solar_os_context_output_handler(
    const solar_os_context_t *ctx,
    void **user);
void solar_os_context_set_app_class(solar_os_context_t *ctx,
                                    solar_os_app_class_t app_class);
solar_os_app_class_t solar_os_context_app_class(
    const solar_os_context_t *ctx);
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active);
void solar_os_context_set_streaming_graphics_active(solar_os_context_t *ctx,
                                                    bool active);
bool solar_os_context_graphics_active(const solar_os_context_t *ctx);
void solar_os_context_request_terminal_preserve(solar_os_context_t *ctx);
bool solar_os_context_take_terminal_preserve(solar_os_context_t *ctx);
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
/* The single foreground-app completion path. The first outcome wins. */
void solar_os_context_finish(solar_os_context_t *ctx,
                             int exit_code,
                             const char *message);
bool solar_os_context_take_exit_request(solar_os_context_t *ctx);
bool solar_os_context_take_exit_result(solar_os_context_t *ctx,
                                       int *exit_code);
void solar_os_context_request_sleep(solar_os_context_t *ctx);
bool solar_os_context_take_sleep_request(solar_os_context_t *ctx);
void solar_os_context_request_deep_sleep(solar_os_context_t *ctx);
bool solar_os_context_take_deep_sleep_request(solar_os_context_t *ctx);
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
