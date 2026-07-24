#include "solar_os_telnetd_job.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_port.h"
#include "solar_os_port_shell.h"
#include "solar_os_task.h"

#define TELNETD_DEFAULT_PORT 23U
#define TELNETD_PORT_NAME "telnet0"
#define TELNETD_PASSWORD_MAX 64U
#define TELNETD_PEER_MAX 40U
#define TELNETD_TTYPE_MAX 32U
#define TELNETD_PENDING_MAX 128U
#define TELNETD_SB_MAX 40U
#define TELNETD_TASK_STACK 6144U
#define TELNETD_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define TELNETD_SELECT_TIMEOUT_MS 100U
#define TELNETD_NEGOTIATION_MS 200U
#define TELNETD_AUTH_TIMEOUT_MS 30000U
#define TELNETD_STOP_WAIT_MS 2000U
#define TELNETD_DEFAULT_COLS 80U
#define TELNETD_DEFAULT_ROWS 24U
#define TELNETD_MIN_COLS 20U
#define TELNETD_MIN_ROWS 8U
#define TELNETD_MAX_COLS 300U
#define TELNETD_MAX_ROWS 120U

#define TELNET_IAC 255U
#define TELNET_DONT 254U
#define TELNET_DO 253U
#define TELNET_WONT 252U
#define TELNET_WILL 251U
#define TELNET_SB 250U
#define TELNET_SE 240U

#define TELNET_OPT_ECHO 1U
#define TELNET_OPT_SGA 3U
#define TELNET_OPT_TTYPE 24U
#define TELNET_OPT_NAWS 31U
#define TELNET_OPT_LINEMODE 34U

#define TELNET_TTYPE_IS 0U
#define TELNET_TTYPE_SEND 1U

static const char *TAG = "solar_os_telnetd";

typedef enum {
    TELNETD_PARSE_DATA,
    TELNETD_PARSE_IAC,
    TELNETD_PARSE_DO,
    TELNETD_PARSE_DONT,
    TELNETD_PARSE_WILL,
    TELNETD_PARSE_WONT,
    TELNETD_PARSE_SB_OPTION,
    TELNETD_PARSE_SB_DATA,
    TELNETD_PARSE_SB_IAC,
} telnetd_parse_state_t;

typedef struct {
    telnetd_parse_state_t state;
    uint8_t sb_option;
    uint8_t sb_data[TELNETD_SB_MAX];
    size_t sb_len;
    bool suppress_nvt_follow;
} telnetd_parser_t;

typedef struct {
    bool running;
    bool stop_requested;
    bool port_registered;
    bool client_disconnected;
    TaskHandle_t task;
    int listen_fd;
    int client_fd;
    uint16_t port;
    uint8_t session_id;
    uint32_t job_generation;
    solar_os_context_t shell_context;
    char password[TELNETD_PASSWORD_MAX];
    char peer[TELNETD_PEER_MAX];
    char terminal_type[TELNETD_TTYPE_MAX];
    uint16_t cols;
    uint16_t rows;
    telnetd_parser_t parser;
    uint8_t pending[TELNETD_PENDING_MAX];
    size_t pending_len;
    uint32_t connection_count;
    uint32_t rejected_count;
    uint32_t auth_failure_count;
    esp_err_t last_error;
} telnetd_job_state_t;

static telnetd_job_state_t telnetd_job = {
    .listen_fd = -1,
    .client_fd = -1,
    .last_error = ESP_OK,
};
static portMUX_TYPE telnetd_lock = portMUX_INITIALIZER_UNLOCKED;

static bool telnetd_should_stop(void)
{
    portENTER_CRITICAL(&telnetd_lock);
    const bool stop = telnetd_job.stop_requested;
    portEXIT_CRITICAL(&telnetd_lock);
    return stop;
}

static int telnetd_client_fd(void)
{
    portENTER_CRITICAL(&telnetd_lock);
    const int fd = telnetd_job.client_fd;
    portEXIT_CRITICAL(&telnetd_lock);
    return fd;
}

static void telnetd_mark_disconnected(int fd)
{
    portENTER_CRITICAL(&telnetd_lock);
    if (telnetd_job.client_fd == fd) {
        telnetd_job.client_disconnected = true;
    }
    portEXIT_CRITICAL(&telnetd_lock);
}

