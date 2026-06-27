#include "solar_os.h"

#include <stddef.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_gfx.h"
#include "solar_os_splash.h"

void solar_os_context_init(solar_os_context_t *ctx,
                           solar_os_terminal_t *terminal,
                           solar_os_gfx_t *gfx)
{
    if (ctx == NULL) {
        return;
    }

    ctx->terminal = terminal;
    ctx->gfx = gfx;
    ctx->shell_io = NULL;
    ctx->shell_session = NULL;
    ctx->requested_app = NULL;
    ctx->exit_requested = false;
    ctx->sleep_requested = false;
    ctx->session_request = SOLAR_OS_SESSION_REQUEST_NONE;
    ctx->session_request_id = 0;
    ctx->session_list_fn = NULL;
    ctx->session_list_user = NULL;
    ctx->graphics_active = false;
    ctx->preserve_terminal = false;
    ctx->argc = 0;
    memset(ctx->argv, 0, sizeof(ctx->argv));
}

solar_os_terminal_t *solar_os_context_terminal(solar_os_context_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }

    return ctx->terminal;
}

solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }

    return ctx->gfx;
}

void solar_os_context_set_shell_io(solar_os_context_t *ctx, solar_os_shell_io_t *io)
{
    if (ctx == NULL) {
        return;
    }

    ctx->shell_io = io;
}

solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }

    return ctx->shell_io;
}

void solar_os_context_set_shell_session(solar_os_context_t *ctx, solar_os_shell_session_t *session)
{
    if (ctx == NULL) {
        return;
    }

    ctx->shell_session = session;
}

solar_os_shell_session_t *solar_os_context_shell_session(solar_os_context_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }

    return ctx->shell_session;
}

void solar_os_context_detach_shell_session(solar_os_context_t *ctx,
                                           solar_os_shell_session_t *session)
{
    if (ctx == NULL || session == NULL || ctx->shell_session != session) {
        return;
    }

    ctx->shell_session = NULL;
    ctx->shell_io = NULL;
}

void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active)
{
    if (ctx == NULL) {
        return;
    }

    ctx->graphics_active = active;
    if (active && ctx->gfx != NULL) {
        solar_os_gfx_clear(ctx->gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_color(ctx->gfx, SOLAR_OS_GFX_COLOR_BLACK);
    }
}

bool solar_os_context_graphics_active(const solar_os_context_t *ctx)
{
    return ctx != NULL && ctx->graphics_active;
}

void solar_os_context_request_terminal_preserve(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->preserve_terminal = true;
    }
}

bool solar_os_context_take_terminal_preserve(solar_os_context_t *ctx)
{
    if (ctx == NULL || !ctx->preserve_terminal) {
        return false;
    }

    ctx->preserve_terminal = false;
    return true;
}

esp_err_t solar_os_context_request_launch(solar_os_context_t *ctx,
                                          const solar_os_app_t *app,
                                          int argc,
                                          char **argv)
{
    if (ctx == NULL || app == NULL || argc < 0 || argc > SOLAR_OS_APP_ARG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < argc; i++) {
        if (argv == NULL || argv[i] == NULL || strlen(argv[i]) >= SOLAR_OS_APP_ARG_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    memset(ctx->argv, 0, sizeof(ctx->argv));
    for (int i = 0; i < argc; i++) {
        strlcpy(ctx->argv[i], argv[i], sizeof(ctx->argv[i]));
    }
    ctx->argc = argc;
    ctx->requested_app = app;
    ctx->exit_requested = false;
    ctx->sleep_requested = false;
    ctx->graphics_active = false;
    return ESP_OK;
}

const solar_os_app_t *solar_os_context_take_launch_request(solar_os_context_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }

    const solar_os_app_t *app = ctx->requested_app;
    ctx->requested_app = NULL;
    return app;
}

void solar_os_context_request_exit(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->exit_requested = true;
    }
}

bool solar_os_context_take_exit_request(solar_os_context_t *ctx)
{
    if (ctx == NULL || !ctx->exit_requested) {
        return false;
    }

    ctx->exit_requested = false;
    return true;
}

void solar_os_context_request_sleep(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->sleep_requested = true;
    }
}

bool solar_os_context_take_sleep_request(solar_os_context_t *ctx)
{
    if (ctx == NULL || !ctx->sleep_requested) {
        return false;
    }

    ctx->sleep_requested = false;
    return true;
}

void solar_os_context_set_session_list_handler(solar_os_context_t *ctx,
                                               solar_os_session_list_fn fn,
                                               void *user)
{
    if (ctx == NULL) {
        return;
    }

    ctx->session_list_fn = fn;
    ctx->session_list_user = user;
}

void solar_os_context_copy_session_handlers(solar_os_context_t *dst,
                                            const solar_os_context_t *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }

    dst->session_list_fn = src->session_list_fn;
    dst->session_list_user = src->session_list_user;
}

esp_err_t solar_os_context_print_session_list(solar_os_context_t *ctx)
{
    if (ctx == NULL || ctx->session_list_fn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx->session_list_fn(ctx->shell_io, ctx->session_list_user);
    return ESP_OK;
}

void solar_os_context_request_session_list(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        ctx->session_request = SOLAR_OS_SESSION_REQUEST_LIST;
        ctx->session_request_id = 0;
    }
}

void solar_os_context_request_session_fg(solar_os_context_t *ctx, uint8_t session_id)
{
    if (ctx != NULL) {
        ctx->session_request = SOLAR_OS_SESSION_REQUEST_FG;
        ctx->session_request_id = session_id;
    }
}

void solar_os_context_request_session_close(solar_os_context_t *ctx, uint8_t session_id)
{
    if (ctx != NULL) {
        ctx->session_request = SOLAR_OS_SESSION_REQUEST_CLOSE;
        ctx->session_request_id = session_id;
    }
}

bool solar_os_context_take_session_request(solar_os_context_t *ctx,
                                           solar_os_session_request_type_t *type,
                                           uint8_t *session_id)
{
    if (ctx == NULL || ctx->session_request == SOLAR_OS_SESSION_REQUEST_NONE) {
        return false;
    }

    if (type != NULL) {
        *type = ctx->session_request;
    }
    if (session_id != NULL) {
        *session_id = ctx->session_request_id;
    }
    ctx->session_request = SOLAR_OS_SESSION_REQUEST_NONE;
    ctx->session_request_id = 0;
    return true;
}

void solar_os_context_reboot(solar_os_context_t *ctx, const char *status)
{
    if (ctx != NULL && ctx->gfx != NULL) {
        solar_os_splash_draw_reboot(ctx->gfx, status);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    esp_restart();
}

int solar_os_context_argc(const solar_os_context_t *ctx)
{
    return ctx != NULL ? ctx->argc : 0;
}

const char *solar_os_context_argv(const solar_os_context_t *ctx, int index)
{
    if (ctx == NULL || index < 0 || index >= ctx->argc) {
        return NULL;
    }

    return ctx->argv[index];
}
