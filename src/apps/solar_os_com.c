#include "solar_os_com.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_log.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_buses.h"
#include "solar_os_port.h"
#include "solar_os_shell_io.h"
#include "solar_os_uart.h"

#define COM_RX_BUFFER_SIZE 128
#define COM_RX_CHUNKS_PER_TICK 16
#define COM_BUS_OWNER "app-com"

typedef struct {
    bool active;
    bool alt_prefix_pending;
    bool bus_leased;
    char bus_name[SOLAR_OS_BUS_NAME_MAX];
    solar_os_shell_io_t fallback_io;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} com_app_state_t;

static const char *TAG = "solar_os_com";
static com_app_state_t com_app;

static solar_os_shell_io_t *com_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        return io;
    }

    solar_os_terminal_t *terminal = solar_os_context_terminal(ctx);
    if (solar_os_shell_io_kind(&com_app.fallback_io) != SOLAR_OS_SHELL_IO_KIND_TERMINAL ||
        solar_os_shell_io_terminal(&com_app.fallback_io) != terminal) {
        solar_os_shell_io_init_terminal(&com_app.fallback_io, terminal);
    }
    return &com_app.fallback_io;
}

static void com_flush(solar_os_context_t *ctx)
{
    (void)solar_os_shell_io_flush(com_io(ctx));
}

static void com_render_header(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = com_io(ctx);
    solar_os_uart_status_t status;

    (void)solar_os_shell_io_clear(io);
    if (solar_os_uart_get_bus_status(com_app.bus_name, &status)) {
        (void)solar_os_shell_io_printf_bold(io,
                                           "COM %s UART%d %" PRIu32 " %s\n",
                                           com_app.bus_name,
                                           status.port_num,
                                           status.baud_rate,
                                           solar_os_uart_mode_name(status.mode));
        (void)solar_os_shell_io_printf(io, "TX %d RX %d\n", status.tx_pin, status.rx_pin);
    } else {
        (void)solar_os_shell_io_printf_bold(io, "COM %s\n", com_app.bus_name);
    }
    (void)solar_os_shell_io_printf(io, "%s exits\n\n", solar_os_shell_io_app_exit_key(io));
    com_flush(ctx);
}

static esp_err_t com_write_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_OK;
    }

    size_t written = 0;
    const esp_err_t err = solar_os_bus_uart_write(com_app.bus_name, data, len, &written);
    if (err == ESP_OK) {
        com_app.tx_bytes += written;
    } else {
        SOLAR_OS_LOGW(TAG, "UART write failed: %s", esp_err_to_name(err));
    }
    if (written != len) {
        SOLAR_OS_LOGW(TAG, "UART write incomplete: %u/%u", (unsigned)written, (unsigned)len);
    }
    return err;
}