static bool telnetd_client_is_disconnected(void)
{
    portENTER_CRITICAL(&telnetd_lock);
    const bool disconnected = telnetd_job.client_disconnected;
    portEXIT_CRITICAL(&telnetd_lock);
    return disconnected;
}

static bool telnetd_send_raw_fd(int fd, const uint8_t *data, size_t len)
{
    if (fd < 0 || (data == NULL && len > 0)) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        const ssize_t sent = send(fd, data + offset, len - offset, 0);
        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        telnetd_mark_disconnected(fd);
        return false;
    }
    return true;
}

static bool telnetd_send_command(uint8_t command, uint8_t option)
{
    const int fd = telnetd_client_fd();
    const uint8_t data[] = {TELNET_IAC, command, option};
    return telnetd_send_raw_fd(fd, data, sizeof(data));
}

static bool telnetd_send_text(const char *text)
{
    return text != NULL &&
        telnetd_send_raw_fd(telnetd_client_fd(),
                            (const uint8_t *)text,
                            strlen(text));
}

static void telnetd_request_terminal_type(void)
{
    const int fd = telnetd_client_fd();
    const uint8_t data[] = {
        TELNET_IAC,
        TELNET_SB,
        TELNET_OPT_TTYPE,
        TELNET_TTYPE_SEND,
        TELNET_IAC,
        TELNET_SE,
    };
    (void)telnetd_send_raw_fd(fd, data, sizeof(data));
}

static void telnetd_negotiate(void)
{
    (void)telnetd_send_command(TELNET_WILL, TELNET_OPT_ECHO);
    (void)telnetd_send_command(TELNET_WILL, TELNET_OPT_SGA);
    (void)telnetd_send_command(TELNET_DO, TELNET_OPT_SGA);
    (void)telnetd_send_command(TELNET_DO, TELNET_OPT_TTYPE);
    (void)telnetd_send_command(TELNET_DO, TELNET_OPT_NAWS);
    (void)telnetd_send_command(TELNET_DONT, TELNET_OPT_LINEMODE);
}

static void telnetd_update_dimensions(uint16_t cols, uint16_t rows)
{
    if (cols < TELNETD_MIN_COLS || rows < TELNETD_MIN_ROWS ||
        cols > TELNETD_MAX_COLS || rows > TELNETD_MAX_ROWS) {
        return;
    }

    uint8_t session_id = 0;
    portENTER_CRITICAL(&telnetd_lock);
    telnetd_job.cols = cols;
    telnetd_job.rows = rows;
    session_id = telnetd_job.session_id;
    portEXIT_CRITICAL(&telnetd_lock);

    if (session_id != 0) {
        (void)solar_os_port_shell_set_dimensions(session_id, cols, rows);
    }
}

static void telnetd_handle_subnegotiation(void)
{
    telnetd_parser_t *parser = &telnetd_job.parser;

    if (parser->sb_option == TELNET_OPT_NAWS && parser->sb_len >= 4U) {
        const uint16_t cols =
            (uint16_t)(((uint16_t)parser->sb_data[0] << 8) | parser->sb_data[1]);
        const uint16_t rows =
            (uint16_t)(((uint16_t)parser->sb_data[2] << 8) | parser->sb_data[3]);
        telnetd_update_dimensions(cols, rows);
    } else if (parser->sb_option == TELNET_OPT_TTYPE &&
               parser->sb_len > 1U &&
               parser->sb_data[0] == TELNET_TTYPE_IS) {
        const size_t available = parser->sb_len - 1U;
        const size_t copy_len =
            available < sizeof(telnetd_job.terminal_type) - 1U ?
                available :
                sizeof(telnetd_job.terminal_type) - 1U;
        for (size_t i = 0; i < copy_len; i++) {
            const unsigned char ch = parser->sb_data[i + 1U];
            telnetd_job.terminal_type[i] =
                isprint(ch) ? (char)tolower(ch) : '?';
        }
        telnetd_job.terminal_type[copy_len] = '\0';
    }
    parser->sb_len = 0;
}

static void telnetd_handle_do(uint8_t option)
{
    if (option != TELNET_OPT_ECHO && option != TELNET_OPT_SGA) {
        (void)telnetd_send_command(TELNET_WONT, option);
    }
}

