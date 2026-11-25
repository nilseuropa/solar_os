#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_module_packages.h"

#define SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES 1
#define SOLAR_OS_SHELL_IO_KIND_PORT 2
#define PKG_NETWORK_WAIT_MS 50U
#define pdMS_TO_TICKS(n) (n)
typedef struct { int kind; int port; } solar_os_shell_io_t;
typedef struct { int code; } solar_os_shell_session_t;
typedef struct { solar_os_shell_session_t *shell_session; bool exit_requested; } solar_os_context_t;
static solar_os_shell_io_t io;
static solar_os_module_package_t packages[1];
static solar_os_module_upgrade_entry_t entries[1];
static solar_os_module_catalog_t catalog;
static solar_os_module_upgrade_plan_t plan;
static char output[8192], input[64];
static size_t input_at, input_count;
static bool disconnected, saved_failure;
static unsigned fetches, plans, checks, applies, frees, prompts, worker_calls;
static esp_err_t fetch_error, plan_error, check_error, apply_error;
static int selected_action;
static bool selected_yes;
static const char *selected_id;
static solar_os_module_type_t loaded_type;
static unsigned unloads;
/* DEFINITIONS */
static esp_err_t emit(solar_os_shell_io_t *term, const char *format, ...)
{
    (void)term; va_list args; va_start(args, format);
    size_t n = strlen(output); int written = vsnprintf(output + n, sizeof(output) - n, format, args);
    va_end(args); assert(written >= 0 && (size_t)written < sizeof(output) - n);
    if (strstr(format, "Apply these changes?")) ++prompts;
    return ESP_OK;
}
static void solar_os_shell_io_flush(solar_os_shell_io_t *term) { (void)term; }
static esp_err_t solar_os_shell_io_writeln(solar_os_shell_io_t *term, const char *text) { return emit(term, "%s\n", text); }
static esp_err_t solar_os_shell_io_write(solar_os_shell_io_t *term, const char *text)
{ if (strstr(text, "Apply these changes?")) ++prompts; return emit(term, "%s", text); }
#define solar_os_shell_io_printf emit
#define solar_os_shell_io_table_printf emit
static const char *solar_os_shell_io_app_exit_key(solar_os_shell_io_t *term) { (void)term; return "Ctrl+]"; }
static int solar_os_shell_io_kind(solar_os_shell_io_t *term) { return term->kind; }
static bool solar_os_port_handle_valid(int *port) { (void)port; return !disconnected; }
static size_t read_input(uint8_t *bytes, size_t capacity)
{ size_t n = input_count - input_at; if (n > capacity) n = capacity; memcpy(bytes, input + input_at, n); input_at += n; return n; }
static esp_err_t solar_os_port_read(int *port, uint8_t *bytes, size_t n, unsigned timeout, size_t *count)
{ (void)port; (void)timeout; if (disconnected) return ESP_FAIL; *count = read_input(bytes, n); return ESP_OK; }
static size_t solar_os_input_read_chars(char *bytes, size_t n) { return read_input((uint8_t *)bytes, n); }
static void vTaskDelay(unsigned ticks) { (void)ticks; assert(!"confirmation unexpectedly waited for input"); }
static void solar_os_shell_diag_esp(solar_os_shell_io_t *term, const char *name, esp_err_t err, const char *detail, const char *unused)
{ (void)unused; emit(term, "%s: error %d %s\n", name, err, detail); }
esp_err_t solar_os_module_catalog_fetch_ex(solar_os_module_catalog_t **out,
    solar_os_module_package_cancel_fn cancel, void *user, char *detail, size_t n)
{ (void)cancel; (void)user; ++fetches; *out = fetch_error ? NULL : &catalog; snprintf(detail, n, "%s", saved_failure ? "catalog cache could not be saved" : ""); return fetch_error; }
void solar_os_module_catalog_free(solar_os_module_catalog_t *p) { (void)p; }
esp_err_t solar_os_module_upgrade_plan(const char *id, const solar_os_module_install_options_t *options,
    solar_os_module_upgrade_plan_t **out, char *detail, size_t n)
{ (void)id; (void)options; ++plans; *out = plan_error ? NULL : &plan; snprintf(detail, n, "test plan"); return plan_error; }
esp_err_t solar_os_module_upgrade_check(const solar_os_module_upgrade_plan_t *p,
    const solar_os_module_install_options_t *options, char *detail, size_t n)
{ (void)p; (void)options; ++checks; snprintf(detail, n, "test preflight"); return check_error; }
esp_err_t solar_os_module_upgrade_apply(solar_os_module_upgrade_plan_t *p,
    const solar_os_module_install_options_t *options, size_t *completed, char *detail, size_t n)
{ ++applies; assert(options->should_cancel && options->progress); *completed = apply_error ? 0 : p->count; snprintf(detail, n, "test apply"); return apply_error; }
void solar_os_module_upgrade_plan_free(solar_os_module_upgrade_plan_t *p) { assert(p == &plan); ++frees; }
const char *solar_os_module_type_name(solar_os_module_type_t type) { return type == SOLAR_OS_MODULE_TYPE_APP ? "app" : "library"; }
static solar_os_shell_io_t *terminal(solar_os_context_t *ctx) { (void)ctx; return &io; }
static esp_err_t solar_os_shell_launch_pkg_tui(solar_os_context_t *ctx) { (void)ctx; return ESP_OK; }
static const char *solar_os_shell_error_text(esp_err_t err) { (void)err; return "error"; }
static void solar_os_shell_session_prepare_foreground_launch(solar_os_context_t *ctx, bool value) { (void)ctx; (void)value; }
static void pkg_print_system(solar_os_shell_io_t *term) { (void)term; }
static void pkg_print_installed(solar_os_shell_io_t *term) { (void)term; }
static void pkg_remove_module(solar_os_shell_io_t *term, const char *id) { (void)term; (void)id; }
static esp_err_t pkg_run_network_worker(solar_os_shell_io_t *term, pkg_network_action_t action, const char *id, bool yes)
{ (void)term; ++worker_calls; selected_action = action; selected_id = id; selected_yes = yes; return apply_error; }
static void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)message; ctx->shell_session->code = code; ctx->exit_requested = true; }
static void solar_os_shell_session_set_exit_result(solar_os_shell_session_t *session, int code, const char *message)
{ (void)message; session->code = code; }
static bool solar_os_native_module_active(solar_os_module_type_t type, const char *id) { (void)id; return type == loaded_type; }
static esp_err_t solar_os_native_module_deactivate(solar_os_module_type_t type, const char *id, char *detail, size_t n)
{ (void)type; (void)id; (void)detail; (void)n; ++unloads; return ESP_OK; }
static void solar_os_shell_diag_subcommand(solar_os_shell_io_t *term, const char *name, int argc,
    char **argv, const char *usage, const char *const *subcommands, size_t count)
{ (void)name; (void)argc; (void)argv; (void)subcommands; (void)count; emit(term, "usage: %s\n", usage); }
/* SHELL */

