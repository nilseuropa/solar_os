#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static char captured[1024];
static size_t captured_len;

static esp_err_t capture(const char *text, size_t length)
{
    if (text == NULL || captured_len + length >= sizeof(captured)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(captured + captured_len, text, length);
    captured_len += length;
    captured[captured_len] = '\0';
    return ESP_OK;
}

esp_err_t solar_os_shell_io_write(solar_os_shell_io_t *io, const char *text)
{
    (void)io;
    return capture(text, strlen(text));
}

esp_err_t solar_os_shell_io_writeln(solar_os_shell_io_t *io, const char *text)
{
    esp_err_t err = solar_os_shell_io_write(io, text);
    return err == ESP_OK ? capture("\n", 1U) : err;
}

esp_err_t solar_os_shell_io_newline(solar_os_shell_io_t *io)
{
    (void)io;
    return capture("\n", 1U);
}

esp_err_t solar_os_shell_io_printf(solar_os_shell_io_t *io,
                                   const char *format,
                                   ...)
{
    (void)io;
    char text[256];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return capture(text, (size_t)length);
}

int main(void)
{
    char usage[420];
    size_t offset = 0U;
    while (offset + 8U < sizeof(usage)) {
        memcpy(usage + offset, "command\n", 8U);
        offset += 8U;
    }
    usage[offset] = '\0';

    solar_os_shell_io_t io = {0};
    solar_os_shell_diag_problem(&io, "control", "invalid command", usage,
                                "use man control");

    assert(strncmp(captured, "control: invalid command\nusage: ", 32U) == 0);
    assert(strstr(captured, usage) != NULL);
    assert(strstr(captured, "\nhint: use man control\n") != NULL);
    assert(captured_len > 192U);
    puts("shell diagnostic tests passed");
    return 0;
}