static void telnetd_handle_will(uint8_t option)
{
    switch (option) {
    case TELNET_OPT_TTYPE:
        telnetd_request_terminal_type();
        break;
    case TELNET_OPT_SGA:
    case TELNET_OPT_NAWS:
        break;
    default:
        (void)telnetd_send_command(TELNET_DONT, option);
        break;
    }
}

static void telnetd_emit_nvt_byte(uint8_t byte,
                                  uint8_t *out,
                                  size_t out_cap,
                                  size_t *out_len)
{
    telnetd_parser_t *parser = &telnetd_job.parser;

    if (parser->suppress_nvt_follow) {
        parser->suppress_nvt_follow = false;
        if (byte == '\n' || byte == '\0') {
            return;
        }
    }
    if (byte == '\r') {
        parser->suppress_nvt_follow = true;
    } else if (byte == '\n') {
        byte = '\r';
    }
    if (*out_len < out_cap) {
        out[(*out_len)++] = byte;
    }
}

static void telnetd_feed_wire_byte(uint8_t byte,
                                   uint8_t *out,
                                   size_t out_cap,
                                   size_t *out_len)
{
    telnetd_parser_t *parser = &telnetd_job.parser;

    switch (parser->state) {
    case TELNETD_PARSE_DATA:
        if (byte == TELNET_IAC) {
            parser->state = TELNETD_PARSE_IAC;
        } else {
            telnetd_emit_nvt_byte(byte, out, out_cap, out_len);
        }
        break;
    case TELNETD_PARSE_IAC:
        switch (byte) {
        case TELNET_IAC:
            telnetd_emit_nvt_byte(TELNET_IAC, out, out_cap, out_len);
            parser->state = TELNETD_PARSE_DATA;
            break;
        case TELNET_DO:
            parser->state = TELNETD_PARSE_DO;
            break;
        case TELNET_DONT:
            parser->state = TELNETD_PARSE_DONT;
            break;
        case TELNET_WILL:
            parser->state = TELNETD_PARSE_WILL;
            break;
        case TELNET_WONT:
            parser->state = TELNETD_PARSE_WONT;
            break;
        case TELNET_SB:
            parser->sb_option = 0;
            parser->sb_len = 0;
            parser->state = TELNETD_PARSE_SB_OPTION;
            break;
        default:
            parser->state = TELNETD_PARSE_DATA;
            break;
        }
        break;
    case TELNETD_PARSE_DO:
        telnetd_handle_do(byte);
        parser->state = TELNETD_PARSE_DATA;
        break;
    case TELNETD_PARSE_DONT:
        parser->state = TELNETD_PARSE_DATA;
        break;
    case TELNETD_PARSE_WILL:
        telnetd_handle_will(byte);
        parser->state = TELNETD_PARSE_DATA;
        break;
    case TELNETD_PARSE_WONT:
        parser->state = TELNETD_PARSE_DATA;
        break;
    case TELNETD_PARSE_SB_OPTION:
        parser->sb_option = byte;
        parser->state = TELNETD_PARSE_SB_DATA;
        break;
    case TELNETD_PARSE_SB_DATA:
        if (byte == TELNET_IAC) {
            parser->state = TELNETD_PARSE_SB_IAC;
        } else if (parser->sb_len < sizeof(parser->sb_data)) {
            parser->sb_data[parser->sb_len++] = byte;
        }
        break;
    case TELNETD_PARSE_SB_IAC:
        if (byte == TELNET_SE) {
            telnetd_handle_subnegotiation();
            parser->state = TELNETD_PARSE_DATA;
        } else if (byte == TELNET_IAC) {
            if (parser->sb_len < sizeof(parser->sb_data)) {
                parser->sb_data[parser->sb_len++] = TELNET_IAC;
            }
            parser->state = TELNETD_PARSE_SB_DATA;
        } else {
            parser->state = TELNETD_PARSE_DATA;
        }
        break;
    default:
        parser->state = TELNETD_PARSE_DATA;
        break;
    }
}

