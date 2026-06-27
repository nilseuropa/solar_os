#include "solar_os_ssh_app.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_ble_keyboard.h"
#include "solar_os_identity.h"
#include "solar_os_shell_io.h"
#include "solar_os_ssh.h"
#include "solar_os_ssh_keys.h"
#include "solar_os_terminal.h"

#define SSH_DEFAULT_PORT 22
#define SSH_PASSWORD_PROMPT_MAX SOLAR_OS_SSH_PASSWORD_MAX

typedef enum {
    SSH_APP_PASSWORD,
    SSH_APP_CONNECTING,
    SSH_APP_CONNECTED,
    SSH_APP_ERROR,
} ssh_app_mode_t;

typedef enum {
    SSH_ANSI_NORMAL,
    SSH_ANSI_ESC,
    SSH_ANSI_CSI,
    SSH_ANSI_OSC,
    SSH_ANSI_OSC_ESC,
    SSH_ANSI_CHARSET_G0,
    SSH_ANSI_CHARSET_G1,
    SSH_ANSI_SKIP_ONE,
} ssh_ansi_state_t;

typedef enum {
    SSH_CHARSET_ASCII,
    SSH_CHARSET_DEC_SPECIAL,
} ssh_charset_t;

typedef struct {
    int params[8];
    int param_count;
    bool have_value;
    bool private_question;
    char private_prefix;
} ssh_csi_state_t;

typedef struct {
    solar_os_ssh_session_t *session;
    ssh_app_mode_t mode;
    ssh_ansi_state_t ansi_state;
    ssh_csi_state_t csi;
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char host[SOLAR_OS_SSH_HOST_MAX];
    char password[SSH_PASSWORD_PROMPT_MAX];
    size_t password_len;
    uint16_t port;
    bool cursor_application_mode;
    bool alt_prefix_pending;
    uint8_t active_charset;
    ssh_charset_t charset[2];
    size_t saved_row;
    size_t saved_col;
    bool saved_cursor_valid;
    bool saw_error;
    bool suspended;
} ssh_app_state_t;

static ssh_app_state_t ssh_app;
static solar_os_shell_io_t ssh_fallback_io;

static solar_os_shell_io_t *ssh_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&ssh_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &ssh_fallback_io);
        io = &ssh_fallback_io;
    }
    return io;
}

static solar_os_terminal_t *ssh_terminal(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = ssh_io(ctx);
    solar_os_terminal_t *term = solar_os_shell_io_terminal(io);
    return term != NULL ? term : solar_os_context_terminal(ctx);
}

static void ssh_flush(solar_os_context_t *ctx)
{
    if (!ssh_app.suspended) {
        solar_os_shell_io_flush(ssh_io(ctx));
    }
}

static bool ssh_is_printable(char ch)
{
    const unsigned char value = (unsigned char)ch;

    return isprint(value) || value >= 0xa0;
}

static void ssh_render_usage(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = ssh_io(ctx);

    solar_os_shell_io_clear(io);
    solar_os_shell_io_write_bold(io, "ssh");
    solar_os_shell_io_newline(io);
    solar_os_shell_io_writeln(io, "usage: ssh [user@]host [port]");
    solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
    ssh_flush(ctx);
}

static bool ssh_parse_port(const char *text, uint16_t *port)
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

static bool ssh_parse_target(const char *target)
{
    if (target == NULL || target[0] == '\0') {
        return false;
    }

    char work[SOLAR_OS_SSH_USERNAME_MAX + SOLAR_OS_SSH_HOST_MAX + 8];
    strlcpy(work, target, sizeof(work));

    char *host = work;
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char *at = strchr(work, '@');
    if (at != NULL) {
        if (at == work || at[1] == '\0') {
            return false;
        }
        *at = '\0';
        host = at + 1;
        strlcpy(username, work, sizeof(username));
    } else {
        solar_os_identity_get_user(username, sizeof(username));
    }

    char *colon = strrchr(host, ':');
    if (colon != NULL && strchr(colon + 1, ':') == NULL && colon[1] != '\0') {
        uint16_t parsed_port = 0;
        if (!ssh_parse_port(colon + 1, &parsed_port)) {
            return false;
        }
        *colon = '\0';
        ssh_app.port = parsed_port;
    }

    if (host[0] == '\0') {
        return false;
    }

    strlcpy(ssh_app.username, username, sizeof(ssh_app.username));
    strlcpy(ssh_app.host, host, sizeof(ssh_app.host));
    return ssh_app.username[0] != '\0' && ssh_app.host[0] != '\0';
}

