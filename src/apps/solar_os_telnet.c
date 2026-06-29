#include "solar_os_telnet.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "esp_err.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_net.h"
#include "solar_os_shell_io.h"
#include "solar_os_terminal.h"

#define TELNET_DEFAULT_PORT 23U
#define TELNET_CONNECT_TIMEOUT_MS 10000U
#define TELNET_RX_BUFFER_SIZE 256U
#define TELNET_RX_CHUNKS_PER_TICK 12U
#define TELNET_TERMINAL_TYPE "xterm-mono"

#define TELNET_IAC 255U
#define TELNET_DONT 254U
#define TELNET_DO 253U
#define TELNET_WONT 252U
#define TELNET_WILL 251U
#define TELNET_SB 250U
#define TELNET_SE 240U

#define TELNET_OPT_BINARY 0U
#define TELNET_OPT_ECHO 1U
#define TELNET_OPT_SGA 3U
#define TELNET_OPT_TTYPE 24U
#define TELNET_OPT_NAWS 31U
#define TELNET_OPT_LINEMODE 34U

#define TELNET_TTYPE_IS 0U
#define TELNET_TTYPE_SEND 1U

static const char *TAG = "solar_os_telnet";

typedef enum {
    TELNET_MODE_CONNECTING,
    TELNET_MODE_CONNECTED,
    TELNET_MODE_ERROR,
} telnet_mode_t;

typedef enum {
    TELNET_PARSE_DATA,
    TELNET_PARSE_IAC,
    TELNET_PARSE_DO,
    TELNET_PARSE_DONT,
    TELNET_PARSE_WILL,
    TELNET_PARSE_WONT,
    TELNET_PARSE_SB_OPTION,
    TELNET_PARSE_SB_DATA,
    TELNET_PARSE_SB_IAC,
} telnet_parse_state_t;

typedef enum {
    TELNET_ANSI_NORMAL,
    TELNET_ANSI_ESC,
    TELNET_ANSI_CSI,
    TELNET_ANSI_OSC,
    TELNET_ANSI_OSC_ESC,
} telnet_ansi_state_t;

typedef struct {
    int params[8];
    int count;
    bool have_value;
    bool private_question;
} telnet_csi_state_t;

typedef struct {
    int fd;
    telnet_mode_t mode;
    telnet_parse_state_t parse_state;
    telnet_ansi_state_t ansi_state;
    telnet_csi_state_t csi;
    solar_os_shell_io_t fallback_io;
    char host[SOLAR_OS_NET_HOST_MAX];
    char resolved_ip[SOLAR_OS_NET_ADDR_MAX];
    uint16_t port;
    bool raw_mode;
    bool active;
    bool suspended;
    bool alt_prefix_pending;
    bool local_echo;
    uint8_t sb_option;
    uint8_t sb_data[32];
    size_t sb_len;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} telnet_app_state_t;

static telnet_app_state_t telnet_app;

static solar_os_shell_io_t *telnet_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&telnet_app.fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &telnet_app.fallback_io);
        io = &telnet_app.fallback_io;
    }
    return io;
}

static void telnet_flush(solar_os_context_t *ctx)
{
    if (!telnet_app.suspended) {
        (void)solar_os_shell_io_flush(telnet_io(ctx));
    }
}

static void telnet_close_socket(void)
{
    if (telnet_app.fd >= 0) {
        shutdown(telnet_app.fd, SHUT_RDWR);
        close(telnet_app.fd);
        telnet_app.fd = -1;
    }
}

static bool telnet_is_private_display_session(solar_os_context_t *ctx)
{
    return ctx != NULL && solar_os_context_shell_session(ctx) == NULL;
}

static void telnet_request_close_with_status(solar_os_context_t *ctx, const char *status)
{
    if (status != NULL && status[0] != '\0') {
        SOLAR_OS_LOGW(TAG, "%s", status);
        if (telnet_is_private_display_session(ctx)) {
            solar_os_context_set_status_message(ctx, status);
        }
    }
    solar_os_context_request_terminal_preserve(ctx);
    solar_os_context_request_exit(ctx);
}

