#include "solar_os_tls.h"

#include <limits.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ssl.h"
#include "solar_os_memory.h"

#define TLS_HOST_MAX 255U
#define TLS_TIMEOUT_MAX_MS 60000U
#define TLS_RETRY_MS 20U
#define TLS_CONNECT_POLL_MAX_MS 1000U

struct solar_os_tls_connection {
    esp_tls_t *tls;
    size_t record_bytes;
};

static void tls_wait(int64_t deadline)
{
    const int64_t left_us = deadline - esp_timer_get_time();
    if (left_us <= 0) return;
    uint32_t wait_ms = (uint32_t)((left_us + 999) / 1000);
    if (wait_ms > TLS_RETRY_MS) wait_ms = TLS_RETRY_MS;
    TickType_t ticks = pdMS_TO_TICKS(wait_ms);
    vTaskDelay(ticks ? ticks : 1);
}

static void tls_disconnect(solar_os_tls_connection_t *connection)
{
    if (connection->tls) {
        (void)esp_tls_conn_destroy(connection->tls);
        connection->tls = NULL;
    }
}

void solar_os_tls_close(solar_os_tls_connection_t *connection)
{
    if (!connection) return;
    tls_disconnect(connection);
    solar_os_memory_free(connection);
}

esp_err_t solar_os_tls_connect(const char *host, uint16_t port, uint32_t timeout_ms,
    solar_os_tls_cancel_fn cancel, void *user, solar_os_tls_connection_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!host || !*host || !port || !timeout_ms || timeout_ms > TLS_TIMEOUT_MAX_MS)
        return ESP_ERR_INVALID_ARG;
    const size_t host_length = strnlen(host, TLS_HOST_MAX + 1U);
    if (host_length > TLS_HOST_MAX) return ESP_ERR_INVALID_ARG;
    if (cancel && cancel(user)) return ESP_ERR_INVALID_STATE;

    solar_os_tls_connection_t *connection = solar_os_memory_alloc(sizeof(*connection),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "tls.connection");
    if (!connection) return ESP_ERR_NO_MEM;
    connection->tls = esp_tls_init();
    if (!connection->tls) {
        solar_os_tls_close(connection);
        return ESP_ERR_NO_MEM;
    }

    const esp_tls_cfg_t options = {.crt_bundle_attach = esp_crt_bundle_attach,
                                   .non_block = true,
                                   .timeout_ms = (int)(timeout_ms < TLS_CONNECT_POLL_MAX_MS ?
                                                       timeout_ms : TLS_CONNECT_POLL_MAX_MS)};
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t error = ESP_ERR_TIMEOUT;
    while (esp_timer_get_time() < deadline) {
        if (cancel && cancel(user)) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
        const int connected = esp_tls_conn_new_async(host, (int)host_length, port, &options, connection->tls);
        if (connected < 0) {
            error = ESP_FAIL;
            break;
        }
        if (connected == 1) {
            if (cancel && cancel(user)) {
                error = ESP_ERR_INVALID_STATE;
                break;
            }
            if (esp_timer_get_time() >= deadline) break;
            const mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(connection->tls);
            const int record_bytes = ssl ? mbedtls_ssl_get_max_out_record_payload(ssl) : 0;
            if (record_bytes <= 0) {
                error = ESP_FAIL;
                break;
            }
            connection->record_bytes = (size_t)record_bytes;
            *out = connection;
            return ESP_OK;
        }
        tls_wait(deadline);
    }

    solar_os_tls_close(connection);
    return error;
}

static esp_err_t tls_io(solar_os_tls_connection_t *connection, void *buffer, size_t size,
    uint32_t timeout_ms, bool write, solar_os_tls_cancel_fn cancel, void *user, size_t *transferred)
{
    if (!transferred) return ESP_ERR_INVALID_ARG;
    *transferred = 0;
    if (!connection || (!buffer && size) || size > INT_MAX || timeout_ms > TLS_TIMEOUT_MAX_MS)
        return ESP_ERR_INVALID_ARG;
    if (!connection->tls || (cancel && cancel(user))) return ESP_ERR_INVALID_STATE;
    if (!size) return ESP_OK;
    /* Keep each write within one record so a positive result does not leave
     * a following record pending in the SDK. */
    if (write && size > connection->record_bytes) size = connection->record_bytes;

    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    esp_err_t error = ESP_ERR_TIMEOUT;
    do {
        const ssize_t count = write ? esp_tls_conn_write(connection->tls, buffer, size) :
            esp_tls_conn_read(connection->tls, buffer, size);
        if (count >= 0 && (!write || count > 0) && (size_t)count <= size) {
            *transferred = (size_t)count;
            return ESP_OK;
        }
        if (count != ESP_TLS_ERR_SSL_WANT_READ && count != ESP_TLS_ERR_SSL_WANT_WRITE) {
            error = ESP_FAIL;
            break;
        }
        tls_wait(deadline);
        if (cancel && cancel(user)) {
            error = ESP_ERR_INVALID_STATE;
            break;
        }
    } while (esp_timer_get_time() < deadline);

    if (write || error == ESP_FAIL) tls_disconnect(connection);
    return error;
}

esp_err_t solar_os_tls_read(solar_os_tls_connection_t *connection, void *buffer, size_t size,
    uint32_t timeout_ms, solar_os_tls_cancel_fn cancel, void *user, size_t *transferred)
{
    return tls_io(connection, buffer, size, timeout_ms, false, cancel, user, transferred);
}

esp_err_t solar_os_tls_write(solar_os_tls_connection_t *connection, const void *buffer, size_t size,
    uint32_t timeout_ms, solar_os_tls_cancel_fn cancel, void *user, size_t *transferred)
{
    return tls_io(connection, (void *)buffer, size, timeout_ms, true, cancel, user, transferred);
}