static void ssh_render_password_prompt(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = ssh_io(ctx);

    solar_os_shell_io_clear(io);
    solar_os_shell_io_printf_bold(io,
                                  "ssh %s@%s:%u\n",
                                  ssh_app.username,
                                  ssh_app.host,
                                  (unsigned)ssh_app.port);
    solar_os_shell_io_write(io,
                            solar_os_ssh_keys_default_exists() ?
                                "password (Enter for key): " :
                                "password: ");
    for (size_t i = 0; i < ssh_app.password_len; i++) {
        solar_os_shell_io_put_char(io, '*');
    }
    ssh_flush(ctx);
}

static esp_err_t ssh_begin_connect(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = ssh_io(ctx);
    solar_os_ssh_config_t config = {
        .host = ssh_app.host,
        .port = ssh_app.port,
        .username = ssh_app.username,
        .password = ssh_app.password,
        .cols = solar_os_shell_io_cols(io),
        .rows = solar_os_shell_io_rows(io),
    };

    solar_os_shell_io_clear(io);
    solar_os_shell_io_printf_bold(io,
                                  "ssh %s@%s:%u\n",
                                  ssh_app.username,
                                  ssh_app.host,
                                  (unsigned)ssh_app.port);
    solar_os_shell_io_writeln(io, "starting");
    ssh_flush(ctx);

    const esp_err_t err = solar_os_ssh_start(&config, &ssh_app.session);
    memset(ssh_app.password, 0, sizeof(ssh_app.password));
    ssh_app.password_len = 0;
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "ssh start failed: %s\n", esp_err_to_name(err));
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        ssh_flush(ctx);
        ssh_app.mode = SSH_APP_ERROR;
        return err;
    }

    ssh_app.mode = SSH_APP_CONNECTING;
    return ESP_OK;
}

static void ssh_reset_csi(void)
{
    memset(&ssh_app.csi, 0, sizeof(ssh_app.csi));
}

static void ssh_reset_terminal_modes(solar_os_terminal_t *term)
{
    solar_os_terminal_set_bold(term, false);
    solar_os_terminal_set_inverse(term, false);
    solar_os_terminal_set_cursor_visible(term, true);
    solar_os_terminal_utf8_reset(term);
    ssh_app.active_charset = 0;
    ssh_app.charset[0] = SSH_CHARSET_ASCII;
    ssh_app.charset[1] = SSH_CHARSET_ASCII;
    ssh_app.cursor_application_mode = false;
}

static int ssh_csi_param(size_t index, int fallback)
{
    if (index >= (size_t)ssh_app.csi.param_count) {
        return fallback;
    }

    const int value = ssh_app.csi.params[index];
    return value > 0 ? value : fallback;
}

static bool ssh_csi_has_param(int value)
{
    for (int i = 0; i < ssh_app.csi.param_count; i++) {
        if (ssh_app.csi.params[i] == value) {
            return true;
        }
    }
    return false;
}

static void ssh_save_cursor(solar_os_terminal_t *term)
{
    ssh_app.saved_row = solar_os_terminal_cursor_row(term);
    ssh_app.saved_col = solar_os_terminal_cursor_col(term);
    ssh_app.saved_cursor_valid = true;
}

static void ssh_restore_cursor(solar_os_terminal_t *term)
{
    if (ssh_app.saved_cursor_valid) {
        solar_os_terminal_set_cursor(term, ssh_app.saved_row, ssh_app.saved_col);
    }
}

static void ssh_clear_line(solar_os_terminal_t *term, size_t row)
{
    const size_t saved_row = solar_os_terminal_cursor_row(term);
    const size_t saved_col = solar_os_terminal_cursor_col(term);

    solar_os_terminal_set_cursor(term, row, 0);
    solar_os_terminal_clear_line_from(term, row, 0);
    solar_os_terminal_set_cursor(term, saved_row, saved_col);
}