static void com_send_key(solar_os_context_t *ctx, char ch)
{
    solar_os_shell_io_t *io = com_io(ctx);
    const uint8_t key = (uint8_t)ch;
    const char *seq = NULL;
    uint8_t data[2] = {0};
    size_t len = 0;

    switch (key) {
    case SOLAR_OS_KEY_ALT_PREFIX:
        com_app.alt_prefix_pending = true;
        return;
    case SOLAR_OS_KEY_UP:
        seq = "\x1b[A";
        break;
    case SOLAR_OS_KEY_DOWN:
        seq = "\x1b[B";
        break;
    case SOLAR_OS_KEY_RIGHT:
        seq = "\x1b[C";
        break;
    case SOLAR_OS_KEY_LEFT:
        seq = "\x1b[D";
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        seq = "\x1b[5~";
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        seq = "\x1b[6~";
        break;
    case SOLAR_OS_KEY_HOME:
        seq = "\x1b[H";
        break;
    case SOLAR_OS_KEY_END:
        seq = "\x1b[F";
        break;
    case SOLAR_OS_KEY_DELETE:
        seq = "\x1b[3~";
        break;
    case SOLAR_OS_KEY_SHIFT_UP:
        seq = "\x1b[1;2A";
        break;
    case SOLAR_OS_KEY_SHIFT_DOWN:
        seq = "\x1b[1;2B";
        break;
    case SOLAR_OS_KEY_SHIFT_RIGHT:
        seq = "\x1b[1;2C";
        break;
    case SOLAR_OS_KEY_SHIFT_LEFT:
        seq = "\x1b[1;2D";
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_UP:
        seq = "\x1b[5;2~";
        break;
    case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
        seq = "\x1b[6;2~";
        break;
    case SOLAR_OS_KEY_SHIFT_HOME:
        seq = "\x1b[1;2H";
        break;
    case SOLAR_OS_KEY_SHIFT_END:
        seq = "\x1b[1;2F";
        break;
    case SOLAR_OS_KEY_CTRL_UP:
        seq = "\x1b[1;5A";
        break;
    case SOLAR_OS_KEY_CTRL_DOWN:
        seq = "\x1b[1;5B";
        break;
    case SOLAR_OS_KEY_CTRL_RIGHT:
        seq = "\x1b[1;5C";
        break;
    case SOLAR_OS_KEY_CTRL_LEFT:
        seq = "\x1b[1;5D";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_UP:
        seq = "\x1b[1;6A";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_DOWN:
        seq = "\x1b[1;6B";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_RIGHT:
        seq = "\x1b[1;6C";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_LEFT:
        seq = "\x1b[1;6D";
        break;
    case SOLAR_OS_KEY_CTRL_HOME:
        seq = "\x1b[1;5H";
        break;
    case SOLAR_OS_KEY_CTRL_END:
        seq = "\x1b[1;5F";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_HOME:
        seq = "\x1b[1;6H";
        break;
    case SOLAR_OS_KEY_CTRL_SHIFT_END:
        seq = "\x1b[1;6F";
        break;
    case SOLAR_OS_KEY_F1:
        seq = "\x1b[11~";
        break;
    case SOLAR_OS_KEY_F2:
        seq = "\x1b[12~";
        break;
    case SOLAR_OS_KEY_F3:
        seq = "\x1b[13~";
        break;
    case SOLAR_OS_KEY_F4:
        seq = "\x1b[14~";
        break;
    case SOLAR_OS_KEY_F5:
        seq = "\x1b[15~";
        break;
    case SOLAR_OS_KEY_F6:
        seq = "\x1b[17~";
        break;
    case SOLAR_OS_KEY_F7:
        seq = "\x1b[18~";
        break;
    case SOLAR_OS_KEY_F8:
        seq = "\x1b[19~";
        break;
    case SOLAR_OS_KEY_F9:
        seq = "\x1b[20~";
        break;
    case SOLAR_OS_KEY_F10:
        seq = "\x1b[21~";
        break;
    case SOLAR_OS_KEY_F11:
        seq = "\x1b[23~";
        break;
    case SOLAR_OS_KEY_F12:
        seq = "\x1b[24~";
        break;
    case '\b':
        data[0] = 0x7f;
        len = 1;
        break;
    case '\n':
    case '\r':
        data[0] = '\r';
        len = 1;
        break;
    default:
        if (key >= 0x80) {
            return;
        }
        data[0] = key;
        len = 1;
        break;
    }

    if (com_app.alt_prefix_pending) {
        com_app.alt_prefix_pending = false;
        if (com_write_bytes((const uint8_t *)"\x1b", 1) != ESP_OK) {
            (void)solar_os_shell_io_writeln(io, "\ncom: UART write failed");
            com_flush(ctx);
            return;
        }
    }

    const esp_err_t err = seq != NULL ?
        com_write_bytes((const uint8_t *)seq, strlen(seq)) :
        com_write_bytes(data, len);
    if (err != ESP_OK) {
        (void)solar_os_shell_io_writeln(io, "\ncom: UART write failed");
        com_flush(ctx);
    }
}

static void com_drain_rx(solar_os_context_t *ctx)
{
    if (!com_app.active) {
        return;
    }

    solar_os_shell_io_t *io = com_io(ctx);
    uint8_t buffer[COM_RX_BUFFER_SIZE];
    bool output_changed = false;

    for (size_t chunk = 0; chunk < COM_RX_CHUNKS_PER_TICK; chunk++) {
        size_t read_len = 0;
        const esp_err_t err =
            solar_os_bus_uart_read(com_app.bus_name, buffer, sizeof(buffer), 0, &read_len);
        if (err != ESP_OK) {
            (void)solar_os_shell_io_printf(io,
                                           "\ncom: UART read failed: %s\n",
                                           esp_err_to_name(err));
            SOLAR_OS_LOGW(TAG, "UART read failed: %s", esp_err_to_name(err));
            com_app.active = false;
            com_flush(ctx);
            return;
        }
        if (read_len == 0) {
            break;
        }

        com_app.rx_bytes += read_len;
        output_changed = true;
        if (solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
            (void)solar_os_shell_io_write_raw(io, (const char *)buffer, read_len);
        } else {
            for (size_t i = 0; i < read_len; i++) {
                (void)solar_os_shell_io_put_utf8_byte(io, buffer[i]);
            }
        }
    }
    if (output_changed) {
        com_flush(ctx);
    }
}

