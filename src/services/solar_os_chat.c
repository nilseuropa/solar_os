#include "solar_os_chat.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_log.h"
#include "solar_os_task.h"

#define CHAT_NVS_NAMESPACE "chat"
#define CHAT_NVS_URL_KEY "url"
#define CHAT_NVS_TOKEN_KEY "token"
#define CHAT_NVS_USER_KEY "user"
#define CHAT_NVS_DEVICE_KEY "device"

#define CHAT_DEFAULT_USER "user"
#define CHAT_DEFAULT_DEVICE "sol"
#define CHAT_DEFAULT_TCP_PORT 7777
#define CHAT_DEFAULT_TLS_PORT 7778
#define CHAT_CONNECT_TIMEOUT_MS 10000
#define CHAT_IO_TIMEOUT_MS 250
#define CHAT_TASK_STACK 8192
#define CHAT_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define CHAT_EVENT_QUEUE_LEN 12
#define CHAT_TX_QUEUE_LEN 8
#define CHAT_TEXT_ESC_MAX (SOLAR_OS_CHAT_TEXT_MAX * 6U + 1U)
#define CHAT_TX_LINE_MAX (CHAT_TEXT_ESC_MAX + SOLAR_OS_CHAT_CHANNEL_MAX * 2U + 64U)
#define CHAT_LINE_MAX (CHAT_TX_LINE_MAX + SOLAR_OS_CHAT_USER_MAX * 2U + 256U)
#define CHAT_HOST_MAX 128
#define CHAT_HELLO_LINE_MAX \
    (SOLAR_OS_CHAT_TOKEN_MAX * 2U + SOLAR_OS_CHAT_USER_MAX * 2U + \
     SOLAR_OS_CHAT_DEVICE_MAX * 2U + 80U)

typedef struct {
    bool tls;
    uint16_t port;
    char host[CHAT_HOST_MAX];
} chat_endpoint_t;

