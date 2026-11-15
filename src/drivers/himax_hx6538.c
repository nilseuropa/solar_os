#include "himax_hx6538.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/base64.h"
#include "solar_os_json_scan.h"

esp_err_t solar_os_himax_init(solar_os_himax_t *camera, const solar_os_himax_io_t *io)
{
    if (!camera || !io || !io->write || !io->read || !io->resize || !io->now_ms || !io->wait_ms)
        return ESP_ERR_INVALID_ARG;
    memset(camera, 0, sizeof(*camera));
    camera->io = *io;
    return ESP_OK;
}

void solar_os_himax_clear(solar_os_himax_t *camera)
{
    if (!camera) return;
    if (camera->reply) camera->io.resize(camera->io.ctx, camera->reply, 0);
    camera->reply = NULL;
    camera->reply_used = camera->reply_capacity = 0;
    camera->input_used = camera->input_position = 0;
}

static esp_err_t reserve_reply(solar_os_himax_t *camera)
{
    if (camera->reply_used + 1U < camera->reply_capacity) return ESP_OK;
    if (camera->reply_capacity >= SOLAR_OS_HIMAX_REPLY_MAX) return ESP_ERR_INVALID_SIZE;
    size_t capacity = camera->reply_capacity ? camera->reply_capacity * 2U : 4096U;
    void *resized = camera->io.resize(camera->io.ctx, camera->reply, capacity);
    if (!resized) return ESP_ERR_NO_MEM;
    camera->reply = resized;
    camera->reply_capacity = capacity;
    return ESP_OK;
}

