#include "solar_os_download.h"
#include <stdio.h>
#include <string.h>
#include "solar_os_crypto.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_work.h"

typedef struct {
    FILE *file;
    uint8_t *buffer;
    size_t pending, received, maximum, expected, total;
    solar_os_crypto_sha256_t hash;
    solar_os_download_progress_fn progress;
    void *user;
    solar_os_work_budget_t work;
} download_state_t;

static bool download_cancelled(const solar_os_http_request_options_t *options)
{
    return (options->cancel_flag && *options->cancel_flag) ||
        (options->should_cancel && options->should_cancel(options->cancel_user_data));
}

static esp_err_t download_event(const solar_os_http_event_t *event, void *user)
{
    download_state_t *state = user;
    if (event->type == SOLAR_OS_HTTP_EVENT_RESPONSE) {
        if (event->status_code != 200) return ESP_ERR_INVALID_RESPONSE;
        if (event->content_length >= 0) {
            if ((uint64_t)event->content_length > state->maximum ||
                (state->expected && (uint64_t)event->content_length != state->expected))
                return ESP_ERR_INVALID_SIZE;
            state->total = (size_t)event->content_length;
        }
        return ESP_OK;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA) return ESP_OK;
    if (event->data_len > state->maximum - state->received) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = solar_os_crypto_sha256_update(&state->hash, event->data, event->data_len);
    if (err != ESP_OK) return err;
    const uint8_t *input = event->data;
    size_t remaining = event->data_len;
    while (remaining) {
        const size_t room = SOLAR_OS_DOWNLOAD_BUFFER_BYTES - state->pending;
        const size_t count = remaining < room ? remaining : room;
        memcpy(state->buffer + state->pending, input, count);
        state->pending += count;
        remaining -= count;
        input += count;
        if (state->pending == SOLAR_OS_DOWNLOAD_BUFFER_BYTES) {
            if (fwrite(state->buffer, 1, state->pending, state->file) != state->pending) return ESP_FAIL;
            state->pending = 0;
            solar_os_work_checkpoint(&state->work);
        }
    }
    state->received += event->data_len;
    if (state->progress) state->progress(state->received, state->total, state->user);
    return ESP_OK;
}

static esp_err_t download_verify(const solar_os_http_request_options_t *options,
    const char *path, download_state_t *state, const uint8_t expected[32])
{
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_FAIL;
    esp_err_t err = solar_os_crypto_sha256_start(&state->hash);
    size_t received = 0;
    while (err == ESP_OK) {
        if (download_cancelled(options)) { err = ESP_ERR_INVALID_STATE; break; }
        size_t count = fread(state->buffer, 1, SOLAR_OS_DOWNLOAD_BUFFER_BYTES, file);
        received += count;
        if (count) err = solar_os_crypto_sha256_update(&state->hash, state->buffer, count);
        if (count < SOLAR_OS_DOWNLOAD_BUFFER_BYTES) {
            if (ferror(file)) err = ESP_FAIL;
            break;
        }
        solar_os_work_checkpoint(&state->work);
    }
    uint8_t digest[32];
    if (err == ESP_OK) err = solar_os_crypto_sha256_finish(&state->hash, digest);
    if (err == ESP_OK && (received != state->received || memcmp(digest, expected, sizeof(digest))))
        err = ESP_ERR_INVALID_CRC;
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    return err;
}

esp_err_t solar_os_download_file(const solar_os_http_request_options_t *options,
    const char *staged_path, size_t max_size, size_t expected_size,
    const char *expected_sha256, solar_os_download_progress_fn progress,
    void *progress_user, solar_os_download_result_t *result)
{
    if (!options || !staged_path || !*staged_path || !max_size || expected_size > max_size ||
        (expected_sha256 && !solar_os_crypto_sha256_hex_is_valid(expected_sha256)) || !result)
        return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    if (download_cancelled(options)) return ESP_ERR_INVALID_STATE;
    download_state_t state = {.maximum = max_size, .expected = expected_size,
        .total = expected_size, .progress = progress, .user = progress_user};
    state.buffer = solar_os_memory_alloc(SOLAR_OS_DOWNLOAD_BUFFER_BYTES,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "download.buffer");
    if (!state.buffer) return ESP_ERR_NO_MEM;
    state.file = fopen(staged_path, "wb");
    if (!state.file) { solar_os_memory_free(state.buffer); return ESP_FAIL; }
    solar_os_crypto_sha256_init(&state.hash);
    esp_err_t err = setvbuf(state.file, NULL, _IONBF, 0) == 0 ?
        solar_os_crypto_sha256_start(&state.hash) : ESP_FAIL;
    solar_os_work_begin(&state.work);
    solar_os_http_request_options_t request_options = *options;
    request_options.event_handler = download_event;
    request_options.user_data = &state;
    solar_os_http_request_t *request = NULL;
    if (err == ESP_OK) err = solar_os_http_request_create(&request_options, &request);
    if (err == ESP_OK) err = solar_os_http_request_perform(request, &result->response);
    if (request) {
        esp_err_t cleanup = solar_os_http_request_destroy(request);
        if (err == ESP_OK) err = cleanup;
    }
    result->bytes = state.received;
    if (err == ESP_OK && result->response.status_code != 200) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && expected_size && state.received != expected_size) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK && download_cancelled(options)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && state.pending &&
        fwrite(state.buffer, 1, state.pending, state.file) != state.pending) err = ESP_FAIL;
    if (err == ESP_OK) err = solar_os_crypto_sha256_finish(&state.hash, result->sha256);
    if (err == ESP_OK && expected_sha256 &&
        !solar_os_crypto_sha256_matches_hex(result->sha256, expected_sha256)) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) err = solar_os_storage_sync_file(state.file);
    if (fclose(state.file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) err = download_verify(options, staged_path, &state, result->sha256);
    solar_os_crypto_sha256_free(&state.hash);
    solar_os_memory_free(state.buffer);
    if (err != ESP_OK) (void)remove(staged_path);
    return err;
}
