#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_tls.h"
#include "solar_os_memory.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "mbedtls/ssl.h"

struct esp_tls {
    unsigned attempts;
    const void *pending_write;
    size_t pending_size;
};
static unsigned tls_instances, allocations;
static unsigned handshake_attempts = 2;
static int64_t now_us, cancel_at_us;
static bool reject_peer, wait_reads, wait_writes, eof;
static bool cancelled(void *user)
{ assert(user == &cancel_at_us); return cancel_at_us && now_us >= cancel_at_us; }

int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(uint32_t ticks) { now_us += (int64_t)ticks * 1000; }
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "tls.connection"));
    void *p = malloc(size);
    if (p) ++allocations;
    return p;
}

void solar_os_memory_free(void *p)
{ if (p) { --allocations; free(p); } }

esp_tls_t *esp_tls_init(void)
{
    esp_tls_t *state = calloc(1U, sizeof(*state));
    if (state) ++tls_instances;
    return state;
}

int esp_tls_conn_destroy(esp_tls_t *state)
{ assert(state); --tls_instances; free(state); return 0; }

void *esp_tls_get_ssl_context(esp_tls_t *state) { return state; }
int mbedtls_ssl_get_max_out_record_payload(const mbedtls_ssl_context *state)
{ assert(state); return 4; }

int esp_tls_conn_new_async(const char *host, int length, int port, const esp_tls_cfg_t *cfg, esp_tls_t *state)
{
    assert(!strcmp(host, "trusted.test") && length == 12 && port == 443);
    assert(cfg->crt_bundle_attach == esp_crt_bundle_attach && cfg->non_block && cfg->timeout_ms > 0);
    if (reject_peer) return -1;
    return ++state->attempts >= handshake_attempts;
}

ssize_t esp_tls_conn_read(esp_tls_t *state, void *data, size_t length)
{
    assert(state && length >= 3);
    if (wait_reads) return ESP_TLS_ERR_SSL_WANT_READ;
    if (eof) return 0;
    memcpy(data, "tls", 3);
    return 3;
}

ssize_t esp_tls_conn_write(esp_tls_t *state, const void *data, size_t length)
{
    assert(state && data && length && length <= 4);
    if (state->pending_write) assert(data == state->pending_write && length == state->pending_size);
    if (wait_writes) {
        state->pending_write = data;
        state->pending_size = length;
        return ESP_TLS_ERR_SSL_WANT_WRITE;
    }
    return length > 2 ? 2 : (ssize_t)length;
}

int main(void)
{
    solar_os_tls_connection_t *connection;
    assert(solar_os_tls_connect("trusted.test", 443, 20, NULL, NULL, &connection) == ESP_ERR_TIMEOUT);
    assert(!connection && !tls_instances && !allocations);
    reject_peer = true;
    assert(solar_os_tls_connect("trusted.test", 443, 60, NULL, NULL, &connection) == ESP_FAIL);
    assert(!connection && !tls_instances && !allocations);
    reject_peer = false;
    handshake_attempts = 10;
    cancel_at_us = now_us + 20000;
    assert(solar_os_tls_connect("trusted.test", 443, 60, cancelled, &cancel_at_us, &connection) == ESP_ERR_INVALID_STATE);
    assert(!connection && !tls_instances && !allocations);
    handshake_attempts = 2;
    cancel_at_us = 0;
    assert(!solar_os_tls_connect("trusted.test", 443, 60, cancelled, &cancel_at_us, &connection));

    size_t transferred;
    char data[8];
    wait_reads = true;
    assert(solar_os_tls_read(connection, data, sizeof(data), 0, NULL, NULL, &transferred) == ESP_ERR_TIMEOUT);
    assert(!transferred && tls_instances == 1);
    wait_reads = false;
    assert(!solar_os_tls_read(connection, data, sizeof(data), 60, NULL, NULL, &transferred));
    assert(transferred == 3 && !memcmp(data, "tls", 3));
    assert(!solar_os_tls_write(connection, "hello", 5, 60, NULL, NULL, &transferred) && transferred == 2);
    eof = true;
    assert(!solar_os_tls_read(connection, data, sizeof(data), 60, NULL, NULL, &transferred) && !transferred);
    wait_writes = true;
    cancel_at_us = now_us + 40000;
    assert(solar_os_tls_write(connection, "must stay alive", 15, 60, cancelled, &cancel_at_us, &transferred) ==
        ESP_ERR_INVALID_STATE);
    assert(!transferred && !tls_instances);
    assert(solar_os_tls_write(connection, "different bytes", 15, 60, NULL, NULL, &transferred) == ESP_ERR_INVALID_STATE);
    solar_os_tls_close(connection);
    assert(!tls_instances && !allocations);
    return 0;
}
