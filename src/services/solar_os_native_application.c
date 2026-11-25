#include "solar_os_native_application.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_application_abi.h"
#include "solar_os_native_support.h"
#include "solar_os.h"
#include "solar_os_app_registry.h"
#include "solar_os_shell_io.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
static bool application_active(void) { return solar_os_application_descriptor() != NULL; }
static solar_os_context_t *application_context(void)
{ return application_active() ? solar_os_application_context() : NULL; }
static const char *application_name(void)
{ const solar_os_app_t *app = solar_os_application_descriptor(); return app ? app->name : NULL; }
static const char *application_summary(void)
{ const solar_os_app_t *app = solar_os_application_descriptor(); return app ? app->summary : NULL; }
static uint32_t application_class(void)
{ return solar_os_context_app_class(application_context()); }
static uint32_t application_flags(void)
{ const solar_os_app_t *app = solar_os_application_descriptor(); return app ? app->flags : 0; }
static int32_t application_argc(void) { return solar_os_context_argc(application_context()); }
static const char *application_argv(uint32_t index)
{ return index < SOLAR_OS_APP_ARG_MAX ? solar_os_context_argv(application_context(), index) : NULL; }
static const char *application_cwd(void)
{ solar_os_context_t *ctx = application_context(); return ctx ? ctx->launch_cwd : NULL; }
static void *application_state(void)
{ const solar_os_app_t *app = solar_os_application_descriptor(); return app && app->state_slot ? *app->state_slot : NULL; }
static solar_os_terminal_t *application_terminal(void)
{ return solar_os_context_terminal(application_context()); }
static solar_os_gfx_t *application_gfx(void)
{ return solar_os_context_gfx(application_context()); }
static bool application_graphics_active(void)
{ return solar_os_context_graphics_active(application_context()); }
static int application_set_class(uint32_t app_class)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (app_class < SOLAR_OS_APP_CLASS_COMMAND || app_class > SOLAR_OS_APP_CLASS_GUI)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (app_class == SOLAR_OS_APP_CLASS_GUI && !ctx->gfx) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    solar_os_context_set_app_class(ctx, (solar_os_app_class_t)app_class);
    return SOLAR_OS_NATIVE_OK;
}
static int application_set_graphics(bool active)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (active && !ctx->gfx) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    solar_os_context_set_graphics_active(ctx, active);
    return SOLAR_OS_NATIVE_OK;
}
static int application_set_streaming(bool active)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (active && !ctx->gfx) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    solar_os_context_set_streaming_graphics_active(ctx, active);
    return SOLAR_OS_NATIVE_OK;
}
static int application_finish(int32_t exit_code, const char *message)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_context_finish(ctx, exit_code, message);
    return SOLAR_OS_NATIVE_OK;
}
#define APPLICATION_REQUEST(name, function) \
    static int name(void) { \
        solar_os_context_t *ctx = application_context(); \
        if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE; \
        function(ctx); return SOLAR_OS_NATIVE_OK; \
    }
APPLICATION_REQUEST(application_preserve, solar_os_context_request_terminal_preserve)
APPLICATION_REQUEST(application_sleep, solar_os_context_request_sleep)
APPLICATION_REQUEST(application_deep_sleep, solar_os_context_request_deep_sleep)
APPLICATION_REQUEST(application_suspend, solar_os_context_request_suspend)
APPLICATION_REQUEST(application_session_list, solar_os_context_request_session_list)
#undef APPLICATION_REQUEST
static int application_launch(const char *name, uint32_t count, const char *const args[], uint32_t policy)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (policy > SOLAR_OS_LAUNCH_CHILD_RETURN || count >= SOLAR_OS_APP_ARG_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int result = solar_os_native_result(solar_os_app_registry_request_launch(ctx, name, count, args));
    if (!result) ctx->launch_policy = (solar_os_launch_policy_t)policy;
    return result;
}
static int application_open(const char *path)
{
    solar_os_context_t *ctx = application_context();
    return ctx ? solar_os_native_result(solar_os_app_registry_request_open(ctx, path)) : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
}
static int application_print_sessions(void)
{
    solar_os_context_t *ctx = application_context();
    return ctx ? solar_os_native_result(solar_os_context_print_session_list(ctx)) : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
}
static int application_session_fg(uint32_t id)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!id || id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_context_request_session_fg(ctx, id);
    return SOLAR_OS_NATIVE_OK;
}
static int application_session_close(uint32_t id)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!id || id > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_context_request_session_close(ctx, id);
    return SOLAR_OS_NATIVE_OK;
}
static int application_reboot(const char *status)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_context_reboot(ctx, status);
    return SOLAR_OS_NATIVE_OK;
}
static int application_write(const char *text, size_t length)
{
    solar_os_context_t *ctx = application_context();
    if (!ctx) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!text) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!length) return SOLAR_OS_NATIVE_OK;
    if (ctx->shell_io) return solar_os_native_result(solar_os_shell_io_write_len(ctx->shell_io, text, length));
    if (ctx->output_fn) return solar_os_native_result(ctx->output_fn(text, length, ctx->output_user));
    if (!ctx->terminal) return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    solar_os_shell_io_t io;
    solar_os_shell_io_init_terminal(&io, ctx->terminal);
    return solar_os_native_result(solar_os_shell_io_write_len(&io, text, length));
}
static uint32_t application_discovery_count(bool include_playground)
{ return solar_os_app_discovery_count(include_playground); }
static int application_discovery_info(uint32_t index, bool include_playground, solar_os_app_discovery_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    return solar_os_app_discovery_get(index, include_playground, info) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int application_discovery_find(const char *name, bool include_playground, solar_os_app_discovery_info_t *info)
{
    if (!info) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    if (!name || !*name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0; i < solar_os_app_discovery_count(include_playground); ++i) {
        if (solar_os_app_discovery_get(i, include_playground, info) && !strcmp(name, info->name)) return SOLAR_OS_NATIVE_OK;
    }
    memset(info, 0, sizeof(*info));
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static const solar_os_native_application_api_v1_t application_api = {
    .abi_version = SOLAR_OS_NATIVE_APPLICATION_ABI, .struct_size = sizeof(application_api),
    .active = application_active, .name = application_name, .summary = application_summary,
    .app_class = application_class, .flags = application_flags,
    .argc = application_argc, .argv = application_argv, .cwd = application_cwd, .state = application_state,
    .terminal = application_terminal, .gfx = application_gfx, .graphics_active = application_graphics_active,
    .set_app_class = application_set_class, .set_graphics_active = application_set_graphics,
    .set_streaming_graphics_active = application_set_streaming, .preserve_terminal = application_preserve,
    .finish = application_finish, .sleep = application_sleep, .deep_sleep = application_deep_sleep,
    .suspend = application_suspend, .launch = application_launch, .open = application_open,
    .can_open = solar_os_app_registry_can_open, .print_sessions = application_print_sessions,
    .request_session_list = application_session_list, .request_session_fg = application_session_fg,
    .request_session_close = application_session_close, .reboot = application_reboot, .write_utf8 = application_write,
    .discovery_count = application_discovery_count, .discovery_info = application_discovery_info,
    .discovery_find = application_discovery_find,
};
#endif

const void *solar_os_native_application_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_application_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
