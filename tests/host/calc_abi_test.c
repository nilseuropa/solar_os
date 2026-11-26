#include <stdarg.h>
#include <stdio.h>
#include "gfx_fixture.h"
#include "solar_os_calc.h"
#include "solar_os_native_application.h"
#include "solar_os_native_abi.h"
#include "solar_os_app_registry.h"
#include "solar_os_shell_io.h"
#include "solar_os_keys.h"
#include "freertos/FreeRTOS.h"

static unsigned lookups;
static bool missing_service;
static char output[32768];
static size_t output_length;
const void *__real_solar_os_native_application_get_service(const char *name, uint32_t abi, uint32_t size);
const void *__wrap_solar_os_native_application_get_service(const char *name, uint32_t abi, uint32_t size)
{
    ++lookups;
    return missing_service ? NULL : __real_solar_os_native_application_get_service(name, abi, size);
}
const void *__real_solar_os_native_gfx_get_service(const char *name, uint32_t abi, uint32_t size);
const void *__wrap_solar_os_native_gfx_get_service(const char *name, uint32_t abi, uint32_t size)
{
    ++lookups;
    return missing_service ? NULL : __real_solar_os_native_gfx_get_service(name, abi, size);
}
bool solar_os_shutdown_requested(void) { return false; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
void solar_os_splash_draw_reboot(solar_os_gfx_t *gfx, const char *status) { (void)gfx; (void)status; }
void esp_restart(void) { abort(); }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void solar_os_shell_io_capture_output(solar_os_shell_io_t *io, solar_os_context_t *ctx)
{ (void)io; (void)ctx; }
void solar_os_shell_io_init_terminal(solar_os_shell_io_t *io, solar_os_terminal_t *terminal)
{ *io = (solar_os_shell_io_t){.kind = SOLAR_OS_SHELL_IO_KIND_TERMINAL, .terminal = terminal, .cols = 80}; }
solar_os_shell_io_kind_t solar_os_shell_io_kind(const solar_os_shell_io_t *io) { return io->kind; }
esp_err_t solar_os_shell_io_write_len(solar_os_shell_io_t *io, const char *text, size_t length)
{
    assert(io && text && output_length + length < sizeof(output));
    memcpy(output + output_length, text, length); output_length += length; output[output_length] = 0;
    return ESP_OK;
}
esp_err_t solar_os_shell_io_write(solar_os_shell_io_t *io, const char *text)
{ return solar_os_shell_io_write_len(io, text, strlen(text)); }
esp_err_t solar_os_shell_io_writeln(solar_os_shell_io_t *io, const char *text)
{ solar_os_shell_io_write(io, text); return solar_os_shell_io_write(io, "\n"); }
esp_err_t solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *fmt, ...)
{
    char buffer[512]; va_list args; va_start(args, fmt); vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args);
    return solar_os_shell_io_write(io, buffer);
}
esp_err_t solar_os_shell_io_flush(solar_os_shell_io_t *io) { assert(io); return ESP_OK; }
uint16_t solar_os_shell_io_cols(const solar_os_shell_io_t *io) { return io->cols; }
size_t solar_os_shell_io_cursor_row(const solar_os_shell_io_t *io) { return io->cursor_row; }
size_t solar_os_shell_io_cursor_col(const solar_os_shell_io_t *io) { return io->cursor_col; }
esp_err_t solar_os_shell_io_set_cursor(solar_os_shell_io_t *io, size_t row, size_t col)
{ io->cursor_row = row; io->cursor_col = col; return ESP_OK; }
esp_err_t solar_os_shell_io_clear_line_from(solar_os_shell_io_t *io, size_t row, size_t col)
{ (void)row; (void)col; assert(io); return ESP_OK; }
esp_err_t solar_os_shell_io_newline(solar_os_shell_io_t *io) { return solar_os_shell_io_write(io, "\n"); }
esp_err_t solar_os_shell_resolve_path(solar_os_context_t *ctx, const char *path, char *out, size_t size)
{ assert(ctx); snprintf(out, size, "%s", path ? path : "/"); return ESP_OK; }
esp_err_t solar_os_app_registry_request_launch(solar_os_context_t *ctx, const char *name,
    size_t argc, const char *const args[])
{ (void)ctx; (void)name; (void)argc; (void)args; return ESP_ERR_NOT_SUPPORTED; }
bool solar_os_app_registry_can_open(const char *path) { (void)path; return false; }
bool solar_os_app_registry_opener_info(const char *path, solar_os_app_discovery_info_t *info)
{ (void)path; (void)info; return false; }
const solar_os_app_registry_entry_t *solar_os_app_registry_find(const char *name)
{ (void)name; return NULL; }
bool solar_os_app_registry_owner(const solar_os_app_t *app, char *owner, size_t capacity)
{ (void)app; if (owner && capacity) owner[0] = 0; return false; }
esp_err_t solar_os_app_registry_foreach_module(bool (*visit)(const char *id, void *user), void *user)
{ (void)visit; (void)user; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_app_registry_request_open(solar_os_context_t *ctx, const char *path)
{ (void)ctx; (void)path; return ESP_ERR_NOT_SUPPORTED; }
size_t solar_os_app_discovery_count(bool playground) { (void)playground; return 0; }
bool solar_os_app_discovery_get(size_t index, bool playground, solar_os_app_discovery_info_t *info)
{ (void)index; (void)playground; (void)info; return false; }
size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t len = strlen(src); if (size) { size_t n = len < size ? len : size-1; memcpy(dst, src, n); dst[n] = 0; } return len; }
size_t strlcat(char *dst, const char *src, size_t size)
{ size_t len = strnlen(dst, size); return len + strlcpy(dst + len, src, size - len); }