static void ssh_clear_screen_from(solar_os_terminal_t *term, size_t row, size_t col)
{
    const size_t rows = solar_os_terminal_rows(term);

    if (row >= rows) {
        return;
    }
    solar_os_terminal_clear_line_from(term, row, col);
    for (size_t i = row + 1; i < rows; i++) {
        ssh_clear_line(term, i);
    }
}

static void ssh_send_terminal_response(const char *response)
{
    if (ssh_app.session != NULL && response != NULL) {
        (void)solar_os_ssh_send(ssh_app.session, response, strlen(response));
    }
}

static void ssh_send_cursor_position_response(solar_os_terminal_t *term, bool private_response)
{
    if (ssh_app.session == NULL) {
        return;
    }

    char response[32];
    const unsigned row = (unsigned)solar_os_terminal_cursor_row(term) + 1U;
    const unsigned col = (unsigned)solar_os_terminal_cursor_col(term) + 1U;
    const int response_len = private_response ?
        snprintf(response, sizeof(response), "\x1b[?%u;%uR", row, col) :
        snprintf(response, sizeof(response), "\x1b[%u;%uR", row, col);
    if (response_len > 0 && response_len < (int)sizeof(response)) {
        (void)solar_os_ssh_send(ssh_app.session, response, (size_t)response_len);
    }
}

static void ssh_handle_sgr(solar_os_terminal_t *term)
{
    if (ssh_app.csi.param_count == 0) {
        solar_os_terminal_set_bold(term, false);
        solar_os_terminal_set_inverse(term, false);
        return;
    }

    for (int i = 0; i < ssh_app.csi.param_count; i++) {
        switch (ssh_app.csi.params[i]) {
        case 0:
            solar_os_terminal_set_bold(term, false);
            solar_os_terminal_set_inverse(term, false);
            break;
        case 1:
            solar_os_terminal_set_bold(term, true);
            break;
        case 2:
        case 4:
        case 5:
            break;
        case 7:
            solar_os_terminal_set_inverse(term, true);
            break;
        case 22:
            solar_os_terminal_set_bold(term, false);
            break;
        case 24:
        case 25:
            break;
        case 27:
            solar_os_terminal_set_inverse(term, false);
            break;
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
        case 38:
        case 39:
            break;
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
        case 48:
            solar_os_terminal_set_inverse(term, true);
            break;
        case 49:
            solar_os_terminal_set_inverse(term, false);
            break;
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            solar_os_terminal_set_bold(term, true);
            break;
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            solar_os_terminal_set_inverse(term, true);
            break;
        default:
            break;
        }
    }
}