static void reset(const char *keys)
{
    memset(output, 0, sizeof(output)); memset(input, 0, sizeof(input));
    input_at = 0; input_count = strlen(keys); memcpy(input, keys, input_count);
    fetches = plans = checks = applies = frees = prompts = worker_calls = 0;
    unloads = 0; loaded_type = SOLAR_OS_MODULE_TYPE_INVALID;
    disconnected = saved_failure = false; fetch_error = plan_error = check_error = apply_error = ESP_OK;
    memset(&catalog, 0, sizeof(catalog)); memset(&plan, 0, sizeof(plan));
    catalog.packages = packages; plan.entries = entries;
    memset(packages, 0, sizeof(packages)); memset(entries, 0, sizeof(entries));
    catalog.count = 1; catalog.packages[0].type = SOLAR_OS_MODULE_TYPE_LIBRARY;
    strcpy(catalog.packages[0].id, "jet"); strcpy(catalog.packages[0].version, "1.12.0");
    plan.catalog = &catalog; plan.count = 1; plan.download_bytes = 100;
    plan.entries[0].installed = true; strcpy(plan.entries[0].previous_version, "1.11.0");
}
static esp_err_t run(pkg_network_action_t action, bool yes)
{ pkg_network_worker_t worker = {.term = &io, .action = action, .assume_yes = yes}; return pkg_update_or_upgrade(&worker); }

