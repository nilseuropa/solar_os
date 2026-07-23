#include "solar_os_http_client.h"

#include <limits.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "solar_os_memory.h"

#define SOLAR_OS_HTTP_DEFAULT_TIMEOUT_MS 10000U
#define SOLAR_OS_HTTP_DEFAULT_RX_BUFFER 1024U
#define SOLAR_OS_HTTP_DEFAULT_TX_BUFFER 512U
#define SOLAR_OS_HTTP_DEFAULT_MAX_REDIRECTS 10U
#define SOLAR_OS_HTTP_STREAM_CHUNK 1024U

struct solar_os_http_request {
    solar_os_http_request_options_t options;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    esp_http_client_handle_t client;
    volatile bool cancel_requested;
    bool active;
    bool performed;
    bool deadline_exceeded;
    esp_err_t event_error;
    int64_t started_us;
    int64_t deadline_us;
    uint64_t bytes_received;
};

static bool solar_os_http_method_valid(solar_os_http_method_t method)
{
    return method >= SOLAR_OS_HTTP_METHOD_GET && method <= SOLAR_OS_HTTP_METHOD_HEAD;
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

static esp_err_t solar_os_http_apply_timeout(solar_os_http_request_t *request,
                                             esp_http_client_handle_t client)
{
    int timeout_ms = request->options.timeout_ms != 0 ?
        (int)request->options.timeout_ms : (int)SOLAR_OS_HTTP_DEFAULT_TIMEOUT_MS;
    const int remaining_ms = solar_os_http_remaining_ms(request);
    if (remaining_ms == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (remaining_ms < timeout_ms) {
        timeout_ms = remaining_ms;
    }
    return esp_http_client_set_timeout_ms(client, timeout_ms);
}

static esp_err_t solar_os_http_event_bridge(esp_http_client_event_t *esp_event)
{
    if (esp_event == NULL || esp_event->user_data == NULL) {
        return ESP_OK;
    }

    solar_os_http_request_t *request = esp_event->user_data;
    if (request->cancel_requested) {
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

    if (request->options.event_handler == NULL ||
        esp_event->event_id != HTTP_EVENT_ON_HEADER) {
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
    if (request->cancel_requested) {
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

        if (request->options.method == SOLAR_OS_HTTP_METHOD_HEAD) {
            return ESP_OK;
        }

        while (true) {
            err = solar_os_http_abort_error(request);
            if (err != ESP_OK) {
                return err;
            }
            err = solar_os_http_apply_timeout(request, client);
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
    if (request->active || request->performed || request->cancel_requested) {
        xSemaphoreGive(request->lock);
        if (response != NULL) {
            response->cancelled = request->cancel_requested;
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

    esp_http_client_config_t config = {
        .url = request->options.url,
        .method = solar_os_http_esp_method(request->options.method),
        .user_agent = request->options.user_agent,
        .timeout_ms = (int)initial_timeout,
        .disable_auto_redirect = true,
        .event_handler = solar_os_http_event_bridge,
        .buffer_size = request->options.receive_buffer_size != 0 ?
            (int)request->options.receive_buffer_size : (int)SOLAR_OS_HTTP_DEFAULT_RX_BUFFER,
        .buffer_size_tx = request->options.transmit_buffer_size != 0 ?
            (int)request->options.transmit_buffer_size : (int)SOLAR_OS_HTTP_DEFAULT_TX_BUFFER,
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
    const bool cancelled = request->cancel_requested;
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

    if (request->cancel_requested) {
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
        response->cancelled = request->cancel_requested;
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