static void ssh_handle_csi(solar_os_terminal_t *term, char final)
{
    const int count = ssh_csi_param(0, 1);
    size_t row = solar_os_terminal_cursor_row(term);
    size_t col = solar_os_terminal_cursor_col(term);

    switch (final) {
    case 'A':
        solar_os_terminal_set_cursor(term, row > (size_t)count ? row - (size_t)count : 0, col);
        break;
    case 'B':
        row += (size_t)count;
        if (row >= solar_os_terminal_rows(term)) {
            row = solar_os_terminal_rows(term) - 1;
        }
        solar_os_terminal_set_cursor(term, row, col);
        break;
    case 'C':
        col += (size_t)count;
        if (col >= solar_os_terminal_cols(term)) {
            col = solar_os_terminal_cols(term) - 1;
        }
        solar_os_terminal_set_cursor(term, row, col);
        break;
    case 'D':
        solar_os_terminal_set_cursor(term, row, col > (size_t)count ? col - (size_t)count : 0);
        break;
    case 'E':
        row += (size_t)count;
        if (row >= solar_os_terminal_rows(term)) {
            row = solar_os_terminal_rows(term) - 1;
        }
        solar_os_terminal_set_cursor(term, row, 0);
        break;
    case 'F':
        solar_os_terminal_set_cursor(term, row > (size_t)count ? row - (size_t)count : 0, 0);
        break;
    case 'G':
        col = (size_t)ssh_csi_param(0, 1);
        solar_os_terminal_set_cursor(term, row, col > 0 ? col - 1 : 0);
        break;
    case 'H':
    case 'f':
        row = (size_t)ssh_csi_param(0, 1);
        col = (size_t)ssh_csi_param(1, 1);
        solar_os_terminal_set_cursor(term, row > 0 ? row - 1 : 0, col > 0 ? col - 1 : 0);
        break;
    case 'J':
        switch (ssh_app.csi.params[0]) {
        case 0:
            ssh_clear_screen_from(term, row, col);
            break;
        case 1:
        case 2:
        case 3:
        default:
            solar_os_terminal_clear(term);
            break;
        }
        break;
    case 'K':
        switch (ssh_app.csi.params[0]) {
        case 0:
            solar_os_terminal_clear_line_from(term, row, col);
            break;
        case 2:
            ssh_clear_line(term, row);
            break;
        case 1:
        default:
            break;
        }
        break;
    case 'X': {
        const size_t saved_row = row;
        const size_t saved_col = col;
        const size_t cols = solar_os_terminal_cols(term);
        const size_t erase_count = (size_t)count;
        const size_t max_count = col < cols ? cols - col : 0;
        const size_t limit = erase_count < max_count ? erase_count : max_count;
        for (size_t i = 0; i < limit; i++) {
            solar_os_terminal_put_codepoint(term, ' ');
        }
        solar_os_terminal_set_cursor(term, saved_row, saved_col);
        break;
    }
    case 'm':
        ssh_handle_sgr(term);
        break;
    case 'c':
        if (ssh_app.csi.private_prefix == '>') {
            ssh_send_terminal_response("\x1b[>0;0;0c");
        } else {
            ssh_send_terminal_response("\x1b[?1;2c");
        }
        break;
    case 'd':
        row = (size_t)ssh_csi_param(0, 1);
        solar_os_terminal_set_cursor(term, row > 0 ? row - 1 : 0, col);
        break;
    case 's':
        ssh_save_cursor(term);
        break;
    case 'u':
        ssh_restore_cursor(term);
        break;
    case 'n':
        if (!ssh_app.csi.private_question && ssh_csi_param(0, 0) == 5) {
            ssh_send_terminal_response("\x1b[0n");
        } else if (ssh_app.csi.private_question && ssh_csi_param(0, 0) == 6) {
            ssh_send_cursor_position_response(term, true);
        } else if (!ssh_app.csi.private_question && ssh_csi_param(0, 0) == 6) {
            ssh_send_cursor_position_response(term, false);
        }
        break;
    case 'h':
        if (ssh_app.csi.private_question) {
            if (ssh_csi_has_param(1)) {
                ssh_app.cursor_application_mode = true;
            }
            if (ssh_csi_has_param(25)) {
                solar_os_terminal_set_cursor_visible(term, true);
            }
            if (ssh_csi_has_param(47) || ssh_csi_has_param(1047) || ssh_csi_has_param(1049)) {
                ssh_save_cursor(term);
                solar_os_terminal_clear(term);
            }
        }
        break;
    case 'l':
        if (ssh_app.csi.private_question) {
            if (ssh_csi_has_param(1)) {
                ssh_app.cursor_application_mode = false;
            }
            if (ssh_csi_has_param(25)) {
                solar_os_terminal_set_cursor_visible(term, false);
            }
            if (ssh_csi_has_param(47) || ssh_csi_has_param(1047) || ssh_csi_has_param(1049)) {
                solar_os_terminal_clear(term);
                ssh_restore_cursor(term);
            }
        }
        break;
    default:
        break;
    }
}

static uint32_t ssh_dec_special_codepoint(char ch)
{
    switch (ch) {
    case '`':
        return 0x25c6;
    case 'a':
        return 0x2592;
    case 'f':
        return 0x00b0;
    case 'g':
        return 0x00b1;
    case 'h':
        return 0x2424;
    case 'i':
        return 0x000b;
    case 'j':
        return 0x2518;
    case 'k':
        return 0x2510;
    case 'l':
        return 0x250c;
    case 'm':
        return 0x2514;
    case 'n':
        return 0x253c;
    case 'o':
        return 0x23ba;
    case 'p':
        return 0x23bb;
    case 'q':
        return 0x2500;
    case 'r':
        return 0x23bc;
    case 's':
        return 0x23bd;
    case 't':
        return 0x251c;
    case 'u':
        return 0x2524;
    case 'v':
        return 0x2534;
    case 'w':
        return 0x252c;
    case 'x':
        return 0x2502;
    case 'y':
        return 0x2264;
    case 'z':
        return 0x2265;
    case '{':
        return 0x03c0;
    case '|':
        return 0x2260;
    case '}':
        return 0x00a3;
    case '~':
        return 0x00b7;
    default:
        return (unsigned char)ch;
    }
}