static void prepare(solar_os_context_t *ctx, solar_os_gfx_t *gfx, solar_os_shell_io_t *io,
                    int argc, const char *const argv[])
{
    *ctx = (solar_os_context_t){.gfx = gfx, .shell_io = io, .argc = argc};
    for (int i=0; i<argc; ++i) strlcpy(ctx->argv[i], argv[i], sizeof(ctx->argv[i]));
    lookups = 0; output_length = 0; output[0] = 0;
}
static void character(solar_os_context_t *ctx, char ch)
{
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = ch};
    assert(solar_os_app_event(&solar_os_calc_app, ctx, &event));
}
static void type(solar_os_context_t *ctx, const char *text) { while (*text) character(ctx, *text++); }
static void stop(solar_os_context_t *ctx)
{
    assert(solar_os_app_stop_checked(&solar_os_calc_app, ctx));
    assert(!*solar_os_calc_app.state_slot && !solar_os_application_context());
}
int main(void)
{
    solar_os_context_t ctx;
    solar_os_shell_io_t port = {.kind = SOLAR_OS_SHELL_IO_KIND_PORT, .cols = 80};
    const char *eval[] = {"calc", "-e", "2+3*4"};
    unsigned before = allocations;
    prepare(&ctx, NULL, &port, 3, eval);
    assert(!solar_os_app_start(&solar_os_calc_app, &ctx));
    assert(!strcmp(output, "14\n") && ctx.exit_code == 0 && ctx.exit_requested);
    assert(ctx.app_class == SOLAR_OS_APP_CLASS_COMMAND && lookups == 2 && allocations == before+1);
    stop(&ctx); assert(ctx.shell_io == &port);
    const char *bad[] = {"calc", "--invalid"};
    prepare(&ctx, NULL, &port, 2, bad); assert(!solar_os_app_start(&solar_os_calc_app, &ctx));
    assert(ctx.exit_code == 2 && strstr(output, "unknown option")); stop(&ctx);
    const char *syntax[] = {"calc", "-e", "2+"};
    prepare(&ctx, NULL, &port, 3, syntax); assert(!solar_os_app_start(&solar_os_calc_app, &ctx));
    assert(ctx.exit_code == 1); stop(&ctx);

    const char *interactive[] = {"calc"};
    prepare(&ctx, NULL, NULL, 1, interactive); before = allocations;
    assert(!solar_os_app_start(&solar_os_calc_app, &ctx) && ctx.shell_io);
    assert(ctx.app_class == SOLAR_OS_APP_CLASS_TUI && strstr(output, "calculator"));
    type(&ctx, "2+3*4\n"); assert(strstr(output, "14\n"));
    type(&ctx, ":quit\n"); assert(ctx.exit_requested && !ctx.exit_code);
    assert(lookups == 2 && allocations == before+1);
    stop(&ctx); assert(!ctx.shell_io); /* No reference into the released cold state. */

    uint8_t buffer[52*52*8];
    u8g2_t u = {0}; u8g2_SetupDisplay(&u, display, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&u, buffer, 52, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    solar_os_display_geometry_t geometry; solar_os_display_geometry_init(&geometry, 412, 412, false);
    u8g2_SetUserPtr(&u, &geometry);
    solar_os_gfx_t gfx; solar_os_gfx_init(&gfx, &u); solar_os_gfx_set_black_is_one(&gfx, true);
    prepare(&ctx, &gfx, NULL, 1, interactive); before = allocations;
    assert(!solar_os_app_start(&solar_os_calc_app, &ctx));
    assert(ctx.app_class == SOLAR_OS_APP_CLASS_GUI && ctx.graphics_active && ordinary_frames);
    unsigned frames = ordinary_frames; void *state = *solar_os_calc_app.state_slot;
    type(&ctx, "y=x"); assert(ordinary_frames == frames+3 && mono_pixel(&u, 300, 173));
    assert(lookups == 2);
    solar_os_app_suspend(&solar_os_calc_app, &ctx); assert(!ctx.graphics_active);
    frames = ordinary_frames; solar_os_app_resume(&solar_os_calc_app, &ctx);
    assert(ctx.graphics_active && ordinary_frames == frames+1 && *solar_os_calc_app.state_slot == state);
    character(&ctx, SOLAR_OS_KEY_APP_EXIT); assert(ctx.exit_requested);
    stop(&ctx); assert(!ctx.graphics_active && allocations == before+1);

    /* A remote shell stays in text mode even when a display is attached. */
    prepare(&ctx, &gfx, &port, 1, interactive); frames = ordinary_frames;
    assert(!solar_os_app_start(&solar_os_calc_app, &ctx));
    assert(ctx.app_class == SOLAR_OS_APP_CLASS_TUI && ordinary_frames == frames); stop(&ctx);
    missing_service = true;
    prepare(&ctx, NULL, &port, 1, interactive);
    assert(solar_os_app_start(&solar_os_calc_app, &ctx) == ESP_ERR_NOT_SUPPORTED);
    assert(!*solar_os_calc_app.state_slot && ctx.exit_code == 1); stop(&ctx);
    solar_os_gfx_release_surface(&gfx);
    return 0;
}