typedef struct {
    bool initialized;
    bool configured;
    bool running;
    bool connected;
    volatile bool stop_requested;
    volatile bool task_done;
    char url[SOLAR_OS_CHAT_URL_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
    char last_error[SOLAR_OS_CHAT_ERROR_MAX];
    esp_err_t last_esp_error;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t dropped_count;
    TaskHandle_t task;
    QueueHandle_t events;
    QueueHandle_t tx;
    SemaphoreHandle_t lock;
} solar_os_chat_state_data_t;

static solar_os_chat_state_data_t chat_state;
static const char *TAG = "solar_os_chat";
static ssize_t chat_last_io_ret;
static int chat_last_io_errno;

static void *chat_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *chat_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void chat_lock(void)
{
    if (chat_state.lock != NULL) {
        (void)xSemaphoreTake(chat_state.lock, portMAX_DELAY);
    }
}

static void chat_unlock(void)
{
    if (chat_state.lock != NULL) {
        xSemaphoreGive(chat_state.lock);
    }
}

static void chat_clear_event_queue(void)
{
    if (chat_state.events == NULL) {
        return;
    }

    solar_os_chat_event_t *event = NULL;
    while (xQueueReceive(chat_state.events, &event, 0) == pdTRUE) {
        heap_caps_free(event);
        event = NULL;
    }
    xQueueReset(chat_state.events);
}

static void chat_clear_tx_queue(void)
{
    if (chat_state.tx == NULL) {
        return;
    }

    char *line = NULL;
    while (xQueueReceive(chat_state.tx, &line, 0) == pdTRUE) {
        heap_caps_free(line);
        line = NULL;
    }
    xQueueReset(chat_state.tx);
}

static bool chat_string_is_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return allow_empty;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool chat_text_is_valid(const char *text, size_t max_len)
{
    if (text == NULL || text[0] == '\0' || strlen(text) >= max_len) {
        return false;
    }
    return true;
}

static bool chat_url_is_valid(const char *url)
{
    if (!chat_string_is_valid(url, SOLAR_OS_CHAT_URL_MAX, false)) {
        return false;
    }
    return strncmp(url, "chat://", 7) == 0 ||
        strncmp(url, "chats://", 8) == 0 ||
        strncmp(url, "tcp://", 6) == 0 ||
        strncmp(url, "tls://", 6) == 0;
}

static esp_err_t chat_parse_url(const char *url, chat_endpoint_t *endpoint)
{
    if (!chat_url_is_valid(url) || endpoint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(endpoint, 0, sizeof(*endpoint));
    const char *p = NULL;
    if (strncmp(url, "chat://", 7) == 0) {
        p = url + 7;
        endpoint->tls = false;
        endpoint->port = CHAT_DEFAULT_TCP_PORT;
    } else if (strncmp(url, "chats://", 8) == 0) {
        p = url + 8;
        endpoint->tls = true;
        endpoint->port = CHAT_DEFAULT_TLS_PORT;
    } else if (strncmp(url, "tcp://", 6) == 0) {
        p = url + 6;
        endpoint->tls = false;
        endpoint->port = CHAT_DEFAULT_TCP_PORT;
    } else if (strncmp(url, "tls://", 6) == 0) {
        p = url + 6;
        endpoint->tls = true;
        endpoint->port = CHAT_DEFAULT_TLS_PORT;
    }

    if (p == NULL || *p == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *host_start = p;
    while (*p != '\0' && *p != ':' && *p != '/') {
        p++;
    }
    const size_t host_len = (size_t)(p - host_start);
    if (host_len == 0 || host_len >= sizeof(endpoint->host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(endpoint->host, host_start, host_len);
    endpoint->host[host_len] = '\0';

    if (*p == ':') {
        p++;
        if (!isdigit((unsigned char)*p)) {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t port = 0;
        while (isdigit((unsigned char)*p)) {
            port = port * 10U + (uint32_t)(*p - '0');
            if (port > UINT16_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            p++;
        }
        if (port == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        endpoint->port = (uint16_t)port;
    }

    if (*p != '\0' && *p != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void chat_set_error_locked(esp_err_t err, const char *message)
{
    chat_state.last_esp_error = err;
    strlcpy(chat_state.last_error,
            message != NULL ? message : esp_err_to_name(err),
            sizeof(chat_state.last_error));
}

static void chat_clear_error_locked(void)
{
    chat_state.last_esp_error = ESP_OK;
    chat_state.last_error[0] = '\0';
}

static esp_err_t chat_save_config_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(CHAT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, CHAT_NVS_URL_KEY, chat_state.url);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_TOKEN_KEY, chat_state.token);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_USER_KEY, chat_state.user);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, CHAT_NVS_DEVICE_KEY, chat_state.device);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void chat_load_config_locked(void)
{
    strlcpy(chat_state.user, CHAT_DEFAULT_USER, sizeof(chat_state.user));
    strlcpy(chat_state.device, CHAT_DEFAULT_DEVICE, sizeof(chat_state.device));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(CHAT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return;
    }

    char url[SOLAR_OS_CHAT_URL_MAX] = {0};
    char token[SOLAR_OS_CHAT_TOKEN_MAX] = {0};
    char user[SOLAR_OS_CHAT_USER_MAX] = {0};
    char device[SOLAR_OS_CHAT_DEVICE_MAX] = {0};
    size_t len = sizeof(url);
    ret = nvs_get_str(nvs, CHAT_NVS_URL_KEY, url, &len);
    if (ret == ESP_OK && chat_url_is_valid(url)) {
        len = sizeof(token);
        esp_err_t token_ret = nvs_get_str(nvs, CHAT_NVS_TOKEN_KEY, token, &len);
        if (token_ret == ESP_ERR_NVS_NOT_FOUND) {
            token[0] = '\0';
            token_ret = ESP_OK;
        }

        len = sizeof(user);
        esp_err_t user_ret = nvs_get_str(nvs, CHAT_NVS_USER_KEY, user, &len);
        if (user_ret == ESP_ERR_NVS_NOT_FOUND) {
            strlcpy(user, CHAT_DEFAULT_USER, sizeof(user));
            user_ret = ESP_OK;
        }

        len = sizeof(device);
        esp_err_t device_ret = nvs_get_str(nvs, CHAT_NVS_DEVICE_KEY, device, &len);
        if (device_ret == ESP_ERR_NVS_NOT_FOUND) {
            strlcpy(device, CHAT_DEFAULT_DEVICE, sizeof(device));
            device_ret = ESP_OK;
        }

        if (token_ret == ESP_OK &&
            user_ret == ESP_OK &&
            device_ret == ESP_OK &&
            chat_string_is_valid(token, sizeof(token), true) &&
            chat_string_is_valid(user, sizeof(user), false) &&
            chat_string_is_valid(device, sizeof(device), false)) {
            strlcpy(chat_state.url, url, sizeof(chat_state.url));
            strlcpy(chat_state.token, token, sizeof(chat_state.token));
            strlcpy(chat_state.user, user, sizeof(chat_state.user));
            strlcpy(chat_state.device, device, sizeof(chat_state.device));
            chat_state.configured = true;
        }
    }
    nvs_close(nvs);
}

static solar_os_chat_event_t *chat_alloc_event(solar_os_chat_event_type_t type)
{
    solar_os_chat_event_t *event = chat_calloc(1, sizeof(*event));
    if (event != NULL) {
        event->type = type;
    }
    return event;
}

static void chat_count_dropped_event(void)
{
    chat_lock();
    chat_state.dropped_count++;
    chat_unlock();
}

static bool chat_queue_event_owned(solar_os_chat_event_t *event)
{
    if (event == NULL || chat_state.events == NULL) {
        heap_caps_free(event);
        return false;
    }

    if (xQueueSend(chat_state.events, &event, 0) != pdPASS) {
        heap_caps_free(event);
        chat_count_dropped_event();
        return false;
    }
    return true;
}

static void chat_queue_simple_event(solar_os_chat_event_type_t type, const char *text)
{
    solar_os_chat_event_t *event = chat_alloc_event(type);
    if (event == NULL) {
        chat_count_dropped_event();
        return;
    }
    if (text != NULL) {
        strlcpy(event->text, text, sizeof(event->text));
    }
    (void)chat_queue_event_owned(event);
}

static size_t chat_json_escape(const char *text, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }

    size_t used = 0;
    if (text == NULL) {
        out[0] = '\0';
        return 0;
    }

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        char esc[7] = {0};
        const char *write = esc;
        size_t write_len = 0;

        switch (*p) {
        case '"':
            write = "\\\"";
            write_len = 2;
            break;
        case '\\':
            write = "\\\\";
            write_len = 2;
            break;
        case '\n':
            write = "\\n";
            write_len = 2;
            break;
        case '\r':
            write = "\\r";
            write_len = 2;
            break;
        case '\t':
            write = "\\t";
            write_len = 2;
            break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
                write_len = 6;
            } else {
                esc[0] = (char)*p;
                write_len = 1;
            }
            break;
        }

        if (used + write_len >= out_len) {
            break;
        }
        memcpy(out + used, write, write_len);
        used += write_len;
    }
    out[used] = '\0';
    return used;
}

static const char *chat_skip_ws(const char *p)
{
    while (p != NULL && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static const char *chat_parse_json_string(const char *p, char *out, size_t out_len, bool *truncated)
{
    if (p == NULL || *p != '"' || out == NULL || out_len == 0) {
        return NULL;
    }

    p++;
    size_t used = 0;
    bool was_truncated = false;
    while (*p != '\0' && *p != '"') {
        char ch = *p++;
        if (ch == '\\') {
            ch = *p++;
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case 'b':
            case 'f':
                ch = ' ';
                break;
            case 'u':
                for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++) {
                    p++;
                }
                ch = '?';
                break;
            default:
                if (ch == '\0') {
                    return NULL;
                }
                break;
            }
        }

        if (used + 1 < out_len) {
            out[used++] = ch;
        } else {
            was_truncated = true;
        }
    }

    if (*p != '"') {
        return NULL;
    }
    out[used] = '\0';
    if (truncated != NULL && was_truncated) {
        *truncated = true;
    }
    return p + 1;
}

static const char *chat_skip_json_value(const char *p)
{
    p = chat_skip_ws(p);
    if (p == NULL) {
        return NULL;
    }
    if (*p == '"') {
        char scratch[2];
        return chat_parse_json_string(p, scratch, sizeof(scratch), NULL);
    }

    int depth = 0;
    while (*p != '\0') {
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0) {
                return p;
            }
            depth--;
        } else if (*p == ',' && depth == 0) {
            return p;
        }
        p++;
    }
    return p;
}

static bool chat_json_get_string(const char *json,
                                 const char *key,
                                 char *out,
                                 size_t out_len,
                                 bool *truncated)
{
    if (json == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const char *p = chat_skip_ws(json);
    if (p == NULL || *p != '{') {
        return false;
    }
    p++;

    while (*p != '\0') {
        p = chat_skip_ws(p);
        if (*p == '}') {
            return false;
        }

        char member[32];
        p = chat_parse_json_string(p, member, sizeof(member), NULL);
        if (p == NULL) {
            return false;
        }
        p = chat_skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p = chat_skip_ws(p + 1);

        if (strcmp(member, key) == 0 && *p == '"') {
            return chat_parse_json_string(p, out, out_len, truncated) != NULL;
        }

        p = chat_skip_json_value(p);
        if (p == NULL) {
            return false;
        }
        p = chat_skip_ws(p);
        if (*p == ',') {
            p++;
        } else if (*p == '}') {
            return false;
        }
    }

    return false;
}

static bool chat_json_get_u64(const char *json, const char *key, uint64_t *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }

    const char *p = chat_skip_ws(json);
    if (p == NULL || *p != '{') {
        return false;
    }
    p++;

    while (*p != '\0') {
        p = chat_skip_ws(p);
        if (*p == '}') {
            return false;
        }

        char member[32];
        p = chat_parse_json_string(p, member, sizeof(member), NULL);
        if (p == NULL) {
            return false;
        }
        p = chat_skip_ws(p);
        if (*p != ':') {
            return false;
        }
        p = chat_skip_ws(p + 1);

        if (strcmp(member, key) == 0 && isdigit((unsigned char)*p)) {
            uint64_t value = 0;
            while (isdigit((unsigned char)*p)) {
                value = value * 10ULL + (uint64_t)(*p - '0');
                p++;
            }
            *out = value;
            return true;
        }

        p = chat_skip_json_value(p);
        if (p == NULL) {
            return false;
        }
        p = chat_skip_ws(p);
        if (*p == ',') {
            p++;
        } else if (*p == '}') {
            return false;
        }
    }
    return false;
}

static void chat_handle_gateway_line(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }

    solar_os_chat_event_t *event = chat_alloc_event(SOLAR_OS_CHAT_EVENT_RAW);
    if (event == NULL) {
        chat_count_dropped_event();
        return;
    }

    char type[24];
    bool truncated = false;
    if (!chat_json_get_string(line, "type", type, sizeof(type), NULL)) {
        strlcpy(event->text, line, sizeof(event->text));
        event->truncated = strlen(line) >= sizeof(event->text);
        (void)chat_queue_event_owned(event);
        return;
    }

    if (strcmp(type, "msg") == 0 || strcmp(type, "message") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_MESSAGE;
    } else if (strcmp(type, "joined") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_JOINED;
    } else if (strcmp(type, "left") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_LEFT;
    } else if (strcmp(type, "channel") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_CHANNEL;
    } else if (strcmp(type, "deleted") == 0 || strcmp(type, "channel_deleted") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED;
    } else if (strcmp(type, "presence") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_PRESENCE;
    } else if (strcmp(type, "error") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_ERROR;
    } else if (strcmp(type, "connected") == 0 || strcmp(type, "hello") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_CONNECTED;
    } else if (strcmp(type, "disconnected") == 0) {
        event->type = SOLAR_OS_CHAT_EVENT_DISCONNECTED;
    }

    truncated = false;
    (void)chat_json_get_string(line, "channel", event->channel, sizeof(event->channel), &truncated);
    (void)chat_json_get_string(line, "from", event->from, sizeof(event->from), &truncated);
    if (!chat_json_get_string(line, "text", event->text, sizeof(event->text), &truncated)) {
        (void)chat_json_get_string(line, "name", event->text, sizeof(event->text), &truncated);
    }
    (void)chat_json_get_u64(line, "ts", &event->timestamp);
    uint64_t code = 0;
    if (chat_json_get_u64(line, "code", &code)) {
        event->code = (int)code;
    }
    event->truncated = truncated;

    (void)chat_queue_event_owned(event);
    chat_lock();
    chat_state.rx_count++;
    chat_unlock();
}

static bool chat_io_would_block(ssize_t ret)
{
    if (ret == ESP_TLS_ERR_SSL_WANT_READ ||
        ret == ESP_TLS_ERR_SSL_WANT_WRITE ||
        ret == ESP_TLS_ERR_SSL_TIMEOUT) {
        return true;
    }

    return ret == -1 &&
        (errno == EAGAIN ||
         errno == EWOULDBLOCK ||
         errno == EINTR);
}

static esp_err_t chat_write_all(esp_tls_t *tls, const char *data, size_t len)
{
    if (tls == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (offset < len && !chat_state.stop_requested) {
        errno = 0;
        const ssize_t written = esp_tls_conn_write(tls, data + offset, len - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (chat_io_would_block(written)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        chat_last_io_ret = written;
        chat_last_io_errno = errno;
        return ESP_FAIL;
    }

    return offset == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t chat_send_line(esp_tls_t *tls, const char *line)
{
    const esp_err_t ret = chat_write_all(tls, line, strlen(line));
    if (ret == ESP_OK) {
        chat_lock();
        chat_state.tx_count++;
        chat_unlock();
    }
    return ret;
}

static esp_err_t chat_set_socket_nonblocking(esp_tls_t *tls)
{
    int fd = -1;
    esp_err_t ret = esp_tls_get_conn_sockfd(tls, &fd);
    if (ret != ESP_OK) {
        return ret;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return ESP_FAIL;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t chat_send_hello(esp_tls_t *tls)
{
    char user[SOLAR_OS_CHAT_USER_MAX];
    char device[SOLAR_OS_CHAT_DEVICE_MAX];
    char token[SOLAR_OS_CHAT_TOKEN_MAX];
    chat_lock();
    strlcpy(user, chat_state.user, sizeof(user));
    strlcpy(device, chat_state.device, sizeof(device));
    strlcpy(token, chat_state.token, sizeof(token));
    chat_unlock();

    char user_esc[SOLAR_OS_CHAT_USER_MAX * 2];
    char device_esc[SOLAR_OS_CHAT_DEVICE_MAX * 2];
    char token_esc[SOLAR_OS_CHAT_TOKEN_MAX * 2];
    chat_json_escape(user, user_esc, sizeof(user_esc));
    chat_json_escape(device, device_esc, sizeof(device_esc));
    chat_json_escape(token, token_esc, sizeof(token_esc));

    char line[CHAT_HELLO_LINE_MAX];
    if (token_esc[0] != '\0') {
        snprintf(line,
                 sizeof(line),
                 "{\"type\":\"hello\",\"user\":\"%s\",\"device\":\"%s\",\"token\":\"%s\"}\n",
                 user_esc,
                 device_esc,
                 token_esc);
    } else {
        snprintf(line,
                 sizeof(line),
                 "{\"type\":\"hello\",\"user\":\"%s\",\"device\":\"%s\"}\n",
                 user_esc,
                 device_esc);
    }
    return chat_send_line(tls, line);
}

static void chat_task_set_state(bool running, bool connected, esp_err_t err, const char *error)
{
    chat_lock();
    chat_state.running = running;
    chat_state.connected = connected;
    if (err == ESP_OK) {
        chat_clear_error_locked();
    } else {
        chat_set_error_locked(err, error);
    }
    chat_unlock();
}

static void chat_gateway_task(void *arg)
{
    (void)arg;

    char url[SOLAR_OS_CHAT_URL_MAX];
    chat_lock();
    strlcpy(url, chat_state.url, sizeof(url));
    chat_unlock();

    chat_endpoint_t endpoint;
    esp_err_t ret = chat_parse_url(url, &endpoint);
    if (ret != ESP_OK) {
        chat_task_set_state(false, false, ret, "invalid gateway URL");
        chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "invalid gateway URL");
        chat_state.task_done = true;
        vTaskDelete(NULL);
        return;
    }

    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        chat_task_set_state(false, false, ESP_ERR_NO_MEM, "TLS init failed");
        chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "TLS init failed");
        chat_state.task_done = true;
        vTaskDelete(NULL);
        return;
    }

    esp_tls_cfg_t cfg = {
        .is_plain_tcp = !endpoint.tls,
        .non_block = endpoint.tls,
        .timeout_ms = CHAT_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = endpoint.tls ? esp_crt_bundle_attach : NULL,
    };

    SOLAR_OS_LOGI(TAG,
                  "connecting to %s:%u %s",
                  endpoint.host,
                  (unsigned)endpoint.port,
                  endpoint.tls ? "TLS" : "TCP");
    const int connected = esp_tls_conn_new_sync(endpoint.host,
                                                (int)strlen(endpoint.host),
                                                endpoint.port,
                                                &cfg,
                                                tls);
    if (connected != 1 || chat_state.stop_requested) {
        esp_tls_conn_destroy(tls);
        chat_task_set_state(false, false, ESP_FAIL, "gateway connect failed");
        if (!chat_state.stop_requested) {
            chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "gateway connect failed");
        }
        chat_state.task_done = true;
        vTaskDelete(NULL);
        return;
    }

    if (!endpoint.tls) {
        ret = chat_set_socket_nonblocking(tls);
        if (ret != ESP_OK) {
            esp_tls_conn_destroy(tls);
            chat_task_set_state(false, false, ret, "gateway socket setup failed");
            chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "gateway socket setup failed");
            chat_state.task_done = true;
            vTaskDelete(NULL);
            return;
        }
    }

    chat_task_set_state(true, false, ESP_OK, NULL);
    ret = chat_send_hello(tls);
    if (ret != ESP_OK || chat_state.stop_requested) {
        SOLAR_OS_LOGW(TAG,
                      "gateway hello write failed: ret=%d errno=%d",
                      (int)chat_last_io_ret,
                      chat_last_io_errno);
        esp_tls_conn_destroy(tls);
        chat_task_set_state(false, false, ret != ESP_OK ? ret : ESP_FAIL, "gateway write failed");
        if (!chat_state.stop_requested) {
            chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "gateway write failed");
        }
        chat_state.task_done = true;
        vTaskDelete(NULL);
        return;
    }
    chat_task_set_state(true, true, ESP_OK, NULL);
    chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_CONNECTED, "connected");

    char rx[128];
    char *line = chat_malloc(CHAT_LINE_MAX);
    if (line == NULL) {
        esp_tls_conn_destroy(tls);
        chat_task_set_state(false, false, ESP_ERR_NO_MEM, "chat RX alloc failed");
        chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "chat RX alloc failed");
        chat_state.task_done = true;
        vTaskDelete(NULL);
        return;
    }
    size_t line_len = 0;
    bool line_truncated = false;

    while (!chat_state.stop_requested) {
        char *tx_line = NULL;
        while (!chat_state.stop_requested &&
               xQueueReceive(chat_state.tx, &tx_line, 0) == pdTRUE) {
            ret = chat_send_line(tls, tx_line);
            heap_caps_free(tx_line);
            tx_line = NULL;
            if (ret != ESP_OK) {
                SOLAR_OS_LOGW(TAG,
                              "gateway write failed: ret=%d errno=%d",
                              (int)chat_last_io_ret,
                              chat_last_io_errno);
                chat_task_set_state(true, false, ret, "gateway write failed");
                chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "gateway write failed");
                chat_state.stop_requested = true;
                break;
            }
        }
        if (chat_state.stop_requested) {
            break;
        }

        const ssize_t read_len = esp_tls_conn_read(tls, rx, sizeof(rx));
        if (read_len > 0) {
            for (ssize_t i = 0; i < read_len; i++) {
                const char ch = rx[i];
                if (ch == '\n') {
                    line[line_len] = '\0';
                    if (line_len > 0 && line[line_len - 1U] == '\r') {
                        line[line_len - 1U] = '\0';
                    }
                    chat_handle_gateway_line(line);
                    if (line_truncated) {
                        solar_os_chat_event_t *event =
                            chat_alloc_event(SOLAR_OS_CHAT_EVENT_ERROR);
                        if (event != NULL) {
                            event->truncated = true;
                            strlcpy(event->text, "gateway line truncated", sizeof(event->text));
                            (void)chat_queue_event_owned(event);
                        } else {
                            chat_count_dropped_event();
                        }
                    }
                    line_len = 0;
                    line_truncated = false;
                } else if (line_len + 1 < CHAT_LINE_MAX) {
                    line[line_len++] = ch;
                } else {
                    line_truncated = true;
                }
            }
            continue;
        }

        if (read_len == 0) {
            break;
        }
        if (chat_io_would_block(read_len)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        SOLAR_OS_LOGW(TAG,
                      "gateway read failed: ret=%d errno=%d",
                      (int)read_len,
                      errno);
        chat_task_set_state(true, false, ESP_FAIL, "gateway read failed");
        chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_ERROR, "gateway read failed");
        break;
    }

    heap_caps_free(line);
    esp_tls_conn_destroy(tls);
    chat_task_set_state(false, false, ESP_OK, NULL);
    if (!chat_state.stop_requested) {
        chat_queue_simple_event(SOLAR_OS_CHAT_EVENT_DISCONNECTED, "disconnected");
    }
    chat_state.task_done = true;
    vTaskDelete(NULL);
    return;
}