static void ssh_put_text_char(solar_os_terminal_t *term, char ch)
{
    if (ssh_app.charset[ssh_app.active_charset] == SSH_CHARSET_DEC_SPECIAL) {
        const uint32_t codepoint = ssh_dec_special_codepoint(ch);
        if (codepoint >= 0x20) {
            solar_os_terminal_put_codepoint(term, codepoint);
        }
        return;
    }

    solar_os_terminal_put_utf8_byte(term, (uint8_t)ch);
}

static void ssh_feed_output_char(solar_os_terminal_t *term, char ch)
{
    switch (ssh_app.ansi_state) {
    case SSH_ANSI_NORMAL:
        if (ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_terminal_utf8_reset(term);
            ssh_app.ansi_state = SSH_ANSI_ESC;
        } else if ((unsigned char)ch == 0x0e) {
            solar_os_terminal_utf8_reset(term);
            ssh_app.active_charset = 1;
        } else if ((unsigned char)ch == 0x0f) {
            solar_os_terminal_utf8_reset(term);
            ssh_app.active_charset = 0;
        } else if ((unsigned char)ch == 0x7f) {
            solar_os_terminal_backspace(term);
        } else if ((unsigned char)ch < 0x20) {
            solar_os_terminal_utf8_reset(term);
            solar_os_terminal_put_char(term, ch);
        } else {
            ssh_put_text_char(term, ch);
        }
        break;
    case SSH_ANSI_ESC:
        if (ch == '[') {
            ssh_reset_csi();
            ssh_app.ansi_state = SSH_ANSI_CSI;
        } else if (ch == ']') {
            ssh_app.ansi_state = SSH_ANSI_OSC;
        } else if (ch == '(') {
            ssh_app.ansi_state = SSH_ANSI_CHARSET_G0;
        } else if (ch == ')') {
            ssh_app.ansi_state = SSH_ANSI_CHARSET_G1;
        } else if (ch == '%' || ch == '#') {
            ssh_app.ansi_state = SSH_ANSI_SKIP_ONE;
        } else if (ch == '7') {
            ssh_save_cursor(term);
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        } else if (ch == '8') {
            ssh_restore_cursor(term);
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        } else if (ch == 'D') {
            solar_os_terminal_newline(term);
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        } else if (ch == 'E') {
            solar_os_terminal_newline(term);
            solar_os_terminal_set_cursor(term, solar_os_terminal_cursor_row(term), 0);
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        } else if (ch == 'c') {
            solar_os_terminal_clear(term);
            ssh_reset_terminal_modes(term);
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        } else if (ch == '=' || ch == '>') {
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        } else {
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        }
        break;
    case SSH_ANSI_CHARSET_G0:
        ssh_app.charset[0] = ch == '0' ? SSH_CHARSET_DEC_SPECIAL : SSH_CHARSET_ASCII;
        ssh_app.ansi_state = SSH_ANSI_NORMAL;
        break;
    case SSH_ANSI_CHARSET_G1:
        ssh_app.charset[1] = ch == '0' ? SSH_CHARSET_DEC_SPECIAL : SSH_CHARSET_ASCII;
        ssh_app.ansi_state = SSH_ANSI_NORMAL;
        break;
    case SSH_ANSI_CSI:
        if (isdigit((unsigned char)ch)) {
            if (ssh_app.csi.param_count == 0) {
                ssh_app.csi.param_count = 1;
            }
            const size_t index = (size_t)ssh_app.csi.param_count - 1;
            if (index < sizeof(ssh_app.csi.params) / sizeof(ssh_app.csi.params[0])) {
                ssh_app.csi.params[index] = (ssh_app.csi.params[index] * 10) + (ch - '0');
                ssh_app.csi.have_value = true;
            }
        } else if (ch == ';' || ch == ':') {
            if (ssh_app.csi.param_count == 0) {
                ssh_app.csi.param_count = 1;
            }
            if ((size_t)ssh_app.csi.param_count < sizeof(ssh_app.csi.params) / sizeof(ssh_app.csi.params[0])) {
                ssh_app.csi.param_count++;
            }
        } else if (ch == '?' && ssh_app.csi.param_count == 0 && !ssh_app.csi.have_value) {
            ssh_app.csi.private_question = true;
            ssh_app.csi.private_prefix = '?';
        } else if (ch == '>' && ssh_app.csi.param_count == 0 && !ssh_app.csi.have_value) {
            ssh_app.csi.private_prefix = '>';
        } else if (ch >= '@' && ch <= '~') {
            if (ssh_app.csi.param_count == 0) {
                ssh_app.csi.param_count = 1;
            }
            ssh_handle_csi(term, ch);
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        }
        break;
    case SSH_ANSI_OSC:
        if (ch == '\a') {
            ssh_app.ansi_state = SSH_ANSI_NORMAL;
        } else if (ch == SOLAR_OS_KEY_ESCAPE) {
            ssh_app.ansi_state = SSH_ANSI_OSC_ESC;
        }
        break;
    case SSH_ANSI_OSC_ESC:
        ssh_app.ansi_state = ch == '\\' ? SSH_ANSI_NORMAL : SSH_ANSI_OSC;
        break;
    case SSH_ANSI_SKIP_ONE:
        ssh_app.ansi_state = SSH_ANSI_NORMAL;
        break;
    default:
        ssh_app.ansi_state = SSH_ANSI_NORMAL;
        break;
    }
}

