#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Runtime-only raw TCP endpoints exposed as exclusive duplex byte streams.
 * Reads and writes are nonblocking. Release the stream before removing it. */
esp_err_t solar_os_tcp_stream_listen(const char *name, uint16_t port);
/* Resolve the IPv4 destination now; each stream claim starts a fresh,
 * nonblocking outbound connection. No peer is replaced within a claim. */
esp_err_t solar_os_tcp_stream_connect(const char *name, const char *host, uint16_t port);
esp_err_t solar_os_tcp_stream_remove(const char *name);
