#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct solar_os_tls_connection solar_os_tls_connection_t;
typedef bool (*solar_os_tls_cancel_fn)(void *user);

/* The hostname is sent for SNI and verified against the firmware trust
 * bundle. Callbacks and buffers are borrowed for the duration of the call.
 * Serialize operations on each connection. */
esp_err_t solar_os_tls_connect(const char *host, uint16_t port, uint32_t timeout_ms,
    solar_os_tls_cancel_fn cancel, void *user, solar_os_tls_connection_t **connection);
/* One transfer may report partial progress. A read of zero bytes is EOF.
 * A failed write while TLS is waiting for the same record retires the
 * transport. Close the connection before starting a new one. */
esp_err_t solar_os_tls_read(solar_os_tls_connection_t *connection, void *buffer,
    size_t size, uint32_t timeout_ms, solar_os_tls_cancel_fn cancel, void *user, size_t *transferred);
esp_err_t solar_os_tls_write(solar_os_tls_connection_t *connection, const void *buffer,
    size_t size, uint32_t timeout_ms, solar_os_tls_cancel_fn cancel, void *user, size_t *transferred);
void solar_os_tls_close(solar_os_tls_connection_t *connection);