static void ssh_write_output(solar_os_context_t *ctx, const char *data, size_t len)
{
    solar_os_shell_io_t *io = ssh_io(ctx);
    if (solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        solar_os_shell_io_write_raw(io, data, len);
        ssh_flush(ctx);
        return;
    }

    solar_os_terminal_t *term = ssh_terminal(ctx);

    for (size_t i = 0; i < len; i++) {
        ssh_feed_output_char(term, data[i]);
    }
    ssh_flush(ctx);
}

static void ssh_drain_events(solar_os_context_t *ctx)
{
    if (ssh_app.session == NULL) {
        return;
    }

    solar_os_shell_io_t *io = ssh_io(ctx);
    solar_os_ssh_event_t event;

    while (ssh_app.session != NULL && solar_os_ssh_poll(ssh_app.session, &event)) {
        switch (event.type) {
        case SOLAR_OS_SSH_EVENT_STATUS:
            solar_os_shell_io_printf(io, "ssh: %s\n", event.message);
            ssh_flush(ctx);
            break;
        case SOLAR_OS_SSH_EVENT_CONNECTED:
            ssh_app.mode = SSH_APP_CONNECTED;
            solar_os_shell_io_printf(io, "ssh: %s\n", event.message);
            ssh_flush(ctx);
            break;
        case SOLAR_OS_SSH_EVENT_OUTPUT:
            ssh_write_output(ctx, event.data, event.len);
            break;
        case SOLAR_OS_SSH_EVENT_ERROR:
            ssh_app.saw_error = true;
            solar_os_shell_io_printf(io, "ssh: %s\n", event.message);
            ssh_flush(ctx);
            break;
        case SOLAR_OS_SSH_EVENT_DISCONNECTED:
            solar_os_shell_io_printf(io, "ssh: %s\n", event.message);
            ssh_flush(ctx);
            solar_os_ssh_stop(ssh_app.session);
            ssh_app.session = NULL;
            if (ssh_app.saw_error) {
                ssh_app.mode = SSH_APP_ERROR;
                solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
                ssh_flush(ctx);
            } else {
                solar_os_context_request_exit(ctx);
            }
            break;
        default:
            break;
        }
    }
}

