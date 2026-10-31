#pragma once
#include <stddef.h>
#include <stdint.h>
#include "solar_os_http_client.h"

#define SOLAR_OS_DOWNLOAD_BUFFER_BYTES 4096U
typedef void (*solar_os_download_progress_fn)(size_t received, size_t total, void *user);
typedef struct {
    size_t bytes;
    uint8_t sha256[32];
    solar_os_http_response_t response;
} solar_os_download_result_t;

/* Download into an exclusively caller-owned staging path. Removes that path
 * on failure. Memory is bounded independently of the response size. A nonzero
 * expected_size requires an exact match; max_size is always enforced. An
 * optional expected_sha256 is 64 hex digits. Stored bytes are read back and
 * verified before success. Options' event handler is replaced; cancellation,
 * redirects and deadlines retain their normal HTTP semantics. */
esp_err_t solar_os_download_file(const solar_os_http_request_options_t *options,
    const char *staged_path, size_t max_size, size_t expected_size,
    const char *expected_sha256, solar_os_download_progress_fn progress,
    void *progress_user, solar_os_download_result_t *result);
