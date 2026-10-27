#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os.h"
#include "solar_os_shell_io.h"

esp_err_t solar_os_shell_resolve_path(solar_os_context_t *ctx,
    const char *arg, char *out, size_t size)
{
    assert(ctx->shell_session != NULL && arg == NULL);
    assert(size > strlen("/sdcard/amiga"));
    strcpy(out, "/sdcard/amiga");
    return ESP_OK;
}

typedef struct {
    char text[32];
    size_t len;
} output_capture_t;

static esp_err_t capture_output(const char *text, size_t len, void *user)
{
    output_capture_t *capture = user;
    if (capture == NULL || text == NULL || capture->len + len >= sizeof(capture->text)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&capture->text[capture->len], text, len);
    capture->len += len;
    capture->text[capture->len] = '\0';
    return ESP_OK;
}

void solar_os_terminal_put_char(solar_os_terminal_t *terminal, char ch)
{
    (void)terminal;
    (void)ch;
}

void solar_os_terminal_clear(solar_os_terminal_t *terminal)
{
    (void)terminal;
}

size_t solar_os_terminal_cursor_row(const solar_os_terminal_t *terminal)
{
    (void)terminal;
    return 0U;
}

size_t solar_os_terminal_cursor_col(const solar_os_terminal_t *terminal)
{
    (void)terminal;
    return 0U;
}

bool solar_os_port_handle_valid(const solar_os_port_handle_t *handle)
{
    (void)handle;
    return false;
}

esp_err_t solar_os_port_write(const solar_os_port_handle_t *handle,
                              const uint8_t *data,
                              size_t len,
                              size_t *written)
{
    (void)handle;
    (void)data;
    if (written != NULL) {
        *written = len;
    }
    return ESP_OK;
}

int main(void)
{
    solar_os_context_t ctx;
    solar_os_context_init(&ctx, NULL, NULL);
    assert(!ctx.launch_cwd[0]);
    assert(solar_os_context_app_class(&ctx) ==
           SOLAR_OS_APP_CLASS_UNSPECIFIED);

    output_capture_t capture = {0};
    solar_os_context_set_output_handler(&ctx, capture_output, &capture);
    solar_os_shell_io_t io = {
        .kind = SOLAR_OS_SHELL_IO_KIND_TERMINAL,
        .terminal = (solar_os_terminal_t *)(uintptr_t)1U,
    };
    solar_os_context_set_shell_io(&ctx, &io);
    solar_os_context_set_app_class(&ctx, SOLAR_OS_APP_CLASS_COMMAND);
    assert(solar_os_shell_io_write(&io, "hello") == ESP_OK);
    assert(solar_os_shell_io_clear(&io) == ESP_OK);
    assert(solar_os_shell_io_write(&io, " world") == ESP_OK);
    assert(strcmp(capture.text, "hello world") == 0);

    solar_os_context_set_app_class(&ctx, SOLAR_OS_APP_CLASS_TUI);
    assert(solar_os_shell_io_write(&io, " hidden screen") == ESP_OK);
    assert(strcmp(capture.text, "hello world") == 0);

    solar_os_context_finish(&ctx, 7, "could not load");
    solar_os_context_finish(&ctx, 0, "overwritten");
    assert(solar_os_context_take_exit_request(&ctx));
    assert(!solar_os_context_take_terminal_preserve(&ctx));
    int exit_code = 0;
    assert(solar_os_context_take_exit_result(&ctx, &exit_code));
    assert(exit_code == 7);
    char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    assert(solar_os_context_take_status_message(&ctx, message, sizeof(message)));
    assert(strcmp(message, "could not load") == 0);

    solar_os_context_set_app_class(&ctx, SOLAR_OS_APP_CLASS_COMMAND);
    solar_os_context_request_deep_sleep(&ctx);
    assert(solar_os_context_take_deep_sleep_request(&ctx));
    assert(!solar_os_context_take_deep_sleep_request(&ctx));
    solar_os_context_finish(&ctx, 0, NULL);
    assert(solar_os_context_take_exit_request(&ctx));
    assert(solar_os_context_take_terminal_preserve(&ctx));
    assert(solar_os_context_take_exit_result(&ctx, &exit_code));
    assert(exit_code == 0);

    static const solar_os_app_t app = {.name = "test", .app_class = SOLAR_OS_APP_CLASS_COMMAND};
    char *args[] = {"test"};
    strcpy(ctx.launch_cwd, "/flash");
    assert(solar_os_context_request_launch(&ctx, &app, 1, args) == ESP_OK);
    assert(!ctx.launch_cwd[0]);
    ctx.shell_session = (solar_os_shell_session_t *)(uintptr_t)1;
    char *relative_args[] = {"amiga", "kick13.rom", "workbench.adf"};
    assert(solar_os_context_request_launch(&ctx, &app, 3, relative_args) == ESP_OK);
    assert(strcmp(ctx.launch_cwd, "/sdcard/amiga") == 0);
    assert(strcmp(ctx.argv[1], "kick13.rom") == 0 && strcmp(ctx.argv[2], "workbench.adf") == 0);
    puts("app exit result tests passed");
    return 0;
}