static esp_err_t telnetd_socket_read(uint8_t *data,
                                    size_t len,
                                    uint32_t timeout_ms,
                                    size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (data == NULL || len == 0 || read_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int fd = telnetd_client_fd();
    if (fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
    };
    const int ready = select(fd + 1, &readfds, NULL, NULL, &timeout);
    if (ready == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (ready < 0) {
        if (errno == EINTR) {
            return ESP_ERR_TIMEOUT;
        }
        telnetd_mark_disconnected(fd);
        return ESP_FAIL;
    }

    uint8_t wire[128];
    size_t want = len < sizeof(wire) ? len : sizeof(wire);
    const ssize_t got = recv(fd, wire, want, 0);
    if (got == 0) {
        telnetd_mark_disconnected(fd);
        return ESP_ERR_INVALID_STATE;
    }
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return ESP_ERR_TIMEOUT;
        }
        telnetd_mark_disconnected(fd);
        return ESP_FAIL;
    }

    for (ssize_t i = 0; i < got; i++) {
        telnetd_feed_wire_byte(wire[i], data, len, read_len);
    }
    return *read_len > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

static size_t telnetd_pending_take(uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || telnetd_job.pending_len == 0) {
        return 0;
    }

    const size_t count = telnetd_job.pending_len < len ?
        telnetd_job.pending_len :
        len;
    memcpy(data, telnetd_job.pending, count);
    memmove(telnetd_job.pending,
            telnetd_job.pending + count,
            telnetd_job.pending_len - count);
    telnetd_job.pending_len -= count;
    return count;
}

static void telnetd_pending_append(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 ||
        telnetd_job.pending_len >= sizeof(telnetd_job.pending)) {
        return;
    }
    const size_t available = sizeof(telnetd_job.pending) - telnetd_job.pending_len;
    const size_t count = len < available ? len : available;
    memcpy(telnetd_job.pending + telnetd_job.pending_len, data, count);
    telnetd_job.pending_len += count;
}

