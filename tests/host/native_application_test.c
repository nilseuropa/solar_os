#include "solar_os_native_application_abi.h"
#include "solar_os_native_application.h"
#include "solar_os_app_registry.h"
#include "solar_os_gfx_internal.h"
#include "freertos/FreeRTOS.h"
#define main application_scope_checks
#include "application_scopes_test.c"
#undef main

static unsigned write_bytes, restarts, output_bytes;
static const solar_os_native_application_api_v1_t *api;
void solar_os_gfx_prepare_surface(solar_os_gfx_t *gfx) { assert(gfx); }
void solar_os_gfx_release_surface(solar_os_gfx_t *gfx) { assert(gfx); }
void solar_os_gfx_clear(solar_os_gfx_t *gfx, solar_os_gfx_color_t color) { assert(gfx); (void)color; }
void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color) { assert(gfx); (void)color; }
void solar_os_splash_draw_reboot(solar_os_gfx_t *gfx, const char *status) { assert(gfx && status); }
void esp_restart(void) { ++restarts; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void solar_os_shell_io_init_terminal(solar_os_shell_io_t *io, solar_os_terminal_t *terminal)
{ memset(io, 0, sizeof(*io)); io->terminal = terminal; }
esp_err_t solar_os_shell_io_write_len(solar_os_shell_io_t *io, const char *text, size_t length)
{ assert(io && text && length == 4 && !memcmp(text, "core", length)); write_bytes += length; return ESP_OK; }
esp_err_t solar_os_shell_resolve_path(solar_os_context_t *ctx, const char *path, char *out, size_t capacity)
{ assert(ctx && !path); snprintf(out, capacity, "/launch"); return ESP_OK; }
esp_err_t solar_os_app_registry_request_launch(solar_os_context_t *ctx, const char *name,
    size_t count, const char *const args[])
{
    if (!name || strcmp(name, "child")) return ESP_ERR_NOT_FOUND;
    if (count > 1) return ESP_ERR_INVALID_ARG;
    char *argv[2] = {(char *)name, count ? (char *)args[0] : NULL};
    return solar_os_context_request_launch_ex(ctx, &child, count + 1, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
}
bool solar_os_app_registry_can_open(const char *path) { return path && strstr(path, ".txt"); }
esp_err_t solar_os_app_registry_request_open(solar_os_context_t *ctx, const char *path)
{ return solar_os_app_registry_can_open(path) ? solar_os_app_registry_request_launch(ctx, "child", 1, &path) : ESP_ERR_NOT_SUPPORTED; }
size_t solar_os_app_discovery_count(bool include_playground) { return include_playground ? 2 : 1; }
bool solar_os_app_discovery_get(size_t index, bool include_playground, solar_os_app_discovery_info_t *info)
{
    if (index >= solar_os_app_discovery_count(include_playground)) return false;
    *info = (solar_os_app_discovery_info_t){.kind = index ? SOLAR_OS_APP_DISCOVERY_PLAYGROUND : SOLAR_OS_APP_DISCOVERY_NATIVE};
    strcpy(info->name, index ? "playground:demo" : "child");
    strcpy(info->id, index ? "demo" : "child");
    strcpy(info->title, "Example");
    return true;
}
static esp_err_t output(const char *text, size_t length, void *user)
{ assert(user == &output_bytes && length == 4 && !memcmp(text, "core", 4)); output_bytes += length; return ESP_OK; }
static void *outside(void *user)
{
    assert(!user && !api->active() && !api->name() && !api->state() && !api->gfx());
    assert(api->finish(1, "foreign") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->discovery_count(true) == 2);
    return NULL;
}
int main(void)
{
    assert(!application_scope_checks());
    api = solar_os_native_application_get_service("application", 1, 8);
    assert(api && api->struct_size == sizeof(*api));
    assert(!solar_os_native_application_get_service("application", 2, 8));
    assert(!solar_os_native_application_get_service("application", 1, sizeof(*api) + 1));
    assert(!solar_os_native_application_get_service("missing", 1, 8));
    assert(!api->active() && !api->name() && !api->summary() && !api->argc() && !api->argv(0));
    assert(!api->cwd() && !api->state() && !api->terminal() && !api->gfx());
    assert(api->finish(0, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->write_utf8("core", 4) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_app_discovery_info_t info;
    assert(api->discovery_count(false) == 1 && !api->discovery_info(0, false, &info) && !strcmp(info.name, "child"));
    assert(!api->discovery_find("playground:demo", true, &info) && info.kind == SOLAR_OS_APP_DISCOVERY_PLAYGROUND);
    assert(api->discovery_find("playground:demo", false, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.name[0]);
    assert(api->discovery_info(2, true, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.name[0]);
    assert(api->discovery_info(0, false, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->discovery_find(NULL, false, &info) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    start_failure = false;
    assert(!solar_os_app_start(&parent, &contexts[0]));
    solar_os_application_scope_t scope;
    solar_os_application_scope_enter(&scope, &parent, &contexts[0]);
    assert(api->active() && !strcmp(api->name(), "parent") && api->state() == cold);
    assert(api->app_class() == SOLAR_OS_APP_CLASS_COMMAND);
    contexts[0].argc = 1; strcpy(contexts[0].argv[0], "parent"); strcpy(contexts[0].launch_cwd, "/flash");
    assert(api->argc() == 1 && !strcmp(api->argv(0), "parent") && !api->argv(UINT32_MAX));
    assert(!strcmp(api->cwd(), "/flash"));
    pthread_t task; assert(!pthread_create(&task, NULL, outside, NULL) && !pthread_join(task, NULL));
    assert(api->active());
    assert(api->set_app_class(0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->set_app_class(SOLAR_OS_APP_CLASS_GUI) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(api->set_graphics_active(true) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    contexts[0].gfx = (solar_os_gfx_t *)(uintptr_t)1;
    contexts[0].terminal = (solar_os_terminal_t *)(uintptr_t)2;
    assert(api->gfx() == contexts[0].gfx && api->terminal() == contexts[0].terminal);
    assert(!api->set_app_class(SOLAR_OS_APP_CLASS_GUI));
    assert(!api->set_graphics_active(true) && api->graphics_active());
    assert(!api->set_streaming_graphics_active(false) && !api->graphics_active());
    assert(!api->preserve_terminal() && contexts[0].preserve_terminal);
    contexts[0].exit_requested = false;
    assert(!api->finish(23, "first") && !api->finish(24, "second"));
    assert(contexts[0].exit_code == 23 && !strcmp(contexts[0].status_message, "first"));
    assert(!api->sleep() && !api->deep_sleep() && !api->suspend());
    assert(solar_os_context_take_sleep_request(&contexts[0]) && solar_os_context_take_deep_sleep_request(&contexts[0]));
    assert(solar_os_context_take_suspend_request(&contexts[0]));
    char argument[] = "data.txt"; const char *arguments[] = {argument};
    assert(!api->launch("child", 1, arguments, SOLAR_OS_LAUNCH_REPLACE));
    argument[0] = 'X';
    assert(contexts[0].requested_app == &child && !strcmp(contexts[0].argv[1], "data.txt"));
    assert(contexts[0].launch_policy == SOLAR_OS_LAUNCH_REPLACE);
    assert(api->launch("child", 1, arguments, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->open("file.txt") && contexts[0].launch_policy == SOLAR_OS_LAUNCH_CHILD_RETURN);
    assert(api->open("file.bin") == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && api->can_open("file.txt"));
    assert(api->print_sessions() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->request_session_list() && contexts[0].session_request == SOLAR_OS_SESSION_REQUEST_LIST);
    assert(api->request_session_fg(0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->request_session_close(256) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->request_session_fg(4) && contexts[0].session_request_id == 4);
    assert(!api->request_session_close(5) && contexts[0].session_request == SOLAR_OS_SESSION_REQUEST_CLOSE);
    assert(!api->write_utf8("core", 4) && write_bytes == 4);
    contexts[0].terminal = NULL; contexts[0].output_fn = output; contexts[0].output_user = &output_bytes;
    assert(!api->write_utf8("core", 4) && output_bytes == 4);
    assert(api->write_utf8(NULL, 1) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    solar_os_shell_io_t io = {0}; contexts[0].shell_io = &io;
    assert(!api->write_utf8("core", 4) && write_bytes == 8 && output_bytes == 4);
    contexts[0].shell_io = NULL;
    contexts[0].gfx = NULL;
    assert(!api->reboot("test") && restarts == 1);
    solar_os_application_scope_leave(&scope);
    assert(!api->active() && api->sleep() == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_application_scope_enter(&scope, NULL, &contexts[0]);
    assert(!api->active() && !api->gfx() && api->finish(1, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_application_scope_leave(&scope);
    assert(solar_os_app_stop_checked(&parent, &contexts[0]) && !cold && !allocations);
    return 0;
}
