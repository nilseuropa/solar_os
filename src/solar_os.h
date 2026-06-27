#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_APP_ARG_MAX 8
#define SOLAR_OS_APP_ARG_LEN 160

typedef struct solar_os_terminal solar_os_terminal_t;
typedef struct solar_os_gfx solar_os_gfx_t;
typedef struct solar_os_app solar_os_app_t;
typedef struct solar_os_job solar_os_job_t;
typedef struct solar_os_shell_io solar_os_shell_io_t;
typedef struct solar_os_shell_session solar_os_shell_session_t;

typedef void (*solar_os_session_list_fn)(solar_os_shell_io_t *io, void *user);

#define SOLAR_OS_APP_FLAG_RESUMABLE (1U << 0)

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
    bool exit_requested;
    bool sleep_requested;
    solar_os_session_request_type_t session_request;
    uint8_t session_request_id;
    solar_os_session_list_fn session_list_fn;
    void *session_list_user;
    bool graphics_active;
    bool preserve_terminal;
    int argc;
    char argv[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
} solar_os_context_t;

typedef enum {
    SOLAR_OS_EVENT_CHAR,
    SOLAR_OS_EVENT_TICK,
    SOLAR_OS_EVENT_RESUME,
} solar_os_event_type_t;

typedef struct {
    solar_os_event_type_t type;
    union {
        char ch;
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
};

struct solar_os_job {
    const char *name;
    const char *summary;
    esp_err_t (*start)(solar_os_context_t *ctx, int argc, char **argv);
    void (*stop)(solar_os_context_t *ctx);
    bool (*event)(solar_os_context_t *ctx, const solar_os_event_t *event);
};

void solar_os_context_init(solar_os_context_t *ctx,
                           solar_os_terminal_t *terminal,
                           solar_os_gfx_t *gfx);
solar_os_terminal_t *solar_os_context_terminal(solar_os_context_t *ctx);
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx);
void solar_os_context_set_shell_io(solar_os_context_t *ctx, solar_os_shell_io_t *io);
solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx);
void solar_os_context_set_shell_session(solar_os_context_t *ctx, solar_os_shell_session_t *session);
solar_os_shell_session_t *solar_os_context_shell_session(solar_os_context_t *ctx);
void solar_os_context_detach_shell_session(solar_os_context_t *ctx,
                                           solar_os_shell_session_t *session);
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active);
bool solar_os_context_graphics_active(const solar_os_context_t *ctx);
void solar_os_context_request_terminal_preserve(solar_os_context_t *ctx);
bool solar_os_context_take_terminal_preserve(solar_os_context_t *ctx);
esp_err_t solar_os_context_request_launch(solar_os_context_t *ctx,
                                          const solar_os_app_t *app,
                                          int argc,
                                          char **argv);
const solar_os_app_t *solar_os_context_take_launch_request(solar_os_context_t *ctx);
void solar_os_context_request_exit(solar_os_context_t *ctx);
bool solar_os_context_take_exit_request(solar_os_context_t *ctx);
void solar_os_context_request_sleep(solar_os_context_t *ctx);
bool solar_os_context_take_sleep_request(solar_os_context_t *ctx);
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