static esp_err_t telnetd_port_read(void *user,
                                  uint8_t *data,
                                  size_t len,
                                  uint32_t timeout_ms,
                                  size_t *read_len)
{
    (void)user;
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (data == NULL || len == 0 || read_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t pending = telnetd_pending_take(data, len);
    if (pending > 0) {
        *read_len = pending;
        return ESP_OK;
    }
    return telnetd_socket_read(data, len, timeout_ms, read_len);
}

static esp_err_t telnetd_port_write(void *user,
                                   const uint8_t *data,
                                   size_t len,
                                   size_t *written)
{
    (void)user;
    if (written != NULL) {
        *written = 0;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int fd = telnetd_client_fd();
    if (fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != TELNET_IAC) {
            continue;
        }
        if (i > start && !telnetd_send_raw_fd(fd, data + start, i - start)) {
            return ESP_FAIL;
        }
        const uint8_t escaped[] = {TELNET_IAC, TELNET_IAC};
        if (!telnetd_send_raw_fd(fd, escaped, sizeof(escaped))) {
            return ESP_FAIL;
        }
        start = i + 1U;
    }
    if (start < len && !telnetd_send_raw_fd(fd, data + start, len - start)) {
        return ESP_FAIL;
    }
    if (written != NULL) {
        *written = len;
    }
    return ESP_OK;
}

static esp_err_t telnetd_port_open(void *user)
{
    (void)user;
    return telnetd_client_fd() >= 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t telnetd_port_close(void *user)
{
    (void)user;
    const int fd = telnetd_client_fd();
    if (fd >= 0) {
        telnetd_mark_disconnected(fd);
        (void)shutdown(fd, SHUT_RDWR);
    }
    return ESP_OK;
}

static bool telnetd_register_port(void)
{
    const solar_os_port_driver_t driver = {
        .name = TELNETD_PORT_NAME,
        .label = "Telnet client connection",
        .capabilities = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE,
        .read = telnetd_port_read,
        .write = telnetd_port_write,
        .open = telnetd_port_open,
        .close = telnetd_port_close,
        .user = &telnetd_job,
    };
    const esp_err_t err = solar_os_port_register(&driver);
    if (err != ESP_OK) {
        telnetd_job.last_error = err;
        SOLAR_OS_LOGW(TAG, "register %s failed: %s", TELNETD_PORT_NAME, esp_err_to_name(err));
        return false;
    }
    portENTER_CRITICAL(&telnetd_lock);
    telnetd_job.port_registered = true;
    portEXIT_CRITICAL(&telnetd_lock);
    return true;
}

static solar_os_shell_terminal_profile_t telnetd_terminal_profile(void)
{
    if (strcmp(telnetd_job.terminal_type, "dumb") == 0) {
        return SOLAR_OS_SHELL_TERMINAL_PROFILE_DUMB;
    }
    if (strcmp(telnetd_job.terminal_type, "ansi") == 0) {
        return SOLAR_OS_SHELL_TERMINAL_PROFILE_ANSI;
    }
    return SOLAR_OS_SHELL_TERMINAL_PROFILE_VT100;
}

static bool telnetd_prime_negotiation(void)
{
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(TELNETD_NEGOTIATION_MS);
    while (!telnetd_should_stop() &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t data[64];
        size_t read_len = 0;
        const esp_err_t err =
            telnetd_socket_read(data, sizeof(data), 25U, &read_len);
        if (err == ESP_OK) {
            telnetd_pending_append(data, read_len);
        } else if (err != ESP_ERR_TIMEOUT) {
            return false;
        }
    }
    return !telnetd_client_is_disconnected();
}

static bool telnetd_authenticate(void)
{
    if (telnetd_job.password[0] == '\0') {
        return true;
    }

    if (!telnetd_send_text("Password: ")) {
        return false;
    }

    char entered[TELNETD_PASSWORD_MAX];
    size_t entered_len = 0;
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(TELNETD_AUTH_TIMEOUT_MS);

    while (!telnetd_should_stop() &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        uint8_t data[64];
        size_t read_len = telnetd_pending_take(data, sizeof(data));
        esp_err_t err = ESP_OK;
        if (read_len == 0) {
            err = telnetd_socket_read(data, sizeof(data), 100U, &read_len);
        }
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            return false;
        }

        for (size_t i = 0; i < read_len; i++) {
            const uint8_t ch = data[i];
            if (ch == '\r' || ch == '\n') {
                entered[entered_len] = '\0';
                const bool accepted = strcmp(entered, telnetd_job.password) == 0;
                memset(entered, 0, sizeof(entered));
                if (i + 1U < read_len) {
                    telnetd_pending_append(data + i + 1U, read_len - i - 1U);
                }
                if (accepted) {
                    (void)telnetd_send_text("\r\n");
                    return true;
                }
                telnetd_job.auth_failure_count++;
                (void)telnetd_send_text("\r\nAuthentication failed.\r\n");
                return false;
            }
            if (ch == '\b' || ch == 0x7fU) {
                if (entered_len > 0) {
                    entered[--entered_len] = '\0';
                }
            } else if (isprint(ch) && entered_len + 1U < sizeof(entered)) {
                entered[entered_len++] = (char)ch;
            }
        }
    }

    memset(entered, 0, sizeof(entered));
    (void)telnetd_send_text("\r\nAuthentication timed out.\r\n");
    return false;
}

static bool telnetd_start_shell(void)
{
    if (!telnetd_register_port()) {
        return false;
    }

    const solar_os_port_shell_options_t options = {
        .terminal_profile = telnetd_terminal_profile(),
        .cols = telnetd_job.cols,
        .rows = telnetd_job.rows,
    };
    uint8_t session_id = 0;
    const esp_err_t err =
        solar_os_port_shell_start_with_options(&telnetd_job.shell_context,
                                               TELNETD_PORT_NAME,
                                               &options,
                                               false,
                                               &session_id);
    if (err != ESP_OK) {
        telnetd_job.last_error = err;
        SOLAR_OS_LOGW(TAG, "shell start failed: %s", esp_err_to_name(err));
        (void)solar_os_port_unregister(TELNETD_PORT_NAME);
        portENTER_CRITICAL(&telnetd_lock);
        telnetd_job.port_registered = false;
        portEXIT_CRITICAL(&telnetd_lock);
        return false;
    }

    portENTER_CRITICAL(&telnetd_lock);
    telnetd_job.session_id = session_id;
    portEXIT_CRITICAL(&telnetd_lock);
    SOLAR_OS_LOGI(TAG,
                  "client %s attached as session %u (%s %ux%u)",
                  telnetd_job.peer,
                  (unsigned)session_id,
                  telnetd_job.terminal_type[0] != '\0' ?
                      telnetd_job.terminal_type :
                      "vt100",
                  (unsigned)telnetd_job.cols,
                  (unsigned)telnetd_job.rows);
    return true;
}

static bool telnetd_cleanup_client(void)
{
    uint8_t session_id = 0;
    int fd = -1;
    bool port_registered = false;

    portENTER_CRITICAL(&telnetd_lock);
    session_id = telnetd_job.session_id;
    fd = telnetd_job.client_fd;
    port_registered = telnetd_job.port_registered;
    portEXIT_CRITICAL(&telnetd_lock);

    if (fd >= 0) {
        (void)shutdown(fd, SHUT_RDWR);
    }
    if (session_id != 0 && solar_os_port_shell_is_session_id(session_id)) {
        (void)solar_os_port_shell_stop(session_id);
        for (uint32_t i = 0;
             i < TELNETD_STOP_WAIT_MS / 25U &&
             solar_os_port_shell_is_session_id(session_id);
             i++) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        if (solar_os_port_shell_is_session_id(session_id)) {
            return false;
        }
    }
    if (port_registered) {
        const esp_err_t err = solar_os_port_unregister(TELNETD_PORT_NAME);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            telnetd_job.last_error = err;
            SOLAR_OS_LOGW(TAG, "unregister %s failed: %s", TELNETD_PORT_NAME, esp_err_to_name(err));
            return false;
        }
    }
    if (fd >= 0) {
        close(fd);
    }

    portENTER_CRITICAL(&telnetd_lock);
    telnetd_job.client_fd = -1;
    telnetd_job.client_disconnected = false;
    telnetd_job.port_registered = false;
    telnetd_job.session_id = 0;
    telnetd_job.peer[0] = '\0';
    portEXIT_CRITICAL(&telnetd_lock);
    memset(&telnetd_job.parser, 0, sizeof(telnetd_job.parser));
    telnetd_job.pending_len = 0;
    telnetd_job.terminal_type[0] = '\0';
    return true;
}