static esp_err_t com_start(solar_os_context_t *ctx)
{
    memset(&com_app, 0, sizeof(com_app));

    const int argc = solar_os_context_argc(ctx);
    if (argc < 1 || argc > 2) {
        solar_os_shell_io_t *io = com_io(ctx);
        (void)solar_os_shell_io_clear(io);
        (void)solar_os_shell_io_write_bold(io, "com");
        (void)solar_os_shell_io_newline(io);
        (void)solar_os_shell_io_writeln(io, "usage: com [bus]");
        (void)solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        com_flush(ctx);
        return ESP_OK;
    }

    const char *bus_name = argc == 2
        ? solar_os_context_argv(ctx, 1)
        : SOLAR_OS_UART_PORT_NAME;
    strlcpy(com_app.bus_name, bus_name, sizeof(com_app.bus_name));

    solar_os_bus_info_t bus_info;
    if (!solar_os_bus_find(bus_name, SOLAR_OS_BUS_PROTOCOL_UART, &bus_info)) {
        com_render_header(ctx);
        (void)solar_os_shell_io_printf(com_io(ctx),
                                       "com: UART bus not found: %s\n",
                                       com_app.bus_name);
        com_flush(ctx);
        return ESP_OK;
    }
    strlcpy(com_app.bus_name, bus_info.name, sizeof(com_app.bus_name));

    const esp_err_t lease_err = solar_os_bus_acquire(com_app.bus_name,
                                                     SOLAR_OS_BUS_PROTOCOL_UART,
                                                     COM_BUS_OWNER);
    com_render_header(ctx);
    if (lease_err != ESP_OK) {
        solar_os_port_info_t info;
        if (lease_err == ESP_ERR_INVALID_STATE &&
            solar_os_port_get_info(com_app.bus_name, &info) == ESP_OK &&
            info.claimed) {
            (void)solar_os_shell_io_printf(com_io(ctx),
                                           "com: %s is busy: %s\n",
                                           com_app.bus_name,
                                           info.owner);
        } else {
            (void)solar_os_shell_io_printf(com_io(ctx),
                                           "com: %s lease failed: %s\n",
                                           com_app.bus_name,
                                           esp_err_to_name(lease_err));
        }
        com_flush(ctx);
        return ESP_OK;
    }

    com_app.bus_leased = true;
    com_app.active = true;
    SOLAR_OS_LOGI(TAG, "COM app started on %s", com_app.bus_name);
    return ESP_OK;
}

static void com_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    SOLAR_OS_LOGI(TAG,
                 "COM app stopped on %s: tx=%" PRIu32 " rx=%" PRIu32,
                 com_app.bus_name,
                 com_app.tx_bytes,
                 com_app.rx_bytes);
    if (com_app.bus_leased) {
        (void)solar_os_bus_release(com_app.bus_name,
                                   SOLAR_OS_BUS_PROTOCOL_UART,
                                   COM_BUS_OWNER);
    }
    memset(&com_app, 0, sizeof(com_app));
}

static void com_resume(solar_os_context_t *ctx)
{
    (void)com_io(ctx);
    com_drain_rx(ctx);
}

static void com_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    snprintf(buffer,
             buffer_len,
             "com %s",
             com_app.bus_name[0] != '\0' ? com_app.bus_name : SOLAR_OS_UART_PORT_NAME);
}

static bool com_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        com_drain_rx(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        (void)solar_os_shell_io_newline(com_io(ctx));
        com_flush(ctx);
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (com_app.active) {
        com_send_key(ctx, ch);
        com_drain_rx(ctx);
    }
    return true;
}

const solar_os_app_t solar_os_com_app = {
    .name = "com",
    .summary = "serial terminal",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = com_start,
    .resume = com_resume,
    .stop = com_stop,
    .event = com_event,
    .title = com_title,
};