static void telnet_render_usage(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = telnet_io(ctx);
    solar_os_shell_io_clear(io);
    solar_os_shell_io_write_bold(io, "telnet");
    solar_os_shell_io_newline(io);
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  telnet host [port]");
    solar_os_shell_io_writeln(io, "  telnet -r host [port]");
    solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
    telnet_flush(ctx);
}

static bool telnet_parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool telnet_parse_args(solar_os_context_t *ctx)
{
    telnet_app.port = TELNET_DEFAULT_PORT;
    telnet_app.raw_mode = false;

    const int argc = solar_os_context_argc(ctx);
    int argi = 1;
    if (argc > argi && strcmp(solar_os_context_argv(ctx, argi), "-r") == 0) {
        telnet_app.raw_mode = true;
        argi++;
    }
    if (argc <= argi || argc > argi + 2) {
        return false;
    }

    const char *host = solar_os_context_argv(ctx, argi++);
    if (host == NULL || host[0] == '\0') {
        return false;
    }
    strlcpy(telnet_app.host, host, sizeof(telnet_app.host));

    if (argc > argi && !telnet_parse_port(solar_os_context_argv(ctx, argi), &telnet_app.port)) {
        return false;
    }
    return true;
}

static esp_err_t telnet_set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return ESP_FAIL;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t telnet_connect_socket(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = telnet_io(ctx);
    esp_err_t err =
        solar_os_net_resolve_host(telnet_app.host, telnet_app.resolved_ip, sizeof(telnet_app.resolved_ip));
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "telnet: resolve failed: %s\n", esp_err_to_name(err));
        return err;
    }

    telnet_app.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (telnet_app.fd < 0) {
        solar_os_shell_io_printf(io, "telnet: socket failed errno=%d\n", errno);
        return ESP_FAIL;
    }

    err = telnet_set_nonblocking(telnet_app.fd);
    if (err != ESP_OK) {
        solar_os_shell_io_writeln(io, "telnet: nonblocking setup failed");
        telnet_close_socket();
        return err;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(telnet_app.port);
    if (inet_pton(AF_INET, telnet_app.resolved_ip, &addr.sin_addr) != 1) {
        telnet_close_socket();
        return ESP_ERR_INVALID_ARG;
    }

    int rc = connect(telnet_app.fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS && errno != EALREADY) {
        solar_os_shell_io_printf(io, "telnet: connect failed errno=%d\n", errno);
        telnet_close_socket();
        return ESP_FAIL;
    }
    if (rc == 0) {
        return ESP_OK;
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(telnet_app.fd, &writefds);
    struct timeval timeout = {
        .tv_sec = TELNET_CONNECT_TIMEOUT_MS / 1000U,
        .tv_usec = (TELNET_CONNECT_TIMEOUT_MS % 1000U) * 1000U,
    };
    rc = select(telnet_app.fd + 1, NULL, &writefds, NULL, &timeout);
    if (rc <= 0) {
        solar_os_shell_io_writeln(io, rc == 0 ? "telnet: connect timeout" : "telnet: connect select failed");
        telnet_close_socket();
        return rc == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    if (getsockopt(telnet_app.fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) != 0 ||
        so_error != 0) {
        solar_os_shell_io_printf(io, "telnet: connect failed errno=%d\n", so_error);
        telnet_close_socket();
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t telnet_send_raw(const uint8_t *data, size_t len)
{
    if (telnet_app.fd < 0 || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t offset = 0;
    while (offset < len) {
        const ssize_t written = send(telnet_app.fd, data + offset, len - offset, 0);
        if (written > 0) {
            offset += (size_t)written;
            telnet_app.tx_bytes += (uint32_t)written;
            continue;
        }
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void telnet_send_iac3(uint8_t command, uint8_t option)
{
    const uint8_t data[] = {TELNET_IAC, command, option};
    (void)telnet_send_raw(data, sizeof(data));
}

static void telnet_send_window_size(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = telnet_io(ctx);
    const uint16_t cols = solar_os_shell_io_cols(io) != 0 ? solar_os_shell_io_cols(io) : 80U;
    const uint16_t rows = solar_os_shell_io_rows(io) != 0 ? solar_os_shell_io_rows(io) : 24U;
    uint8_t data[] = {
        TELNET_IAC,
        TELNET_SB,
        TELNET_OPT_NAWS,
        (uint8_t)(cols >> 8),
        (uint8_t)(cols & 0xffU),
        (uint8_t)(rows >> 8),
        (uint8_t)(rows & 0xffU),
        TELNET_IAC,
        TELNET_SE,
    };
    (void)telnet_send_raw(data, sizeof(data));
}

static void telnet_send_ttype(void)
{
    uint8_t data[sizeof(TELNET_TERMINAL_TYPE) + 6U];
    size_t pos = 0;
    data[pos++] = TELNET_IAC;
    data[pos++] = TELNET_SB;
    data[pos++] = TELNET_OPT_TTYPE;
    data[pos++] = TELNET_TTYPE_IS;
    memcpy(data + pos, TELNET_TERMINAL_TYPE, strlen(TELNET_TERMINAL_TYPE));
    pos += strlen(TELNET_TERMINAL_TYPE);
    data[pos++] = TELNET_IAC;
    data[pos++] = TELNET_SE;
    (void)telnet_send_raw(data, pos);
}

static void telnet_handle_do(solar_os_context_t *ctx, uint8_t option)
{
    switch (option) {
    case TELNET_OPT_TTYPE:
    case TELNET_OPT_SGA:
        telnet_send_iac3(TELNET_WILL, option);
        break;
    case TELNET_OPT_NAWS:
        telnet_send_iac3(TELNET_WILL, option);
        telnet_send_window_size(ctx);
        break;
    default:
        telnet_send_iac3(TELNET_WONT, option);
        break;
    }
}

static void telnet_handle_will(uint8_t option)
{
    switch (option) {
    case TELNET_OPT_ECHO:
        telnet_app.local_echo = false;
        telnet_send_iac3(TELNET_DO, option);
        break;
    case TELNET_OPT_SGA:
        telnet_send_iac3(TELNET_DO, option);
        break;
    default:
        telnet_send_iac3(TELNET_DONT, option);
        break;
    }
}

static int telnet_csi_param(size_t index, int fallback)
{
    if (index >= (size_t)telnet_app.csi.count) {
        return fallback;
    }
    return telnet_app.csi.params[index] > 0 ? telnet_app.csi.params[index] : fallback;
}

static void telnet_reset_csi(void)
{
    memset(&telnet_app.csi, 0, sizeof(telnet_app.csi));
}

static void telnet_clear_screen_from(solar_os_shell_io_t *io, size_t row, size_t col)
{
    const size_t rows = solar_os_shell_io_rows(io);
    if (row >= rows) {
        return;
    }
    (void)solar_os_shell_io_clear_line_from(io, row, col);
    for (size_t i = row + 1U; i < rows; i++) {
        (void)solar_os_shell_io_clear_line_from(io, i, 0);
    }
}

static void telnet_handle_sgr(solar_os_shell_io_t *io)
{
    if (telnet_app.csi.count == 0) {
        (void)solar_os_shell_io_set_bold(io, false);
        (void)solar_os_shell_io_set_italic(io, false);
        (void)solar_os_shell_io_set_underline(io, false);
        (void)solar_os_shell_io_set_inverse(io, false);
        return;
    }

    for (int i = 0; i < telnet_app.csi.count; i++) {
        switch (telnet_app.csi.params[i]) {
        case 0:
            (void)solar_os_shell_io_set_bold(io, false);
            (void)solar_os_shell_io_set_italic(io, false);
            (void)solar_os_shell_io_set_underline(io, false);
            (void)solar_os_shell_io_set_inverse(io, false);
            break;
        case 1:
            (void)solar_os_shell_io_set_bold(io, true);
            break;
        case 3:
            (void)solar_os_shell_io_set_italic(io, true);
            break;
        case 4:
            (void)solar_os_shell_io_set_underline(io, true);
            break;
        case 7:
            (void)solar_os_shell_io_set_inverse(io, true);
            break;
        case 22:
            (void)solar_os_shell_io_set_bold(io, false);
            break;
        case 23:
            (void)solar_os_shell_io_set_italic(io, false);
            break;
        case 24:
            (void)solar_os_shell_io_set_underline(io, false);
            break;
        case 27:
            (void)solar_os_shell_io_set_inverse(io, false);
            break;
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            (void)solar_os_shell_io_set_inverse(io, true);
            break;
        case 49:
            (void)solar_os_shell_io_set_inverse(io, false);
            break;
        default:
            break;
        }
    }
}

static void telnet_handle_csi(solar_os_context_t *ctx, char final)
{
    solar_os_shell_io_t *io = telnet_io(ctx);
    const int count = telnet_csi_param(0, 1);
    size_t row = solar_os_shell_io_cursor_row(io);
    size_t col = solar_os_shell_io_cursor_col(io);
    const size_t rows = solar_os_shell_io_rows(io);
    const size_t cols = solar_os_shell_io_cols(io);

    switch (final) {
    case 'A':
        row = row > (size_t)count ? row - (size_t)count : 0;
        (void)solar_os_shell_io_set_cursor(io, row, col);
        break;
    case 'B':
        row += (size_t)count;
        if (rows > 0 && row >= rows) {
            row = rows - 1U;
        }
        (void)solar_os_shell_io_set_cursor(io, row, col);
        break;
    case 'C':
        col += (size_t)count;
        if (cols > 0 && col >= cols) {
            col = cols - 1U;
        }
        (void)solar_os_shell_io_set_cursor(io, row, col);
        break;
    case 'D':
        col = col > (size_t)count ? col - (size_t)count : 0;
        (void)solar_os_shell_io_set_cursor(io, row, col);
        break;
    case 'G':
        col = (size_t)telnet_csi_param(0, 1);
        (void)solar_os_shell_io_set_cursor(io, row, col > 0 ? col - 1U : 0);
        break;
    case 'H':
    case 'f':
        row = (size_t)telnet_csi_param(0, 1);
        col = (size_t)telnet_csi_param(1, 1);
        (void)solar_os_shell_io_set_cursor(io, row > 0 ? row - 1U : 0, col > 0 ? col - 1U : 0);
        break;
    case 'J':
        if (telnet_app.csi.params[0] == 0) {
            telnet_clear_screen_from(io, row, col);
        } else {
            (void)solar_os_shell_io_clear(io);
        }
        break;
    case 'K':
        (void)solar_os_shell_io_clear_line_from(io,
                                               row,
                                               telnet_app.csi.params[0] == 2 ? 0U : col);
        break;
    case 'm':
        telnet_handle_sgr(io);
        break;
    case 'n':
        if (!telnet_app.csi.private_question && telnet_csi_param(0, 0) == 6) {
            char response[32];
            const int len = snprintf(response,
                                     sizeof(response),
                                     "\x1b[%u;%uR",
                                     (unsigned)row + 1U,
                                     (unsigned)col + 1U);
            if (len > 0 && len < (int)sizeof(response)) {
                (void)telnet_send_raw((const uint8_t *)response, (size_t)len);
            }
        }
        break;
    default:
        break;
    }
}

static void telnet_output_byte(solar_os_context_t *ctx, uint8_t byte)
{
    solar_os_shell_io_t *io = telnet_io(ctx);
    if (solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        (void)solar_os_shell_io_write_raw(io, (const char *)&byte, 1);
        return;
    }

    const char ch = (char)byte;
    switch (telnet_app.ansi_state) {
    case TELNET_ANSI_NORMAL:
        if (byte == SOLAR_OS_KEY_ESCAPE) {
            telnet_app.ansi_state = TELNET_ANSI_ESC;
        } else if (byte < 0x20U || byte == 0x7fU) {
            (void)solar_os_shell_io_put_char(io, ch);
        } else {
            (void)solar_os_shell_io_put_utf8_byte(io, byte);
        }
        break;
    case TELNET_ANSI_ESC:
        if (ch == '[') {
            telnet_reset_csi();
            telnet_app.ansi_state = TELNET_ANSI_CSI;
        } else if (ch == ']') {
            telnet_app.ansi_state = TELNET_ANSI_OSC;
        } else if (ch == 'c') {
            (void)solar_os_shell_io_clear(io);
            telnet_app.ansi_state = TELNET_ANSI_NORMAL;
        } else {
            telnet_app.ansi_state = TELNET_ANSI_NORMAL;
        }
        break;
    case TELNET_ANSI_CSI:
        if (isdigit((unsigned char)ch)) {
            if (telnet_app.csi.count == 0) {
                telnet_app.csi.count = 1;
            }
            const size_t index = (size_t)telnet_app.csi.count - 1U;
            if (index < sizeof(telnet_app.csi.params) / sizeof(telnet_app.csi.params[0])) {
                telnet_app.csi.params[index] = (telnet_app.csi.params[index] * 10) + (ch - '0');
                telnet_app.csi.have_value = true;
            }
        } else if (ch == ';' || ch == ':') {
            if (telnet_app.csi.count == 0) {
                telnet_app.csi.count = 1;
            }
            if ((size_t)telnet_app.csi.count < sizeof(telnet_app.csi.params) / sizeof(telnet_app.csi.params[0])) {
                telnet_app.csi.count++;
            }
        } else if (ch == '?' && telnet_app.csi.count == 0 && !telnet_app.csi.have_value) {
            telnet_app.csi.private_question = true;
        } else if (ch >= '@' && ch <= '~') {
            if (telnet_app.csi.count == 0) {
                telnet_app.csi.count = 1;
            }
            telnet_handle_csi(ctx, ch);
            telnet_app.ansi_state = TELNET_ANSI_NORMAL;
        }
        break;
    case TELNET_ANSI_OSC:
        if (ch == '\a') {
            telnet_app.ansi_state = TELNET_ANSI_NORMAL;
        } else if (byte == SOLAR_OS_KEY_ESCAPE) {
            telnet_app.ansi_state = TELNET_ANSI_OSC_ESC;
        }
        break;
    case TELNET_ANSI_OSC_ESC:
        telnet_app.ansi_state = ch == '\\' ? TELNET_ANSI_NORMAL : TELNET_ANSI_OSC;
        break;
    default:
        telnet_app.ansi_state = TELNET_ANSI_NORMAL;
        break;
    }
}

static void telnet_handle_subnegotiation(void)
{
    if (telnet_app.sb_option == TELNET_OPT_TTYPE &&
        telnet_app.sb_len > 0 &&
        telnet_app.sb_data[0] == TELNET_TTYPE_SEND) {
        telnet_send_ttype();
    }
    telnet_app.sb_len = 0;
}

static void telnet_feed_byte(solar_os_context_t *ctx, uint8_t byte)
{
    if (telnet_app.raw_mode) {
        telnet_output_byte(ctx, byte);
        return;
    }

    switch (telnet_app.parse_state) {
    case TELNET_PARSE_DATA:
        if (byte == TELNET_IAC) {
            telnet_app.parse_state = TELNET_PARSE_IAC;
        } else {
            telnet_output_byte(ctx, byte);
        }
        break;
    case TELNET_PARSE_IAC:
        switch (byte) {
        case TELNET_IAC:
            telnet_output_byte(ctx, TELNET_IAC);
            telnet_app.parse_state = TELNET_PARSE_DATA;
            break;
        case TELNET_DO:
            telnet_app.parse_state = TELNET_PARSE_DO;
            break;
        case TELNET_DONT:
            telnet_app.parse_state = TELNET_PARSE_DONT;
            break;
        case TELNET_WILL:
            telnet_app.parse_state = TELNET_PARSE_WILL;
            break;
        case TELNET_WONT:
            telnet_app.parse_state = TELNET_PARSE_WONT;
            break;
        case TELNET_SB:
            telnet_app.sb_option = 0;
            telnet_app.sb_len = 0;
            telnet_app.parse_state = TELNET_PARSE_SB_OPTION;
            break;
        default:
            telnet_app.parse_state = TELNET_PARSE_DATA;
            break;
        }
        break;
    case TELNET_PARSE_DO:
        telnet_handle_do(ctx, byte);
        telnet_app.parse_state = TELNET_PARSE_DATA;
        break;
    case TELNET_PARSE_DONT:
        telnet_send_iac3(TELNET_WONT, byte);
        telnet_app.parse_state = TELNET_PARSE_DATA;
        break;
    case TELNET_PARSE_WILL:
        telnet_handle_will(byte);
        telnet_app.parse_state = TELNET_PARSE_DATA;
        break;
    case TELNET_PARSE_WONT:
        if (byte == TELNET_OPT_ECHO) {
            telnet_app.local_echo = true;
        }
        telnet_send_iac3(TELNET_DONT, byte);
        telnet_app.parse_state = TELNET_PARSE_DATA;
        break;
    case TELNET_PARSE_SB_OPTION:
        telnet_app.sb_option = byte;
        telnet_app.parse_state = TELNET_PARSE_SB_DATA;
        break;
    case TELNET_PARSE_SB_DATA:
        if (byte == TELNET_IAC) {
            telnet_app.parse_state = TELNET_PARSE_SB_IAC;
        } else if (telnet_app.sb_len < sizeof(telnet_app.sb_data)) {
            telnet_app.sb_data[telnet_app.sb_len++] = byte;
        }
        break;
    case TELNET_PARSE_SB_IAC:
        if (byte == TELNET_SE) {
            telnet_handle_subnegotiation();
            telnet_app.parse_state = TELNET_PARSE_DATA;
        } else if (byte == TELNET_IAC) {
            if (telnet_app.sb_len < sizeof(telnet_app.sb_data)) {
                telnet_app.sb_data[telnet_app.sb_len++] = TELNET_IAC;
            }
            telnet_app.parse_state = TELNET_PARSE_SB_DATA;
        } else {
            telnet_app.parse_state = TELNET_PARSE_DATA;
        }
        break;
    default:
        telnet_app.parse_state = TELNET_PARSE_DATA;
        break;
    }
}

static void telnet_drain_rx(solar_os_context_t *ctx)
{
    if (!telnet_app.active || telnet_app.fd < 0) {
        return;
    }

    uint8_t buffer[TELNET_RX_BUFFER_SIZE];
    bool received_data = false;
    for (size_t chunk = 0; chunk < TELNET_RX_CHUNKS_PER_TICK; chunk++) {
        const ssize_t got = recv(telnet_app.fd, buffer, sizeof(buffer), 0);
        if (got > 0) {
            received_data = true;
            telnet_app.rx_bytes += (uint32_t)got;
            for (ssize_t i = 0; i < got; i++) {
                telnet_feed_byte(ctx, buffer[i]);
            }
            continue;
        }
        if (got == 0) {
            const char *status = "telnet: disconnected";
            solar_os_shell_io_printf(telnet_io(ctx), "\n%s\n", status);
            telnet_flush(ctx);
            telnet_app.active = false;
            telnet_close_socket();
            telnet_request_close_with_status(ctx, status);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        char status[80];
        snprintf(status, sizeof(status), "telnet: read failed errno=%d", errno);
        solar_os_shell_io_printf(telnet_io(ctx), "\n%s\n", status);
        telnet_flush(ctx);
        telnet_app.active = false;
        telnet_close_socket();
        telnet_request_close_with_status(ctx, status);
        return;
    }
    if (received_data) {
        telnet_flush(ctx);
    }
}

static void telnet_send_bytes_escaped(const uint8_t *data, size_t len)
{
    if (telnet_app.raw_mode) {
        (void)telnet_send_raw(data, len);
        return;
    }

    for (size_t i = 0; i < len; i++) {
        if (data[i] == TELNET_IAC) {
            const uint8_t escaped[] = {TELNET_IAC, TELNET_IAC};
            (void)telnet_send_raw(escaped, sizeof(escaped));
        } else {
            (void)telnet_send_raw(&data[i], 1);
        }
    }
}

static void telnet_send_key(char ch)
{
    const uint8_t key = (uint8_t)ch;
    const char *seq = NULL;
    uint8_t data[2] = {0};
    size_t len = 0;

    switch (key) {
    case SOLAR_OS_KEY_ALT_PREFIX:
        telnet_app.alt_prefix_pending = true;
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
        if (telnet_app.raw_mode) {
            data[0] = '\r';
            len = 1;
        } else {
            data[0] = '\r';
            data[1] = '\n';
            len = 2;
        }
        break;
    default:
        if (key >= 0x80U) {
            return;
        }
        data[0] = key;
        len = 1;
        break;
    }

    if (telnet_app.alt_prefix_pending) {
        telnet_app.alt_prefix_pending = false;
        const uint8_t escape = 0x1b;
        telnet_send_bytes_escaped(&escape, 1);
    }
    if (seq != NULL) {
        telnet_send_bytes_escaped((const uint8_t *)seq, strlen(seq));
    } else if (len > 0) {
        telnet_send_bytes_escaped(data, len);
    }
}

static esp_err_t telnet_start(solar_os_context_t *ctx)
{
    memset(&telnet_app, 0, sizeof(telnet_app));
    telnet_app.fd = -1;
    telnet_app.local_echo = true;

    if (!telnet_parse_args(ctx)) {
        telnet_app.mode = TELNET_MODE_ERROR;
        telnet_render_usage(ctx);
        return ESP_OK;
    }

    solar_os_shell_io_t *io = telnet_io(ctx);
    solar_os_shell_io_clear(io);
    solar_os_shell_io_printf_bold(io,
                                  "telnet %s:%u%s\n",
                                  telnet_app.host,
                                  (unsigned)telnet_app.port,
                                  telnet_app.raw_mode ? " raw" : "");
    solar_os_shell_io_writeln(io, "connecting");
    telnet_flush(ctx);

    const esp_err_t err = telnet_connect_socket(ctx);
    if (err != ESP_OK) {
        char status[80];
        snprintf(status, sizeof(status), "telnet: start failed: %s", esp_err_to_name(err));
        solar_os_shell_io_printf(io, "%s\n", status);
        telnet_app.mode = TELNET_MODE_ERROR;
        telnet_flush(ctx);
        telnet_request_close_with_status(ctx, status);
        return ESP_OK;
    }

    telnet_app.mode = TELNET_MODE_CONNECTED;
    telnet_app.active = true;
    solar_os_shell_io_printf(io, "connected to %s\n", telnet_app.resolved_ip);
    solar_os_shell_io_printf(io, "%s exits\n\n", solar_os_shell_io_app_exit_key(io));
    telnet_flush(ctx);
    return ESP_OK;
}

static void telnet_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    telnet_close_socket();
    memset(&telnet_app, 0, sizeof(telnet_app));
    telnet_app.fd = -1;
}

static void telnet_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    telnet_app.suspended = true;
}

static void telnet_resume(solar_os_context_t *ctx)
{
    telnet_app.suspended = false;
    solar_os_context_set_shell_io(ctx, NULL);
    (void)telnet_io(ctx);
    telnet_drain_rx(ctx);
}

static void telnet_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (telnet_app.host[0] != '\0') {
        snprintf(buffer, buffer_len, "telnet %s", telnet_app.host);
    } else {
        strlcpy(buffer, "telnet", buffer_len);
    }
}

static bool telnet_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        telnet_drain_rx(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_shell_io_writeln(telnet_io(ctx), "\ntelnet: closing");
        telnet_flush(ctx);
        telnet_close_socket();
        solar_os_context_request_exit(ctx);
        return true;
    }

    if (telnet_app.active) {
        telnet_send_key(ch);
        telnet_drain_rx(ctx);
    }
    return true;
}

const solar_os_app_t solar_os_telnet_app = {
    .name = "telnet",
    .summary = "Telnet client",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = telnet_start,
    .suspend = telnet_suspend,
    .resume = telnet_resume,
    .stop = telnet_stop,
    .event = telnet_event,
    .title = telnet_title,
};
