#include "solar_os_http_client.h"

#include <limits.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define SOLAR_OS_HTTP_DEFAULT_TIMEOUT_MS 10000U
#define SOLAR_OS_HTTP_DEFAULT_RX_BUFFER 1024U
#define SOLAR_OS_HTTP_DEFAULT_TX_BUFFER 512U
#define SOLAR_OS_HTTP_DEFAULT_MAX_REDIRECTS 10U
#define SOLAR_OS_HTTP_STREAM_CHUNK 1024U
#define SOLAR_OS_HTTP_REQUEST_LINE_RESERVE 32U
#define SOLAR_OS_HTTP_HEADER_LINE_RESERVE 7U
#define SOLAR_OS_HTTP_SESSION_RX_BUFFER 1024U
#define SOLAR_OS_HTTP_SESSION_TX_BUFFER 4096U
#define SOLAR_OS_HTTP_SESSION_ORIGIN_MAX 512U
#define SOLAR_OS_HTTP_SESSION_USER_AGENT_MAX 160U
#define SOLAR_OS_HTTP_SESSION_CLOSE_WAIT_MS 2000U
#define SOLAR_OS_HTTP_SESSION_TASK_STACK (12U * 1024U)
#define SOLAR_OS_HTTP_SESSION_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(SOLAR_OS_HTTP_SESSION_TASK_STACK);

struct solar_os_http_request {
    solar_os_http_request_options_t options;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    esp_http_client_handle_t client;
    volatile bool cancel_requested;
    bool active;
    bool performed;
    bool deadline_exceeded;
    volatile esp_err_t event_error;
    int64_t started_us;
    int64_t deadline_us;
    uint64_t bytes_received;
    bool persistent_perform;
    bool response_started;
};

typedef struct solar_os_http_session_connection {
    esp_http_client_handle_t client;
    char *origin;
    char *host_header;
    char *user_agent;
    solar_os_http_request_t *active_request;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    bool closing;
} solar_os_http_session_connection_t;

struct solar_os_http_session_context {
    solar_os_http_cancel_fn should_cancel;
    void *cancel_user_data;
    solar_os_http_session_connection_t *connections[SOLAR_OS_HTTP_SESSION_MAX_HANDLES];
    uint32_t generations[SOLAR_OS_HTTP_SESSION_MAX_HANDLES];
};

typedef struct {
    solar_os_http_session_connection_t *connection;
    solar_os_http_request_t *request;
    solar_os_http_response_t *response;
    esp_err_t result;
    volatile bool done;
} solar_os_http_session_work_t;

static portMUX_TYPE http_session_global_lock = portMUX_INITIALIZER_UNLOCKED;
static size_t http_session_global_count;

static esp_err_t solar_os_http_deliver_data(solar_os_http_request_t *request,
                                            int status_code,
                                            const uint8_t *data,
                                            size_t data_len);
static esp_err_t solar_os_http_deliver_response(solar_os_http_request_t *request,
                                                int status_code,
                                                int64_t content_length);

static bool solar_os_http_cancelled(const solar_os_http_request_t *request)
{
    return request->cancel_requested ||
        (request->options.cancel_flag != NULL &&
         *request->options.cancel_flag) ||
        (request->options.should_cancel != NULL &&
         request->options.should_cancel(request->options.cancel_user_data));
}

bool solar_os_http_method_parse(const char *text,
                                solar_os_http_method_t *method)
{
    if (text == NULL || method == NULL) {
        return false;
    }

    static const struct {
        const char *name;
        solar_os_http_method_t method;
    } methods[] = {
        {"GET", SOLAR_OS_HTTP_METHOD_GET},
        {"POST", SOLAR_OS_HTTP_METHOD_POST},
        {"PUT", SOLAR_OS_HTTP_METHOD_PUT},
        {"PATCH", SOLAR_OS_HTTP_METHOD_PATCH},
        {"DELETE", SOLAR_OS_HTTP_METHOD_DELETE},
        {"HEAD", SOLAR_OS_HTTP_METHOD_HEAD},
    };

    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (strcasecmp(text, methods[i].name) == 0) {
            *method = methods[i].method;
            return true;
        }
    }
    return false;
}

static bool solar_os_http_method_valid(solar_os_http_method_t method)
{
    return method >= SOLAR_OS_HTTP_METHOD_GET && method <= SOLAR_OS_HTTP_METHOD_HEAD;
}