static void telnetd_reject_busy(int fd)
{
    static const char busy[] =
        "SolarOS Telnet is busy; only one remote shell is available.\r\n";
    (void)telnetd_send_raw_fd(fd, (const uint8_t *)busy, sizeof(busy) - 1U);
    (void)shutdown(fd, SHUT_RDWR);
    close(fd);
    telnetd_job.rejected_count++;
}

static bool telnetd_accept_one(bool busy)
{
    int listen_fd = -1;
    portENTER_CRITICAL(&telnetd_lock);
    listen_fd = telnetd_job.listen_fd;
    portEXIT_CRITICAL(&telnetd_lock);
    if (listen_fd < 0) {
        return false;
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(listen_fd, &readfds);
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = TELNETD_SELECT_TIMEOUT_MS * 1000U,
    };
    const int ready = select(listen_fd + 1, &readfds, NULL, NULL, &timeout);
    if (ready == 0) {
        return false;
    }
    if (ready < 0) {
        if (errno != EINTR && !telnetd_should_stop()) {
            telnetd_job.last_error = ESP_FAIL;
            SOLAR_OS_LOGW(TAG, "listener select failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        return false;
    }

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    const int fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
        return false;
    }
    if (busy) {
        telnetd_reject_busy(fd);
        return true;
    }

    portENTER_CRITICAL(&telnetd_lock);
    telnetd_job.client_fd = fd;
    telnetd_job.client_disconnected = false;
    telnetd_job.session_id = 0;
    snprintf(telnetd_job.peer,
             sizeof(telnetd_job.peer),
             "%s:%u",
             inet_ntoa(addr.sin_addr),
             (unsigned)ntohs(addr.sin_port));
    portEXIT_CRITICAL(&telnetd_lock);

    telnetd_job.connection_count++;
    telnetd_job.cols = TELNETD_DEFAULT_COLS;
    telnetd_job.rows = TELNETD_DEFAULT_ROWS;
    telnetd_job.pending_len = 0;
    telnetd_job.terminal_type[0] = '\0';
    memset(&telnetd_job.parser, 0, sizeof(telnetd_job.parser));

    const char *banner = telnetd_job.password[0] != '\0' ?
        "SolarOS Telnet (unencrypted)\r\n" :
        "SolarOS Telnet (unencrypted, no authentication)\r\n";
    telnetd_negotiate();
    if (!telnetd_send_text(banner) ||
        !telnetd_prime_negotiation() ||
        !telnetd_authenticate() ||
        !telnetd_start_shell()) {
        while (!telnetd_cleanup_client() && !telnetd_should_stop()) {
            vTaskDelay(pdMS_TO_TICKS(25));
        }
    }
    return true;
}

static void telnetd_job_task(void *arg)
{
    telnetd_job_state_t *state = (telnetd_job_state_t *)arg;

    SOLAR_OS_LOGI(TAG,
                  "started on port %u password=%s",
                  (unsigned)state->port,
                  state->password[0] != '\0' ? "yes" : "no");

    while (!telnetd_should_stop()) {
        uint8_t session_id = 0;
        bool disconnected = false;
        int client_fd = -1;
        portENTER_CRITICAL(&telnetd_lock);
        session_id = state->session_id;
        disconnected = state->client_disconnected;
        client_fd = state->client_fd;
        portEXIT_CRITICAL(&telnetd_lock);

        if (client_fd < 0) {
            (void)telnetd_accept_one(false);
            continue;
        }
        if (disconnected ||
            (session_id != 0 && !solar_os_port_shell_is_session_id(session_id))) {
            if (!telnetd_cleanup_client()) {
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            continue;
        }
        (void)telnetd_accept_one(true);
    }

    while (!telnetd_cleanup_client()) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    int listen_fd = -1;
    portENTER_CRITICAL(&telnetd_lock);
    listen_fd = state->listen_fd;
    state->listen_fd = -1;
    portEXIT_CRITICAL(&telnetd_lock);
    if (listen_fd >= 0) {
        (void)shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
    }

    SOLAR_OS_LOGI(TAG,
                  "stopped: connections=%" PRIu32 " rejected=%" PRIu32
                  " auth-failures=%" PRIu32,
                  state->connection_count,
                  state->rejected_count,
                  state->auth_failure_count);

    const uint32_t generation = state->job_generation;
    const esp_err_t last_error = state->last_error;
    portENTER_CRITICAL(&telnetd_lock);
    state->running = false;
    state->stop_requested = false;
    state->task = NULL;
    memset(state->password, 0, sizeof(state->password));
    portEXIT_CRITICAL(&telnetd_lock);
    (void)solar_os_jobs_mark_stopped(solar_os_telnetd_job.name,
                                     generation,
                                     last_error);
    solar_os_task_delete_internal(NULL);
}

static bool telnetd_parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }
    uint32_t value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (!isdigit(*p)) {
            return false;
        }
        value = (value * 10U) + (uint32_t)(*p - '0');
        if (value > UINT16_MAX) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool telnetd_password_valid(const char *password)
{
    if (password == NULL ||
        password[0] == '\0' ||
        strlen(password) >= TELNETD_PASSWORD_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)password; *p != '\0'; p++) {
        if (!isprint(*p)) {
            return false;
        }
    }
    return true;
}