static esp_err_t receive_reply(solar_os_himax_t *camera, uint64_t deadline)
{
    camera->reply_used = 0;
    while (camera->io.now_ms(camera->io.ctx) < deadline) {
        if (camera->input_position == camera->input_used) {
            camera->input_position = camera->input_used = 0;
            esp_err_t error = camera->io.read(camera->io.ctx, camera->input,
                                               sizeof(camera->input), &camera->input_used);
            if (error != ESP_OK) return error;
            if (camera->input_used > sizeof(camera->input)) return ESP_ERR_INVALID_RESPONSE;
            if (!camera->input_used) { camera->io.wait_ms(camera->io.ctx, 2); continue; }
        }
        const uint8_t c = camera->input[camera->input_position++];
        if (!c || c == '\r') continue;
        if (!camera->reply_used && c != '{') continue;
        esp_err_t error = reserve_reply(camera);
        if (error != ESP_OK) return error;
        if (c == '\n') { camera->reply[camera->reply_used] = '\0'; return ESP_OK; }
        camera->reply[camera->reply_used++] = (char)c;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t reply_code(const char *reply)
{
    uint64_t code;
    if (!solar_os_json_scan_object_uint64(reply, "code", &code)) return ESP_ERR_INVALID_RESPONSE;
    switch (code) {
    case 0: return ESP_OK;
    case 3: return ESP_ERR_TIMEOUT;
    case 5: return ESP_ERR_INVALID_ARG;
    case 6: return ESP_ERR_NO_MEM;
    case 7: return ESP_ERR_INVALID_STATE;
    case 8: return ESP_ERR_NOT_SUPPORTED;
    default: return ESP_FAIL;
    }
}

static bool reply_matches(solar_os_himax_t *camera, const char *name, uint64_t type)
{
    char actual[32]; bool truncated = false;
    uint64_t actual_type;
    return solar_os_json_scan_object_string(camera->reply, "name", actual, sizeof(actual), &truncated) &&
        !truncated && !strcmp(actual, name) &&
        solar_os_json_scan_object_uint64(camera->reply, "type", &actual_type) && actual_type == type;
}

esp_err_t solar_os_himax_command(solar_os_himax_t *camera, const char *command,
                                  const char *response_name, uint32_t timeout_ms)
{
    if (!camera || !command || !response_name || !timeout_ms) return ESP_ERR_INVALID_ARG;
    esp_err_t error = camera->io.write(camera->io.ctx, (const uint8_t *)command, strlen(command));
    const uint64_t deadline = camera->io.now_ms(camera->io.ctx) + timeout_ms;
    while (error == ESP_OK) {
        error = receive_reply(camera, deadline);
        if (error == ESP_OK && reply_matches(camera, response_name, 0)) return reply_code(camera->reply);
    }
    return error;
}

esp_err_t solar_os_himax_probe(solar_os_himax_t *camera, uint32_t timeout_ms)
{
    esp_err_t error = solar_os_himax_command(camera, "AT+VER?\r\n", "VER?", timeout_ms);
    if (error != ESP_OK) return error;
    const char *data; size_t length;
    bool truncated = false;
    if (!solar_os_json_scan_object_value(camera->reply, "data", &data, &length) || *data != '{' ||
        !solar_os_json_scan_object_string(data, "software", camera->version, sizeof(camera->version), &truncated) || truncated)
        return ESP_ERR_INVALID_RESPONSE;
    error = solar_os_himax_command(camera, "AT+SENSORS?\r\n", "SENSORS?", timeout_ms);
    if (error != ESP_OK) return error;
    if (!solar_os_json_scan_object_value(camera->reply, "data", &data, &length)) return ESP_ERR_INVALID_RESPONSE;
    camera->mode_count = 0;
    for (size_t i = 0; i < SOLAR_OS_HIMAX_MODE_MAX; i++) {
        const char *sensor, *options;
        if (!solar_os_json_scan_array_value(data, i, &sensor, &length)) break;
        uint64_t id, type;
        if (!solar_os_json_scan_object_uint64(sensor, "type", &type) || type != 1 ||
            !solar_os_json_scan_object_uint64(sensor, "id", &id) || id > UINT16_MAX ||
            !solar_os_json_scan_object_value(sensor, "opts", &options, &length) || *options != '{') continue;
        camera->sensor_id = (unsigned)id;
        for (unsigned option = 0; option <= UINT8_MAX; option++) {
            char key[4], description[64];
            snprintf(key, sizeof(key), "%u", option);
            truncated = false;
            if (!solar_os_json_scan_object_string(options, key, description, sizeof(description), &truncated)) continue;
            if (truncated || description[0] < '0' || description[0] > '9') return ESP_ERR_INVALID_RESPONSE;
            char *end;
            const unsigned long width = strtoul(description, &end, 10);
            if (*end++ != 'x' || *end < '0' || *end > '9') return ESP_ERR_INVALID_RESPONSE;
            const unsigned long height = strtoul(end, &end, 10);
            if (!width || !height || width > UINT16_MAX || height > UINT16_MAX ||
                (*end && *end != ' ')) return ESP_ERR_INVALID_RESPONSE;
            if (camera->mode_count == SOLAR_OS_HIMAX_MODE_MAX) return ESP_ERR_INVALID_SIZE;
            camera->modes[camera->mode_count++] = (solar_os_himax_mode_t){width, height, option};
        }
        if (camera->mode_count) return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool jpeg_dimensions(const uint8_t *jpeg, size_t length, uint16_t *width, uint16_t *height)
{
    if (length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
        jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9) return false;
    size_t p = 2;
    while (p + 4 < length) {
        if (jpeg[p++] != 0xff) return false;
        while (p < length && jpeg[p] == 0xff) p++;
        if (p + 3 >= length) return false;
        const uint8_t marker = jpeg[p++];
        if (marker == 0xda || marker == 0xd9) return false;
        const size_t segment = ((size_t)jpeg[p] << 8U) | jpeg[p + 1];
        if (segment < 2 || segment > length - p) return false;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (segment < 8) return false;
            *height = ((uint16_t)jpeg[p + 3] << 8U) | jpeg[p + 4];
            *width = ((uint16_t)jpeg[p + 5] << 8U) | jpeg[p + 6];
            return *width && *height;
        }
        p += segment;
    }
    return false;
}

esp_err_t solar_os_himax_sample(solar_os_himax_t *camera, uint32_t timeout_ms,
                                 const uint8_t **jpeg, size_t *length,
                                 uint16_t *width, uint16_t *height)
{
    if (!camera || !jpeg || !length || !width || !height || !timeout_ms) return ESP_ERR_INVALID_ARG;
    *jpeg = NULL; *length = 0; *width = *height = 0;
    const char command[] = "AT+SAMPLE=1\r\n";
    esp_err_t error = camera->io.write(camera->io.ctx, (const uint8_t *)command, sizeof(command) - 1);
    const uint64_t deadline = camera->io.now_ms(camera->io.ctx) + timeout_ms;
    while (error == ESP_OK) {
        error = receive_reply(camera, deadline);
        if (error != ESP_OK) break;
        if (!reply_matches(camera, "SAMPLE", 0) && !reply_matches(camera, "SAMPLE", 1)) continue;
        error = reply_code(camera->reply);
        if (error != ESP_OK) break;
        if (!reply_matches(camera, "SAMPLE", 1)) continue;
        const char *data, *encoded; size_t span;
        if (!solar_os_json_scan_object_value(camera->reply, "data", &data, &span) || *data != '{' ||
            !solar_os_json_scan_object_value(data, "image", &encoded, &span) || span < 2 || *encoded != '"')
            return ESP_ERR_INVALID_RESPONSE;
        char *source = (char *)encoded + 1;
        size_t used = 0;
        for (size_t i = 0; i < span - 2; i++) {
            char c = source[i];
            if (c == '\\') {
                if (++i >= span - 2 || source[i] != '/') return ESP_ERR_INVALID_RESPONSE;
                c = '/';
            }
            source[used++] = c;
        }
        /* The destination precedes the encoded input; decoding shrinks it.
         * One PSRAM allocation becomes the immutable leased JPEG. */
        size_t decoded = 0;
        if (mbedtls_base64_decode((uint8_t *)camera->reply, camera->reply_capacity, &decoded,
                                  (const uint8_t *)source, used) != 0)
            return ESP_ERR_INVALID_RESPONSE;
        /* SSCMA may encode its zero-padded JPEG buffer. Keep only the image,
         * while still requiring an end marker before any padding. */
        while (decoded && camera->reply[decoded - 1] == '\0') decoded--;
        if (!jpeg_dimensions((const uint8_t *)camera->reply, decoded, width, height))
            return ESP_ERR_INVALID_RESPONSE;
        *length = decoded;
        *jpeg = (const uint8_t *)camera->reply;
        return ESP_OK;
    }
    return error;
}