static esp_err_t solar_os_http_transmit_buffer_size(
    const solar_os_http_request_options_t *options,
    int *out_size)
{
    size_t required = strlen(options->url);
    if (required > (size_t)INT_MAX - SOLAR_OS_HTTP_REQUEST_LINE_RESERVE) {
        return ESP_ERR_INVALID_ARG;
    }
    required += SOLAR_OS_HTTP_REQUEST_LINE_RESERVE;

    if (options->user_agent != NULL) {
        const size_t value_len = strlen(options->user_agent);
        if (value_len > (size_t)INT_MAX - (sizeof("User-Agent") - 1U) -
                SOLAR_OS_HTTP_HEADER_LINE_RESERVE) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t line_size = (sizeof("User-Agent") - 1U) + value_len +
            SOLAR_OS_HTTP_HEADER_LINE_RESERVE;
        if (line_size > required) {
            required = line_size;
        }
    }

    for (size_t i = 0; i < options->header_count; i++) {
        const solar_os_http_header_t *header = &options->headers[i];
        if (header->name == NULL || header->value == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t name_len = strlen(header->name);
        const size_t value_len = strlen(header->value);
        if (name_len > (size_t)INT_MAX - SOLAR_OS_HTTP_HEADER_LINE_RESERVE ||
            value_len > (size_t)INT_MAX - name_len -
                SOLAR_OS_HTTP_HEADER_LINE_RESERVE) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t line_size = name_len + value_len +
            SOLAR_OS_HTTP_HEADER_LINE_RESERVE;
        if (line_size > required) {
            required = line_size;
        }
    }

    size_t configured = options->transmit_buffer_size != 0U ?
        options->transmit_buffer_size : SOLAR_OS_HTTP_DEFAULT_TX_BUFFER;
    if (configured < required) {
        configured = required;
    }
    *out_size = (int)configured;
    return ESP_OK;
}

static esp_http_client_method_t solar_os_http_esp_method(solar_os_http_method_t method)
{
    switch (method) {
    case SOLAR_OS_HTTP_METHOD_POST:
        return HTTP_METHOD_POST;
    case SOLAR_OS_HTTP_METHOD_PUT:
        return HTTP_METHOD_PUT;
    case SOLAR_OS_HTTP_METHOD_PATCH:
        return HTTP_METHOD_PATCH;
    case SOLAR_OS_HTTP_METHOD_DELETE:
        return HTTP_METHOD_DELETE;
    case SOLAR_OS_HTTP_METHOD_HEAD:
        return HTTP_METHOD_HEAD;
    case SOLAR_OS_HTTP_METHOD_GET:
    default:
        return HTTP_METHOD_GET;
    }
}

static int solar_os_http_remaining_ms(solar_os_http_request_t *request)
{
    if (request->deadline_us == 0) {
        return INT_MAX;
    }

    const int64_t remaining_us = request->deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        request->deadline_exceeded = true;
        return 0;
    }

    const int64_t remaining_ms = (remaining_us + 999) / 1000;
    return remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
}