esp_err_t solar_os_chat_init(void)
{
    if (chat_state.initialized) {
        return ESP_OK;
    }

    chat_state.lock = xSemaphoreCreateMutex();
    if (chat_state.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    chat_state.events = xQueueCreate(CHAT_EVENT_QUEUE_LEN, sizeof(solar_os_chat_event_t *));
    if (chat_state.events == NULL) {
        vSemaphoreDelete(chat_state.lock);
        chat_state.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    chat_state.tx = xQueueCreate(CHAT_TX_QUEUE_LEN, sizeof(char *));
    if (chat_state.tx == NULL) {
        vQueueDelete(chat_state.events);
        chat_state.events = NULL;
        vSemaphoreDelete(chat_state.lock);
        chat_state.lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    chat_state.initialized = true;
    chat_lock();
    chat_load_config_locked();
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_configure(const char *url,
                                  const char *token,
                                  const char *user,
                                  const char *device)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }

    chat_lock();
    if (url != NULL && url[0] != '\0') {
        if (!chat_url_is_valid(url)) {
            chat_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(chat_state.url, url, sizeof(chat_state.url));
        chat_state.configured = true;
    }
    if (token != NULL) {
        if (!chat_string_is_valid(token, SOLAR_OS_CHAT_TOKEN_MAX, true)) {
            chat_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(chat_state.token, token, sizeof(chat_state.token));
    }
    if (user != NULL) {
        if (!chat_string_is_valid(user, SOLAR_OS_CHAT_USER_MAX, false)) {
            chat_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(chat_state.user, user, sizeof(chat_state.user));
    }
    if (device != NULL) {
        if (!chat_string_is_valid(device, SOLAR_OS_CHAT_DEVICE_MAX, false)) {
            chat_unlock();
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(chat_state.device, device, sizeof(chat_state.device));
    }
    if (!chat_state.configured || !chat_url_is_valid(chat_state.url)) {
        chat_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    ret = chat_save_config_locked();
    if (ret != ESP_OK) {
        chat_set_error_locked(ret, esp_err_to_name(ret));
        chat_unlock();
        return ret;
    }
    chat_unlock();
    return ESP_OK;
}

esp_err_t solar_os_chat_connect(const char *url,
                                const char *token,
                                const char *user,
                                const char *device)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = solar_os_chat_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = solar_os_chat_configure(url, token, user, device);
    if (ret != ESP_OK) {
        return ret;
    }

    chat_clear_event_queue();
    chat_clear_tx_queue();
    chat_state.stop_requested = false;
    chat_state.task_done = false;

    chat_lock();
    chat_state.running = true;
    chat_state.connected = false;
    chat_clear_error_locked();
    chat_unlock();

    BaseType_t created = xTaskCreatePinnedToCore(chat_gateway_task,
                                                 "solar_os_chat",
                                                 CHAT_TASK_STACK,
                                                 NULL,
                                                 CHAT_TASK_PRIORITY,
                                                 &chat_state.task,
                                                 tskNO_AFFINITY);
    if (created != pdPASS) {
        chat_lock();
        chat_state.running = false;
        chat_state.connected = false;
        chat_set_error_locked(ESP_ERR_NO_MEM, "task create failed");
        chat_unlock();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t solar_os_chat_disconnect(void)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (chat_state.task == NULL && !chat_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    chat_state.stop_requested = true;
    if (!solar_os_task_wait_done(chat_state.task,
                                 &chat_state.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    chat_state.task = NULL;
    chat_lock();
    chat_state.running = false;
    chat_state.connected = false;
    chat_unlock();
    chat_clear_event_queue();
    chat_clear_tx_queue();
    return ESP_OK;
}

static esp_err_t chat_queue_tx_line(const char *line)
{
    if (line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (!chat_state.running || !chat_state.connected || chat_state.tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t line_len = strlen(line);
    char *queued = chat_malloc(line_len + 1U);
    if (queued == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(queued, line, line_len + 1U);

    if (xQueueSend(chat_state.tx, &queued, 0) != pdTRUE) {
        heap_caps_free(queued);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t solar_os_chat_join(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }

    char channel_esc[SOLAR_OS_CHAT_CHANNEL_MAX * 2];
    chat_json_escape(channel, channel_esc, sizeof(channel_esc));

    char line[CHAT_HELLO_LINE_MAX];
    snprintf(line, sizeof(line), "{\"type\":\"join\",\"channel\":\"%s\"}\n", channel_esc);
    return chat_queue_tx_line(line);
}

esp_err_t solar_os_chat_leave(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }

    char channel_esc[SOLAR_OS_CHAT_CHANNEL_MAX * 2];
    chat_json_escape(channel, channel_esc, sizeof(channel_esc));

    char line[CHAT_HELLO_LINE_MAX];
    snprintf(line, sizeof(line), "{\"type\":\"leave\",\"channel\":\"%s\"}\n", channel_esc);
    return chat_queue_tx_line(line);
}

esp_err_t solar_os_chat_delete_channel(const char *channel)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }

    char channel_esc[SOLAR_OS_CHAT_CHANNEL_MAX * 2];
    chat_json_escape(channel, channel_esc, sizeof(channel_esc));

    char line[CHAT_HELLO_LINE_MAX];
    snprintf(line, sizeof(line), "{\"type\":\"delete\",\"channel\":\"%s\"}\n", channel_esc);
    return chat_queue_tx_line(line);
}

esp_err_t solar_os_chat_send(const char *channel, const char *text)
{
    if (!chat_string_is_valid(channel, SOLAR_OS_CHAT_CHANNEL_MAX, false) ||
        !chat_text_is_valid(text, SOLAR_OS_CHAT_TEXT_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    char channel_esc[SOLAR_OS_CHAT_CHANNEL_MAX * 2];
    char *text_esc = chat_malloc(CHAT_TEXT_ESC_MAX);
    char *line = chat_malloc(CHAT_TX_LINE_MAX);
    if (text_esc == NULL || line == NULL) {
        heap_caps_free(text_esc);
        heap_caps_free(line);
        return ESP_ERR_NO_MEM;
    }

    chat_json_escape(channel, channel_esc, sizeof(channel_esc));
    chat_json_escape(text, text_esc, CHAT_TEXT_ESC_MAX);

    snprintf(line,
             CHAT_TX_LINE_MAX,
             "{\"type\":\"msg\",\"channel\":\"%s\",\"text\":\"%s\"}\n",
             channel_esc,
             text_esc);
    const esp_err_t ret = chat_queue_tx_line(line);
    heap_caps_free(text_esc);
    heap_caps_free(line);
    return ret;
}

esp_err_t solar_os_chat_read_event(solar_os_chat_event_t *event, uint32_t timeout_ms)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_chat_event_t *queued = NULL;
    if (xQueueReceive(chat_state.events, &queued, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (queued == NULL) {
        return ESP_FAIL;
    }
    *event = *queued;
    heap_caps_free(queued);
    return ESP_OK;
}

esp_err_t solar_os_chat_get_status(solar_os_chat_status_t *status)
{
    esp_err_t ret = solar_os_chat_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    chat_lock();
    memset(status, 0, sizeof(*status));
    status->initialized = chat_state.initialized;
    status->configured = chat_state.configured;
    status->running = chat_state.running;
    status->connected = chat_state.connected;
    status->token_set = chat_state.token[0] != '\0';
    status->state = chat_state.connected ? SOLAR_OS_CHAT_STATE_CONNECTED :
        (chat_state.running ? SOLAR_OS_CHAT_STATE_CONNECTING : SOLAR_OS_CHAT_STATE_DISCONNECTED);
    strlcpy(status->url, chat_state.url, sizeof(status->url));
    strlcpy(status->user, chat_state.user, sizeof(status->user));
    strlcpy(status->device, chat_state.device, sizeof(status->device));
    strlcpy(status->last_error, chat_state.last_error, sizeof(status->last_error));
    status->last_esp_error = chat_state.last_esp_error;
    status->rx_count = chat_state.rx_count;
    status->tx_count = chat_state.tx_count;
    status->dropped_count = chat_state.dropped_count;
    status->queued_events = chat_state.events != NULL ? uxQueueMessagesWaiting(chat_state.events) : 0;
    chat_unlock();
    return ESP_OK;
}

const char *solar_os_chat_state_name(solar_os_chat_state_t state)
{
    switch (state) {
    case SOLAR_OS_CHAT_STATE_CONNECTING:
        return "connecting";
    case SOLAR_OS_CHAT_STATE_CONNECTED:
        return "connected";
    case SOLAR_OS_CHAT_STATE_DISCONNECTED:
    default:
        return "disconnected";
    }
}

const char *solar_os_chat_event_type_name(solar_os_chat_event_type_t type)
{
    switch (type) {
    case SOLAR_OS_CHAT_EVENT_CONNECTED:
        return "connected";
    case SOLAR_OS_CHAT_EVENT_DISCONNECTED:
        return "disconnected";
    case SOLAR_OS_CHAT_EVENT_ERROR:
        return "error";
    case SOLAR_OS_CHAT_EVENT_CHANNEL:
        return "channel";
    case SOLAR_OS_CHAT_EVENT_CHANNEL_DELETED:
        return "deleted";
    case SOLAR_OS_CHAT_EVENT_JOINED:
        return "joined";
    case SOLAR_OS_CHAT_EVENT_LEFT:
        return "left";
    case SOLAR_OS_CHAT_EVENT_MESSAGE:
        return "message";
    case SOLAR_OS_CHAT_EVENT_PRESENCE:
        return "presence";
    case SOLAR_OS_CHAT_EVENT_RAW:
    default:
        return "raw";
    }
}