static void ssh_send_key(char ch)
{
    if (ssh_app.session == NULL) {
        return;
    }

    const char *seq = NULL;
    char data[2] = {0};
    size_t len = 0;

    switch ((uint8_t)ch) {
    case SOLAR_OS_KEY_ALT_PREFIX:
        ssh_app.alt_prefix_pending = true;
        return;
    case SOLAR_OS_KEY_UP:
        seq = ssh_app.cursor_application_mode ? "\x1bOA" : "\x1b[A";
        break;
    case SOLAR_OS_KEY_DOWN:
        seq = ssh_app.cursor_application_mode ? "\x1bOB" : "\x1b[B";
        break;
    case SOLAR_OS_KEY_RIGHT:
        seq = ssh_app.cursor_application_mode ? "\x1bOC" : "\x1b[C";
        break;
    case SOLAR_OS_KEY_LEFT:
        seq = ssh_app.cursor_application_mode ? "\x1bOD" : "\x1b[D";
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
        if ((uint8_t)ch >= 0x80) {
            return;
        }
        data[0] = ch;
        len = 1;
        break;
    }

    if (ssh_app.alt_prefix_pending) {
        ssh_app.alt_prefix_pending = false;
        (void)solar_os_ssh_send(ssh_app.session, "\x1b", 1);
    }
    if (seq != NULL) {
        (void)solar_os_ssh_send(ssh_app.session, seq, strlen(seq));
    } else if (len > 0) {
        (void)solar_os_ssh_send(ssh_app.session, data, len);
    }
}

static esp_err_t ssh_start(solar_os_context_t *ctx)
{
    memset(&ssh_app, 0, sizeof(ssh_app));
    ssh_app.port = SSH_DEFAULT_PORT;

    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || argc > 3 || !ssh_parse_target(solar_os_context_argv(ctx, 1))) {
        ssh_app.mode = SSH_APP_ERROR;
        ssh_render_usage(ctx);
        return ESP_OK;
    }

    if (argc == 3 && !ssh_parse_port(solar_os_context_argv(ctx, 2), &ssh_app.port)) {
        ssh_app.mode = SSH_APP_ERROR;
        ssh_render_usage(ctx);
        return ESP_OK;
    }

    ssh_app.mode = SSH_APP_PASSWORD;
    ssh_render_password_prompt(ctx);
    return ESP_OK;
}

static void ssh_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (ssh_app.session != NULL) {
        solar_os_ssh_stop(ssh_app.session);
    }
    memset(&ssh_app, 0, sizeof(ssh_app));
}

static void ssh_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    ssh_app.suspended = true;
}

static void ssh_resume(solar_os_context_t *ctx)
{
    ssh_app.suspended = false;
    solar_os_context_set_shell_io(ctx, NULL);
    (void)ssh_io(ctx);
    ssh_flush(ctx);
    ssh_drain_events(ctx);
}

static void ssh_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;

    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (ssh_app.username[0] != '\0' && ssh_app.host[0] != '\0') {
        snprintf(buffer,
                 buffer_len,
                 "ssh %s@%s",
                 ssh_app.username,
                 ssh_app.host);
        return;
    }
    strlcpy(buffer, "ssh", buffer_len);
}

static bool ssh_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        ssh_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        if (ssh_app.session != NULL) {
            solar_os_shell_io_writeln(ssh_io(ctx), "\nssh: closing");
            ssh_flush(ctx);
            solar_os_ssh_stop(ssh_app.session);
            ssh_app.session = NULL;
        }
        solar_os_context_request_exit(ctx);
        return true;
    }

    switch (ssh_app.mode) {
    case SSH_APP_PASSWORD:
        if (ch == '\b') {
            if (ssh_app.password_len > 0) {
                ssh_app.password_len--;
                ssh_app.password[ssh_app.password_len] = '\0';
                ssh_render_password_prompt(ctx);
            }
        } else if (ch == '\r' || ch == '\n') {
            (void)ssh_begin_connect(ctx);
        } else if (ssh_is_printable(ch) && ssh_app.password_len + 1 < sizeof(ssh_app.password)) {
            ssh_app.password[ssh_app.password_len++] = ch;
            ssh_app.password[ssh_app.password_len] = '\0';
            ssh_render_password_prompt(ctx);
        }
        break;
    case SSH_APP_CONNECTING:
    case SSH_APP_CONNECTED:
        ssh_send_key(ch);
        ssh_drain_events(ctx);
        break;
    case SSH_APP_ERROR:
        break;
    default:
        break;
    }

    return true;
}

const solar_os_app_t solar_os_ssh_app = {
    .name = "ssh",
    .summary = "SSH client",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = ssh_start,
    .suspend = ssh_suspend,
    .resume = ssh_resume,
    .stop = ssh_stop,
    .event = ssh_event,
    .title = ssh_title,
};