static esp_err_t solar_os_http_apply_timeout_value(
    solar_os_http_request_t *request,
    esp_http_client_handle_t client,
    uint32_t requested_timeout_ms)
{
    int timeout_ms = requested_timeout_ms != 0 ?
        (int)requested_timeout_ms :
        request->options.timeout_ms != 0 ?
            (int)request->options.timeout_ms :
            (int)SOLAR_OS_HTTP_DEFAULT_TIMEOUT_MS;
    const int remaining_ms = solar_os_http_remaining_ms(request);
    if (remaining_ms == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (remaining_ms < timeout_ms) {
        timeout_ms = remaining_ms;
    }
    return esp_http_client_set_timeout_ms(client, timeout_ms);
}

static esp_err_t solar_os_http_apply_timeout(solar_os_http_request_t *request,
                                             esp_http_client_handle_t client)
{
    return solar_os_http_apply_timeout_value(request, client, 0U);
}

static esp_err_t solar_os_http_event_bridge(esp_http_client_event_t *esp_event)
{
    if (esp_event == NULL || esp_event->user_data == NULL) {
        return ESP_OK;
    }

    solar_os_http_request_t *request = esp_event->user_data;
    if (solar_os_http_cancelled(request)) {
        request->event_error = ESP_ERR_INVALID_STATE;
        return ESP_FAIL;
    }
    if (request->event_error != ESP_OK) {
        return ESP_FAIL;
    }

    const esp_err_t timeout_err = solar_os_http_apply_timeout(request, esp_event->client);
    if (timeout_err != ESP_OK) {
        request->event_error = timeout_err;
        return ESP_FAIL;
    }

    if (request->persistent_perform && esp_event->event_id == HTTP_EVENT_ON_DATA) {
        request->response_started = true;
        return solar_os_http_deliver_data(
            request,
            esp_http_client_get_status_code(esp_event->client),
            esp_event->data,
            esp_event->data_len > 0 ? (size_t)esp_event->data_len : 0U);
    }
    if (request->persistent_perform && esp_event->event_id == HTTP_EVENT_ON_FINISH) {
        return solar_os_http_deliver_response(
            request,
            esp_http_client_get_status_code(esp_event->client),
            esp_http_client_get_content_length(esp_event->client));
    }
    if (esp_event->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }

    request->response_started = true;
    if (request->options.event_handler == NULL) {
        return ESP_OK;
    }

    solar_os_http_event_t event = {
        .type = SOLAR_OS_HTTP_EVENT_HEADER,
        .status_code = esp_event->client != NULL ?
            esp_http_client_get_status_code(esp_event->client) : -1,
        .header_name = esp_event->header_key,
        .header_value = esp_event->header_value,
    };

    const esp_err_t err = request->options.event_handler(&event,
                                                         request->options.user_data);
    if (err != ESP_OK) {
        request->event_error = err;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t solar_os_http_abort_error(solar_os_http_request_t *request)
{
    if (solar_os_http_cancelled(request)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (solar_os_http_remaining_ms(request) == 0) {
        return ESP_ERR_TIMEOUT;
    }
    return request->event_error;
}

static esp_err_t solar_os_http_read_error(int read_result)
{
    if (read_result == -ESP_ERR_HTTP_EAGAIN) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
}

static esp_err_t solar_os_http_send_body(solar_os_http_request_t *request,
                                         esp_http_client_handle_t client)
{
    size_t offset = 0;
    while (offset < request->options.body_len) {
        esp_err_t err = solar_os_http_abort_error(request);
        if (err != ESP_OK) {
            return err;
        }
        err = solar_os_http_apply_timeout(request, client);
        if (err != ESP_OK) {
            return err;
        }

        const size_t remaining = request->options.body_len - offset;
        const int written = esp_http_client_write(client,
                                                  (const char *)request->options.body + offset,
                                                  (int)remaining);
        if (written <= 0) {
            return ESP_ERR_HTTP_WRITE_DATA;
        }
        offset += (size_t)written;
    }
    return ESP_OK;
}

static esp_err_t solar_os_http_deliver_data(solar_os_http_request_t *request,
                                            int status_code,
                                            const uint8_t *data,
                                            size_t data_len)
{
    if (data_len == 0) {
        return ESP_OK;
    }

    request->bytes_received += data_len;
    if (request->options.event_handler == NULL) {
        return ESP_OK;
    }

    const solar_os_http_event_t event = {
        .type = SOLAR_OS_HTTP_EVENT_DATA,
        .status_code = status_code,
        .data = data,
        .data_len = data_len,
    };
    const esp_err_t err = request->options.event_handler(&event,
                                                         request->options.user_data);
    if (err != ESP_OK) {
        request->event_error = err;
    }
    return err;
}

static esp_err_t solar_os_http_deliver_response(solar_os_http_request_t *request,
                                                int status_code,
                                                int64_t content_length)
{
    if (request->options.event_handler == NULL) {
        return ESP_OK;
    }

    const solar_os_http_event_t event = {
        .type = SOLAR_OS_HTTP_EVENT_RESPONSE,
        .status_code = status_code,
        .content_length = content_length,
    };
    const esp_err_t err = request->options.event_handler(&event,
                                                         request->options.user_data);
    if (err != ESP_OK) {
        request->event_error = err;
    }
    return err;
}

static esp_err_t solar_os_http_perform_stream(solar_os_http_request_t *request,
                                              esp_http_client_handle_t client,
                                              int *status_code,
                                              int64_t *content_length)
{
    uint8_t buffer[SOLAR_OS_HTTP_STREAM_CHUNK];
    const uint8_t redirect_limit = request->options.max_redirects != 0 ?
        request->options.max_redirects : SOLAR_OS_HTTP_DEFAULT_MAX_REDIRECTS;
    uint8_t redirects = 0;

    while (true) {
        esp_err_t err = solar_os_http_abort_error(request);
        if (err != ESP_OK) {
            return err;
        }
        err = solar_os_http_apply_timeout(request, client);
        if (err != ESP_OK) {
            return err;
        }

        err = esp_http_client_open(client, (int)request->options.body_len);
        if (err != ESP_OK) {
            return err;
        }
        err = solar_os_http_send_body(request, client);
        if (err != ESP_OK) {
            return err;
        }

        err = solar_os_http_apply_timeout(request, client);
        if (err != ESP_OK) {
            return err;
        }
        const int64_t header_result = esp_http_client_fetch_headers(client);
        if (header_result < 0) {
            return solar_os_http_read_error((int)header_result);
        }

        *status_code = esp_http_client_get_status_code(client);
        *content_length = esp_http_client_get_content_length(client);
        err = solar_os_http_abort_error(request);
        if (err != ESP_OK) {
            return err;
        }

        if (request->options.follow_redirects &&
            *status_code >= 300 && *status_code < 400) {
            if (redirects >= redirect_limit) {
                return ESP_ERR_HTTP_MAX_REDIRECT;
            }
            err = esp_http_client_set_redirection(client);
            if (err != ESP_OK) {
                return err;
            }
            redirects++;
            (void)esp_http_client_clear_response_buffer(client);
            (void)esp_http_client_close(client);
            continue;
        }

        err = solar_os_http_deliver_response(request,
                                             *status_code,
                                             *content_length);
        if (err != ESP_OK) {
            return err;
        }

        if (request->options.method == SOLAR_OS_HTTP_METHOD_HEAD) {
            return ESP_OK;
        }

        while (true) {
            err = solar_os_http_abort_error(request);
            if (err != ESP_OK) {
                return err;
            }
            err = solar_os_http_apply_timeout_value(
                request, client, request->options.read_poll_ms);
            if (err != ESP_OK) {
                return err;
            }

            const int read_len = esp_http_client_read(client,
                                                      (char *)buffer,
                                                      sizeof(buffer));
            err = solar_os_http_abort_error(request);
            if (err != ESP_OK) {
                return err;
            }
            if (read_len == -ESP_ERR_HTTP_EAGAIN &&
                request->options.read_poll_ms != 0U) {
                continue;
            }
            if (read_len < 0) {
                return solar_os_http_read_error(read_len);
            }
            if (read_len == 0) {
                return ESP_OK;
            }

            err = solar_os_http_deliver_data(request,
                                             *status_code,
                                             buffer,
                                             (size_t)read_len);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
}

static void solar_os_http_finish_request(solar_os_http_request_t *request,
                                         esp_http_client_handle_t client)
{
    xSemaphoreTake(request->lock, portMAX_DELAY);
    request->client = NULL;
    request->active = false;
    xSemaphoreGive(request->lock);

    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
}

esp_err_t solar_os_http_request_create(const solar_os_http_request_options_t *options,
                                       solar_os_http_request_t **out_request)
{
    if (options == NULL || out_request == NULL || options->url == NULL ||
        options->url[0] == '\0' || !solar_os_http_method_valid(options->method) ||
        (options->header_count > 0 && options->headers == NULL) ||
        (options->body_len > 0 && options->body == NULL) ||
        options->body_len > INT_MAX || options->timeout_ms > INT_MAX ||
        options->read_poll_ms > INT_MAX ||
        options->receive_buffer_size > INT_MAX || options->transmit_buffer_size > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_request = NULL;
    solar_os_http_request_t *request =
        solar_os_memory_calloc(1,
                               sizeof(*request),
                               SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
                               "http.request");
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }

    request->options = *options;
    request->lock = xSemaphoreCreateMutexStatic(&request->lock_storage);
    if (request->lock == NULL) {
        solar_os_memory_free(request);
        return ESP_ERR_NO_MEM;
    }

    *out_request = request;
    return ESP_OK;
}

esp_err_t solar_os_http_request_perform(solar_os_http_request_t *request,
                                        solar_os_http_response_t *response)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (response != NULL) {
        memset(response, 0, sizeof(*response));
        response->status_code = -1;
        response->content_length = -1;
    }

    xSemaphoreTake(request->lock, portMAX_DELAY);
    if (request->active || request->performed ||
        solar_os_http_cancelled(request)) {
        xSemaphoreGive(request->lock);
        if (response != NULL) {
            response->cancelled = solar_os_http_cancelled(request);
        }
        return ESP_ERR_INVALID_STATE;
    }
    request->active = true;
    request->performed = true;
    xSemaphoreGive(request->lock);

    request->started_us = esp_timer_get_time();
    request->deadline_us = request->options.deadline_ms != 0 ?
        request->started_us + ((int64_t)request->options.deadline_ms * 1000) : 0;

    const uint32_t transport_timeout = request->options.timeout_ms != 0 ?
        request->options.timeout_ms : SOLAR_OS_HTTP_DEFAULT_TIMEOUT_MS;
    uint32_t initial_timeout = transport_timeout;
    if (request->options.deadline_ms != 0 && request->options.deadline_ms < initial_timeout) {
        initial_timeout = request->options.deadline_ms;
    }

    int transmit_buffer_size = 0;
    const esp_err_t transmit_buffer_err = solar_os_http_transmit_buffer_size(
        &request->options, &transmit_buffer_size);
    if (transmit_buffer_err != ESP_OK) {
        solar_os_http_finish_request(request, NULL);
        return transmit_buffer_err;
    }

    esp_http_client_config_t config = {
        .url = request->options.url,
        .method = solar_os_http_esp_method(request->options.method),
        .user_agent = request->options.user_agent,
        .timeout_ms = (int)initial_timeout,
        .disable_auto_redirect = true,
        .event_handler = solar_os_http_event_bridge,
        .buffer_size = request->options.receive_buffer_size != 0 ?
            (int)request->options.receive_buffer_size : (int)SOLAR_OS_HTTP_DEFAULT_RX_BUFFER,
        .buffer_size_tx = transmit_buffer_size,
        .user_data = request,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        solar_os_http_finish_request(request, NULL);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(request->lock, portMAX_DELAY);
    request->client = client;
    const bool cancelled = solar_os_http_cancelled(request);
    xSemaphoreGive(request->lock);
    if (cancelled) {
        solar_os_http_finish_request(request, client);
        if (response != NULL) {
            response->cancelled = true;
        }
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < request->options.header_count; i++) {
        const solar_os_http_header_t *header = &request->options.headers[i];
        if (header->name == NULL || header->value == NULL) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        err = esp_http_client_set_header(client, header->name, header->value);
        if (err != ESP_OK) {
            break;
        }
    }
    int status_code = -1;
    int64_t content_length = -1;
    if (err == ESP_OK) {
        err = solar_os_http_perform_stream(request,
                                           client,
                                           &status_code,
                                           &content_length);
    }
    const int64_t finished_us = esp_timer_get_time();
    (void)solar_os_http_remaining_ms(request);

    if (solar_os_http_cancelled(request)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (request->deadline_exceeded) {
        err = ESP_ERR_TIMEOUT;
    } else if (request->event_error != ESP_OK) {
        err = request->event_error;
    }

    if (response != NULL) {
        response->status_code = status_code;
        response->content_length = content_length;
        response->bytes_received = request->bytes_received;
        response->duration_ms = finished_us > request->started_us ?
            (uint32_t)((finished_us - request->started_us + 999) / 1000) : 0;
        response->cancelled = solar_os_http_cancelled(request);
        response->deadline_exceeded = request->deadline_exceeded;
    }

    solar_os_http_finish_request(request, client);
    return err;
}

esp_err_t solar_os_http_request_cancel(solar_os_http_request_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(request->lock, portMAX_DELAY);
    request->cancel_requested = true;
    if (request->client != NULL) {
        (void)esp_http_client_cancel_request(request->client);
    }
    xSemaphoreGive(request->lock);
    return ESP_OK;
}

static void solar_os_http_request_abort(solar_os_http_request_t *request)
{
    xSemaphoreTake(request->lock, portMAX_DELAY);
    if (request->client != NULL) {
        (void)esp_http_client_cancel_request(request->client);
    }
    xSemaphoreGive(request->lock);
}

esp_err_t solar_os_http_request_destroy(solar_os_http_request_t *request)
{
    if (request == NULL) {
        return ESP_OK;
    }

    xSemaphoreTake(request->lock, portMAX_DELAY);
    if (request->active) {
        xSemaphoreGive(request->lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(request->lock);

    vSemaphoreDelete(request->lock);
    solar_os_memory_free(request);
    return ESP_OK;
}

typedef struct {
    solar_os_http_buffered_response_t *response;
    size_t body_capacity;
    size_t header_bytes;
    bool follow_redirects;
    solar_os_http_event_fn chained_handler;
    void *chained_user_data;
    esp_err_t error;
} solar_os_http_buffered_collector_t;

static void solar_os_http_buffered_headers_clear(
    solar_os_http_buffered_response_t *response)
{
    if (response == NULL || response->headers == NULL) {
        return;
    }
    for (size_t i = 0; i < response->header_count; i++) {
        solar_os_memory_free(response->headers[i].name);
    }
    response->header_count = 0;
}

void solar_os_http_buffered_response_clear(
    solar_os_http_buffered_response_t *response)
{
    if (response == NULL) {
        return;
    }
    solar_os_http_buffered_headers_clear(response);
    solar_os_memory_free(response->headers);
    solar_os_memory_free(response->body);
    memset(response, 0, sizeof(*response));
}

static esp_err_t solar_os_http_buffered_store_header(
    solar_os_http_buffered_collector_t *collector,
    const solar_os_http_event_t *event)
{
    solar_os_http_buffered_response_t *response = collector->response;
    if (event->header_name == NULL || event->header_value == NULL) {
        return ESP_OK;
    }
    if (collector->follow_redirects &&
        event->status_code >= 300 && event->status_code < 400) {
        return ESP_OK;
    }

    const size_t name_len = strlen(event->header_name);
    const size_t value_len = strlen(event->header_value);
    const size_t stored_bytes = name_len + value_len + 2U;
    if (response->header_count >= SOLAR_OS_HTTP_BUFFERED_MAX_HEADERS ||
        stored_bytes > SOLAR_OS_HTTP_BUFFERED_MAX_HEADER_BYTES -
            collector->header_bytes) {
        response->headers_truncated = true;
        return ESP_OK;
    }

    char *storage = solar_os_memory_alloc(stored_bytes,
                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                          "http.header");
    if (storage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(storage, event->header_name, name_len + 1U);
    memcpy(storage + name_len + 1U, event->header_value, value_len + 1U);

    solar_os_http_response_header_t *header =
        &response->headers[response->header_count++];
    header->name = storage;
    header->value = storage + name_len + 1U;
    collector->header_bytes += stored_bytes;
    return ESP_OK;
}

static esp_err_t solar_os_http_buffered_event(
    const solar_os_http_event_t *event,
    void *user_data)
{
    solar_os_http_buffered_collector_t *collector = user_data;
    if (collector == NULL || event == NULL) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) {
        err = solar_os_http_buffered_store_header(collector, event);
    } else if (event->type == SOLAR_OS_HTTP_EVENT_DATA && event->data_len > 0) {
        solar_os_http_buffered_response_t *response = collector->response;
        const size_t remaining = collector->body_capacity - response->body_len;
        const size_t copy_len = event->data_len < remaining ?
            event->data_len : remaining;
        if (copy_len > 0) {
            memcpy(response->body + response->body_len, event->data, copy_len);
            response->body_len += copy_len;
        }
        if (copy_len < event->data_len) {
            response->body_truncated = true;
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    if (err == ESP_OK && collector->chained_handler != NULL) {
        err = collector->chained_handler(event, collector->chained_user_data);
    }
    if (err != ESP_OK) {
        collector->error = err;
    }
    return err;
}

esp_err_t solar_os_http_perform_buffered(
    const solar_os_http_request_options_t *options,
    size_t max_body_bytes,
    solar_os_http_buffered_response_t *response)
{
    if (options == NULL || response == NULL ||
        max_body_bytes > SOLAR_OS_HTTP_BUFFERED_MAX_BODY) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(response, 0, sizeof(*response));
    response->response.status_code = -1;
    response->response.content_length = -1;
    response->headers = solar_os_memory_calloc(
        SOLAR_OS_HTTP_BUFFERED_MAX_HEADERS,
        sizeof(*response->headers),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "http.headers");
    if (response->headers == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (max_body_bytes > 0) {
        response->body = solar_os_memory_alloc(max_body_bytes,
                                                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                "http.body");
        if (response->body == NULL) {
            solar_os_http_buffered_response_clear(response);
            return ESP_ERR_NO_MEM;
        }
    }

    solar_os_http_buffered_collector_t collector = {
        .response = response,
        .body_capacity = max_body_bytes,
        .follow_redirects = options->follow_redirects,
        .chained_handler = options->event_handler,
        .chained_user_data = options->user_data,
    };
    solar_os_http_request_options_t buffered_options = *options;
    buffered_options.event_handler = solar_os_http_buffered_event;
    buffered_options.user_data = &collector;

    solar_os_http_request_t *request = NULL;
    esp_err_t err = solar_os_http_request_create(&buffered_options, &request);
    if (err == ESP_OK) {
        err = solar_os_http_request_perform(request, &response->response);
    }
    const esp_err_t destroy_err = solar_os_http_request_destroy(request);
    if (err == ESP_OK && destroy_err != ESP_OK) {
        err = destroy_err;
    }
    if (response->body_truncated && err == ESP_ERR_INVALID_SIZE &&
        collector.error == ESP_ERR_INVALID_SIZE) {
        err = ESP_OK;
    }
    return err;
}

static void solar_os_http_session_release_global(void)
{
    portENTER_CRITICAL(&http_session_global_lock);
    if (http_session_global_count > 0) {
        http_session_global_count--;
    }
    portEXIT_CRITICAL(&http_session_global_lock);
}

static bool solar_os_http_origin_length(const char *origin, size_t *out_length)
{
    if (origin == NULL || out_length == NULL) {
        return false;
    }
    const char *authority = NULL;
    if (strncmp(origin, "https://", 8) == 0) {
        authority = origin + 8;
    } else if (strncmp(origin, "http://", 7) == 0) {
        authority = origin + 7;
    } else {
        return false;
    }
    if (*authority == '\0') {
        return false;
    }

    const char *end = authority;
    while (*end != '\0' && *end != '/' && *end != '?' && *end != '#') {
        if (*end == '@') {
            return false;
        }
        end++;
    }
    if (end == authority ||
        (*end != '\0' && !(*end == '/' && end[1] == '\0'))) {
        return false;
    }

    size_t length = (size_t)(end - origin);
    if (length == 0 || length >= SOLAR_OS_HTTP_SESSION_ORIGIN_MAX) {
        return false;
    }
    *out_length = length;
    return true;
}

static bool solar_os_http_url_matches_origin(const char *url,
                                              const char *origin)
{
    if (url == NULL || origin == NULL) {
        return false;
    }
    const size_t origin_len = strlen(origin);
    if (strncasecmp(url, origin, origin_len) != 0) {
        return false;
    }
    const char boundary = url[origin_len];
    return boundary == '\0' || boundary == '/' || boundary == '?' ||
        boundary == '#';
}

static solar_os_http_session_connection_t *solar_os_http_session_find(
    solar_os_http_session_context_t *context,
    uint32_t handle,
    size_t *out_index)
{
    if (context == NULL || handle == 0) {
        return NULL;
    }
    const uint32_t adjusted = handle - 1U;
    const size_t index = adjusted % SOLAR_OS_HTTP_SESSION_MAX_HANDLES;
    const uint32_t generation =
        adjusted / SOLAR_OS_HTTP_SESSION_MAX_HANDLES + 1U;
    if (context->generations[index] != generation) {
        return NULL;
    }
    if (out_index != NULL) {
        *out_index = index;
    }
    return context->connections[index];
}

static void solar_os_http_session_connection_destroy(
    solar_os_http_session_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    if (connection->client != NULL) {
        (void)esp_http_client_cleanup(connection->client);
    }
    if (connection->lock != NULL) {
        vSemaphoreDelete(connection->lock);
    }
    solar_os_memory_free(connection->origin);
    solar_os_memory_free(connection->host_header);
    solar_os_memory_free(connection->user_agent);
    solar_os_memory_free(connection);
    solar_os_http_session_release_global();
}

static esp_err_t solar_os_http_session_finish_unauthorized(
    solar_os_http_request_t *request,
    esp_http_client_handle_t client)
{
    uint8_t discard[SOLAR_OS_HTTP_STREAM_CHUNK];
    request->response_started = true;
    while (!esp_http_client_is_complete_data_received(client)) {
        if (request->event_error != ESP_OK) {
            return request->event_error;
        }
        if (solar_os_http_cancelled(request)) {
            return ESP_ERR_INVALID_STATE;
        }
        const int read = esp_http_client_read(client,
                                              (char *)discard,
                                              sizeof(discard));
        if (read < 0) {
            return ESP_FAIL;
        }
        if (read == 0 && !esp_http_client_is_complete_data_received(client)) {
            if (esp_http_client_get_content_length(client) >= 0 ||
                esp_http_client_is_chunked_response(client)) {
                return ESP_ERR_HTTP_INCOMPLETE_DATA;
            }
            break;
        }
    }
    if (request->event_error != ESP_OK) {
        return request->event_error;
    }
    return solar_os_http_deliver_response(
        request,
        esp_http_client_get_status_code(client),
        esp_http_client_get_content_length(client));
}

static esp_err_t solar_os_http_session_request_perform(
    solar_os_http_session_connection_t *connection,
    solar_os_http_request_t *request,
    solar_os_http_response_t *response)
{
    if (connection == NULL || request == NULL || response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(response, 0, sizeof(*response));
    response->status_code = -1;
    response->content_length = -1;

    xSemaphoreTake(request->lock, portMAX_DELAY);
    if (request->active || request->performed ||
        solar_os_http_cancelled(request)) {
        xSemaphoreGive(request->lock);
        response->cancelled = solar_os_http_cancelled(request);
        return ESP_ERR_INVALID_STATE;
    }
    request->active = true;
    request->performed = true;
    request->persistent_perform = true;
    request->client = connection->client;
    xSemaphoreGive(request->lock);

    request->started_us = esp_timer_get_time();
    request->deadline_us = request->options.deadline_ms != 0 ?
        request->started_us + ((int64_t)request->options.deadline_ms * 1000) : 0;

    esp_err_t err = esp_http_client_set_user_data(connection->client, request);
    if (err == ESP_OK) {
        err = esp_http_client_set_url(connection->client, request->options.url);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_method(
            connection->client,
            solar_os_http_esp_method(request->options.method));
    }
    if (err == ESP_OK) {
        err = solar_os_http_apply_timeout(request, connection->client);
    }
    if (err == ESP_OK) {
        err = esp_http_client_delete_all_headers(connection->client);
    }
    if (err == ESP_OK && connection->user_agent != NULL) {
        err = esp_http_client_set_header(connection->client,
                                         "User-Agent",
                                         connection->user_agent);
    }
    for (size_t i = 0; err == ESP_OK && i < request->options.header_count; i++) {
        err = esp_http_client_set_header(connection->client,
                                         request->options.headers[i].name,
                                         request->options.headers[i].value);
    }
    if (err == ESP_OK) {
        /* Keep the TLS peer and HTTP virtual host aligned even if a caller
         * supplied its own Host header. */
        err = esp_http_client_set_header(connection->client,
                                         "Host",
                                         connection->host_header);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(
            connection->client,
            request->options.body_len > 0 ? request->options.body : NULL,
            (int)request->options.body_len);
        /* ESP-IDF clears the stale body before it tries to remove the default
         * Content-Type header. A bodyless request has no such header after
         * delete_all_headers(), so NOT_FOUND is the successful empty state. */
        if (err == ESP_ERR_NOT_FOUND && request->options.body_len == 0) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = esp_http_client_reset_redirect_counter(connection->client);
    }

    const bool retryable = request->options.method == SOLAR_OS_HTTP_METHOD_GET ||
        request->options.method == SOLAR_OS_HTTP_METHOD_HEAD;
    bool force_close = false;
    for (unsigned attempt = 0; err == ESP_OK && attempt < 2U; attempt++) {
        err = esp_http_client_perform(connection->client);
        if (err == ESP_ERR_NOT_SUPPORTED &&
            esp_http_client_get_status_code(connection->client) == 401) {
            err = solar_os_http_session_finish_unauthorized(
                request,
                connection->client);
            force_close = true;
        }
        if (err == ESP_OK || !retryable || attempt != 0U ||
            request->response_started || request->bytes_received != 0U ||
            solar_os_http_cancelled(request) ||
            solar_os_http_remaining_ms(request) == 0) {
            break;
        }
        (void)esp_http_client_close(connection->client);
        request->event_error = ESP_OK;
        err = solar_os_http_apply_timeout(request, connection->client);
    }

    const int64_t finished_us = esp_timer_get_time();
    (void)solar_os_http_remaining_ms(request);
    if (solar_os_http_cancelled(request)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (request->deadline_exceeded) {
        err = ESP_ERR_TIMEOUT;
    } else if (request->event_error != ESP_OK) {
        err = request->event_error;
    }

    if (request->response_started) {
        response->status_code = esp_http_client_get_status_code(connection->client);
        response->content_length =
            esp_http_client_get_content_length(connection->client);
    }
    response->bytes_received = request->bytes_received;
    response->duration_ms = finished_us > request->started_us ?
        (uint32_t)((finished_us - request->started_us + 999) / 1000) : 0;
    response->cancelled = solar_os_http_cancelled(request);
    response->deadline_exceeded = request->deadline_exceeded;

    if (err != ESP_OK || force_close) {
        (void)esp_http_client_close(connection->client);
    }
    (void)esp_http_client_clear_response_buffer(connection->client);
    (void)esp_http_client_set_user_data(connection->client, NULL);

    xSemaphoreTake(request->lock, portMAX_DELAY);
    request->client = NULL;
    request->active = false;
    xSemaphoreGive(request->lock);
    return err;
}

static void solar_os_http_session_task(void *arg)
{
    solar_os_http_session_work_t *work = arg;
    work->result = solar_os_http_session_request_perform(work->connection,
                                                         work->request,
                                                         work->response);
    work->done = true;
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_http_session_context_create(
    solar_os_http_cancel_fn should_cancel,
    void *cancel_user_data,
    solar_os_http_session_context_t **out_context)
{
    if (out_context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_context = solar_os_memory_calloc(
        1,
        sizeof(**out_context),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "http.session.context");
    if (*out_context == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (*out_context)->should_cancel = should_cancel;
    (*out_context)->cancel_user_data = cancel_user_data;
    return ESP_OK;
}

void solar_os_http_session_context_destroy(
    solar_os_http_session_context_t *context)
{
    if (context == NULL) {
        return;
    }
    solar_os_http_session_close_all(context);
    solar_os_memory_free(context);
}

esp_err_t solar_os_http_session_open(
    solar_os_http_session_context_t *context,
    const char *origin,
    const char *user_agent,
    uint32_t *out_handle)
{
    size_t origin_len = 0;
    if (context == NULL || out_handle == NULL ||
        !solar_os_http_origin_length(origin, &origin_len) ||
        (user_agent != NULL &&
         strlen(user_agent) >= SOLAR_OS_HTTP_SESSION_USER_AGENT_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = 0;

    size_t index = SOLAR_OS_HTTP_SESSION_MAX_HANDLES;
    portENTER_CRITICAL(&http_session_global_lock);
    if (http_session_global_count < SOLAR_OS_HTTP_SESSION_GLOBAL_MAX_HANDLES) {
        for (size_t i = 0; i < SOLAR_OS_HTTP_SESSION_MAX_HANDLES; i++) {
            if (context->connections[i] == NULL) {
                index = i;
                http_session_global_count++;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&http_session_global_lock);
    if (index == SOLAR_OS_HTTP_SESSION_MAX_HANDLES) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_http_session_connection_t *connection = solar_os_memory_calloc(
        1,
        sizeof(*connection),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL,
        "http.session");
    if (connection == NULL) {
        solar_os_http_session_release_global();
        return ESP_ERR_NO_MEM;
    }
    connection->origin = solar_os_memory_alloc(origin_len + 1U,
                                                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                "http.session.origin");
    if (connection->origin != NULL) {
        memcpy(connection->origin, origin, origin_len);
        connection->origin[origin_len] = '\0';
    }
    const char *authority = strstr(origin, "://");
    authority = authority != NULL ? authority + 3 : origin;
    const size_t authority_len = origin_len - (size_t)(authority - origin);
    connection->host_header = solar_os_memory_alloc(
        authority_len + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "http.session.host");
    if (connection->host_header != NULL) {
        memcpy(connection->host_header, authority, authority_len);
        connection->host_header[authority_len] = '\0';
    }
    if (user_agent != NULL) {
        const size_t length = strlen(user_agent) + 1U;
        connection->user_agent = solar_os_memory_alloc(
            length,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "http.session.agent");
        if (connection->user_agent != NULL) {
            memcpy(connection->user_agent, user_agent, length);
        }
    }
    connection->lock = xSemaphoreCreateMutexStatic(&connection->lock_storage);
    if (connection->origin == NULL || connection->host_header == NULL ||
        (user_agent != NULL && connection->user_agent == NULL) ||
        connection->lock == NULL) {
        solar_os_http_session_connection_destroy(connection);
        return ESP_ERR_NO_MEM;
    }

    const esp_http_client_config_t config = {
        .url = connection->origin,
        .user_agent = connection->user_agent,
        .timeout_ms = (int)SOLAR_OS_HTTP_DEFAULT_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .event_handler = solar_os_http_event_bridge,
        .buffer_size = (int)SOLAR_OS_HTTP_SESSION_RX_BUFFER,
        .buffer_size_tx = (int)SOLAR_OS_HTTP_SESSION_TX_BUFFER,
    #if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
    #endif
    };
    connection->client = esp_http_client_init(&config);
    if (connection->client == NULL) {
        solar_os_http_session_connection_destroy(connection);
        return ESP_ERR_NO_MEM;
    }

    context->generations[index]++;
    if (context->generations[index] == 0 ||
        context->generations[index] >
            UINT32_MAX / SOLAR_OS_HTTP_SESSION_MAX_HANDLES) {
        context->generations[index] = 1;
    }
    context->connections[index] = connection;
    *out_handle = (context->generations[index] - 1U) *
        SOLAR_OS_HTTP_SESSION_MAX_HANDLES + (uint32_t)index + 1U;
    return ESP_OK;
}

esp_err_t solar_os_http_session_request(
    solar_os_http_session_context_t *context,
    uint32_t handle,
    const solar_os_http_request_options_t *options,
    size_t max_body_bytes,
    solar_os_http_buffered_response_t *response)
{
    solar_os_http_session_connection_t *connection =
        solar_os_http_session_find(context, handle, NULL);
    if (connection == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (options == NULL || response == NULL || options->url == NULL ||
        options->follow_redirects ||
        !solar_os_http_url_matches_origin(options->url, connection->origin) ||
        max_body_bytes > SOLAR_OS_HTTP_BUFFERED_MAX_BODY ||
        options->timeout_ms > SOLAR_OS_HTTP_SESSION_MAX_TIMEOUT_MS ||
        options->deadline_ms > SOLAR_OS_HTTP_SESSION_MAX_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_http_request_options_t persistent_options = *options;
    persistent_options.user_agent = connection->user_agent;
    persistent_options.should_cancel = context->should_cancel;
    persistent_options.cancel_user_data = context->cancel_user_data;
    int transmit_buffer_size = 0;
    esp_err_t err = solar_os_http_transmit_buffer_size(&persistent_options,
                                                       &transmit_buffer_size);
    if (err != ESP_OK || transmit_buffer_size > SOLAR_OS_HTTP_SESSION_TX_BUFFER) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    memset(response, 0, sizeof(*response));
    response->response.status_code = -1;
    response->response.content_length = -1;
    response->headers = solar_os_memory_calloc(
        SOLAR_OS_HTTP_BUFFERED_MAX_HEADERS,
        sizeof(*response->headers),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "http.headers");
    if (response->headers == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (max_body_bytes > 0) {
        response->body = solar_os_memory_alloc(max_body_bytes,
                                                SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                "http.body");
        if (response->body == NULL) {
            solar_os_http_buffered_response_clear(response);
            return ESP_ERR_NO_MEM;
        }
    }

    solar_os_http_buffered_collector_t collector = {
        .response = response,
        .body_capacity = max_body_bytes,
        .chained_handler = options->event_handler,
        .chained_user_data = options->user_data,
    };
    persistent_options.event_handler = solar_os_http_buffered_event;
    persistent_options.user_data = &collector;

    solar_os_http_request_t *request = NULL;
    err = solar_os_http_request_create(&persistent_options, &request);
    if (err != ESP_OK) {
        solar_os_http_buffered_response_clear(response);
        return err;
    }

    xSemaphoreTake(connection->lock, portMAX_DELAY);
    if (connection->closing || connection->active_request != NULL) {
        xSemaphoreGive(connection->lock);
        (void)solar_os_http_request_destroy(request);
        solar_os_http_buffered_response_clear(response);
        return ESP_ERR_INVALID_STATE;
    }
    connection->active_request = request;
    xSemaphoreGive(connection->lock);

    solar_os_http_session_work_t work = {
        .connection = connection,
        .request = request,
        .response = &response->response,
        .result = ESP_FAIL,
    };
    const BaseType_t created = solar_os_task_create_pinned_internal(
        solar_os_http_session_task,
        "http_session",
        SOLAR_OS_HTTP_SESSION_TASK_STACK,
        &work,
        SOLAR_OS_HTTP_SESSION_TASK_PRIORITY,
        NULL,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created == pdPASS) {
        bool abort_sent = false;
        while (!work.done) {
            if (!abort_sent && request->event_error != ESP_OK) {
                solar_os_http_request_abort(request);
                abort_sent = true;
            } else if (!abort_sent && solar_os_http_cancelled(request)) {
                (void)solar_os_http_request_cancel(request);
                abort_sent = true;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        err = work.result;
    } else {
        err = ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(connection->lock, portMAX_DELAY);
    connection->active_request = NULL;
    xSemaphoreGive(connection->lock);
    const esp_err_t destroy_err = solar_os_http_request_destroy(request);
    if (err == ESP_OK && destroy_err != ESP_OK) {
        err = destroy_err;
    }
    if (response->body_truncated && collector.error == ESP_ERR_INVALID_SIZE &&
        (err == ESP_ERR_INVALID_SIZE || err == ESP_FAIL)) {
        err = ESP_OK;
    }
    return err;
}

esp_err_t solar_os_http_session_close(
    solar_os_http_session_context_t *context,
    uint32_t handle)
{
    size_t index = 0;
    solar_os_http_session_connection_t *connection =
        solar_os_http_session_find(context, handle, &index);
    if (connection == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    context->connections[index] = NULL;

    xSemaphoreTake(connection->lock, portMAX_DELAY);
    connection->closing = true;
    solar_os_http_request_t *active = connection->active_request;
    if (active != NULL) {
        (void)solar_os_http_request_cancel(active);
    }
    xSemaphoreGive(connection->lock);

    for (uint32_t waited = 0; active != NULL &&
         waited < SOLAR_OS_HTTP_SESSION_CLOSE_WAIT_MS; waited += 10U) {
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(connection->lock, portMAX_DELAY);
        active = connection->active_request;
        xSemaphoreGive(connection->lock);
    }
    if (active != NULL) {
        return ESP_ERR_TIMEOUT;
    }
    solar_os_http_session_connection_destroy(connection);
    return ESP_OK;
}

void solar_os_http_session_close_all(
    solar_os_http_session_context_t *context)
{
    if (context == NULL) {
        return;
    }
    for (size_t i = 0; i < SOLAR_OS_HTTP_SESSION_MAX_HANDLES; i++) {
        solar_os_http_session_connection_t *connection = context->connections[i];
        if (connection == NULL) {
            continue;
        }
        const uint32_t handle = (context->generations[i] - 1U) *
            SOLAR_OS_HTTP_SESSION_MAX_HANDLES + (uint32_t)i + 1U;
        (void)solar_os_http_session_close(context, handle);
    }
}