int main(void)
{
    reset(""); assert(run(PKG_NETWORK_UPDATE, false) == ESP_OK);
    assert(fetches == 1 && plans == 1 && !checks && !applies && !prompts && frees == 1);
    assert(strstr(output, "library jet: 1.11.0 -> 1.12.0 (available for upgrade)"));
    reset(""); plan.entries[0].previous_version[0] = 0;
    assert(run(PKG_NETWORK_UPDATE, false) == ESP_OK && strstr(output, "unknown -> 1.12.0 (available for upgrade)"));
    reset("y"); assert(run(PKG_NETWORK_UPGRADE, false) == ESP_OK);
    assert(!fetches && checks == 1 && applies == 1 && prompts == 1 && frees == 1);
    const char *no[] = {"\n", "n", "\r", "\x1b", "\x03", "\x1d"};
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); ++i) {
        reset(no[i]); assert(run(PKG_NETWORK_UPGRADE, false) == ESP_OK);
        assert(!applies && prompts == 1 && frees == 1 && strstr(output, "cancelled"));
    }
    reset(""); assert(run(PKG_NETWORK_UPGRADE, true) == ESP_OK && applies == 1 && !prompts);
    reset(""); check_error = ESP_ERR_INVALID_STATE;
    assert(run(PKG_NETWORK_UPGRADE, true) == check_error && !applies && !prompts && frees == 1);
    reset(""); apply_error = ESP_FAIL;
    assert(run(PKG_NETWORK_UPGRADE, true) == ESP_FAIL && applies == 1 && frees == 1);
    reset(""); fetch_error = ESP_ERR_INVALID_CRC;
    assert(run(PKG_NETWORK_UPDATE, false) == fetch_error && !plans && !applies);
    reset(""); saved_failure = true;
    assert(run(PKG_NETWORK_UPDATE, false) == ESP_FAIL && !plans);
    reset(""); plan_error = ESP_ERR_NOT_FOUND;
    assert(run(PKG_NETWORK_UPGRADE, true) == plan_error && !frees);
    reset(""); plan.count = 0;
    assert(run(PKG_NETWORK_UPGRADE, false) == ESP_OK && !prompts && !applies && frees == 1);
    for (int port = 0; port < 2; ++port) {
        reset("\x03"); io.kind = port ? SOLAR_OS_SHELL_IO_KIND_PORT : 1;
        pkg_install_shell_state_t state = {.term = &io};
        assert(pkg_install_cancelled(&state) && pkg_install_cancelled(&state));
        reset("Y"); assert(run(PKG_NETWORK_UPGRADE, false) == ESP_OK && applies == 1);
    }
    reset(""); io.kind = SOLAR_OS_SHELL_IO_KIND_PORT; disconnected = true;
    assert(run(PKG_NETWORK_UPGRADE, false) == ESP_OK && !applies && frees == 1);
    solar_os_shell_session_t session = {0};
    solar_os_context_t ctx = {.shell_session = &session};
    char *all[] = {"pkg", "upgrade", "-y"};
    reset(""); solar_os_shell_cmd_pkg(&ctx, 3, all);
    assert(worker_calls == 1 && selected_action == PKG_NETWORK_UPGRADE && selected_yes && !selected_id && !session.code && !ctx.exit_requested);
    char *one[] = {"pkg", "upgrade", "jet", "-y"};
    reset(""); solar_os_shell_cmd_pkg(&ctx, 4, one);
    assert(worker_calls == 1 && selected_yes && !strcmp(selected_id, "jet"));
    char *invalid[] = {"pkg", "upgrade", "-y", "-y"};
    reset(""); solar_os_shell_cmd_pkg(&ctx, 4, invalid);
    assert(!worker_calls && strstr(output, "usage:"));
    char *update[] = {"pkg", "update"};
    reset(""); apply_error = ESP_FAIL; solar_os_shell_cmd_pkg(&ctx, 2, update);
    assert(worker_calls == 1 && selected_action == PKG_NETWORK_UPDATE && session.code == 1 && !ctx.exit_requested);
    char *unload[] = {"pkg", "unload", "jet"};
    reset(""); loaded_type = SOLAR_OS_MODULE_TYPE_LIBRARY;
    solar_os_shell_cmd_pkg(&ctx, 3, unload);
    assert(unloads == 1 && strstr(output, "pkg: unloaded library jet"));
    reset(""); loaded_type = SOLAR_OS_MODULE_TYPE_JOB;
    solar_os_shell_cmd_pkg(&ctx, 3, unload); assert(unloads == 1);
    puts("pkg shell: update availability, confirmation defaults, single-module/-y parsing, cancellation, cache errors and exit status passed");
}