static esp_err_t telnetd_parse_args(int argc,
                                    char **argv,
                                    uint16_t *port,
                                    const char **password)
{
    if (argc < 0 || argc > 4 || port == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *port = TELNETD_DEFAULT_PORT;
    *password = NULL;
    bool port_set = false;
    int first_arg = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_telnetd_job.name) == 0) {
        first_arg = 1;
    }

    for (int i = first_arg; i < argc; i++) {
        if (argv == NULL || argv[i] == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(argv[i], "--password") == 0) {
            if (++i >= argc || *password != NULL ||
                !telnetd_password_valid(argv[i])) {
                return ESP_ERR_INVALID_ARG;
            }
            *password = argv[i];
            continue;
        }
        uint16_t parsed_port = 0;
        if (!port_set && telnetd_parse_port(argv[i], &parsed_port)) {
            *port = parsed_port;
            port_set = true;
            continue;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t telnetd_open_listener(uint16_t port, int *listen_fd)
{
    if (listen_fd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *listen_fd = -1;

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return ESP_FAIL;
    }
    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return ESP_FAIL;
    }
    *listen_fd = fd;
    return ESP_OK;
}

static esp_err_t telnetd_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    uint16_t port = TELNETD_DEFAULT_PORT;
    const char *password = NULL;
    esp_err_t err = telnetd_parse_args(argc, argv, &port, &password);
    if (err != ESP_OK || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&telnetd_lock);
    const bool busy = telnetd_job.running || telnetd_job.task != NULL;
    portEXIT_CRITICAL(&telnetd_lock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    int listen_fd = -1;
    err = telnetd_open_listener(port, &listen_fd);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t generation = 0;
    err = solar_os_jobs_get_generation(solar_os_telnetd_job.name, &generation);
    if (err != ESP_OK) {
        close(listen_fd);
        return err;
    }

    solar_os_context_init(&telnetd_job.shell_context,
                          solar_os_context_terminal(ctx),
                          solar_os_context_gfx(ctx));
    solar_os_context_copy_session_handlers(&telnetd_job.shell_context, ctx);

    portENTER_CRITICAL(&telnetd_lock);
    telnetd_job.running = true;
    telnetd_job.stop_requested = false;
    telnetd_job.port_registered = false;
    telnetd_job.client_disconnected = false;
    telnetd_job.listen_fd = listen_fd;
    telnetd_job.client_fd = -1;
    telnetd_job.port = port;
    telnetd_job.session_id = 0;
    telnetd_job.job_generation = generation;
    telnetd_job.password[0] = '\0';
    if (password != NULL) {
        strlcpy(telnetd_job.password, password, sizeof(telnetd_job.password));
    }
    telnetd_job.connection_count = 0;
    telnetd_job.rejected_count = 0;
    telnetd_job.auth_failure_count = 0;
    telnetd_job.last_error = ESP_OK;
    portEXIT_CRITICAL(&telnetd_lock);

    TaskHandle_t task = NULL;
    if (solar_os_task_create_pinned_internal(telnetd_job_task,
                                             "telnetd_job",
                                             TELNETD_TASK_STACK,
                                             &telnetd_job,
                                             TELNETD_TASK_PRIORITY,
                                             &task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        close(listen_fd);
        portENTER_CRITICAL(&telnetd_lock);
        telnetd_job.running = false;
        telnetd_job.listen_fd = -1;
        memset(telnetd_job.password, 0, sizeof(telnetd_job.password));
        portEXIT_CRITICAL(&telnetd_lock);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&telnetd_lock);
    telnetd_job.task = task;
    portEXIT_CRITICAL(&telnetd_lock);

    char resource[16];
    snprintf(resource, sizeof(resource), "tcp:%u", (unsigned)port);
    (void)solar_os_jobs_note_resource(solar_os_telnetd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      resource,
                                      "listen");
    return ESP_OK;
}

static void telnetd_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    int listen_fd = -1;
    int client_fd = -1;
    TaskHandle_t task = NULL;
    portENTER_CRITICAL(&telnetd_lock);
    if (!telnetd_job.running && telnetd_job.task == NULL) {
        portEXIT_CRITICAL(&telnetd_lock);
        return;
    }
    telnetd_job.stop_requested = true;
    listen_fd = telnetd_job.listen_fd;
    client_fd = telnetd_job.client_fd;
    task = telnetd_job.task;
    portEXIT_CRITICAL(&telnetd_lock);

    if (listen_fd >= 0) {
        (void)shutdown(listen_fd, SHUT_RDWR);
    }
    if (client_fd >= 0) {
        (void)shutdown(client_fd, SHUT_RDWR);
    }

    for (uint32_t i = 0; i < TELNETD_STOP_WAIT_MS / 25U; i++) {
        portENTER_CRITICAL(&telnetd_lock);
        task = telnetd_job.task;
        portEXIT_CRITICAL(&telnetd_lock);
        if (task == NULL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

const solar_os_job_t solar_os_telnetd_job = {
    .name = "telnetd",
    .summary = "remote Telnet shell server",
    .start = telnetd_job_start,
    .stop = telnetd_job_stop,
    .event = NULL,
    .worker_stack_bytes = TELNETD_TASK_STACK,
};
