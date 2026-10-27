#include "gnss_output.h"

#include <string.h>

#define CONFIG_TIMEOUT_US 800000LL
#define NMEA_OUTPUT_KEY 0x10740002UL

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    for (size_t i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

size_t solar_os_gnss_output_encode(solar_os_gnss_output_protocol_t protocol,
                                   uint8_t cls, uint8_t id,
                                   const uint8_t *payload, size_t len,
                                   uint8_t frame[42])
{
    if (len > 32 || (len != 0 && payload == NULL) ||
        (protocol != SOLAR_OS_GNSS_OUTPUT_UBX && protocol != SOLAR_OS_GNSS_OUTPUT_CASIC) ||
        (protocol == SOLAR_OS_GNSS_OUTPUT_CASIC && len % 4 != 0)) {
        return 0;
    }
    const bool casic = protocol == SOLAR_OS_GNSS_OUTPUT_CASIC;
    frame[0] = casic ? 0xba : 0xb5;
    frame[1] = casic ? 0xce : 0x62;
    frame[casic ? 2 : 4] = (uint8_t)len;
    frame[casic ? 3 : 5] = 0;
    frame[casic ? 4 : 2] = cls;
    frame[casic ? 5 : 3] = id;
    if (len != 0) {
        memcpy(frame + 6, payload, len);
    }
    if (casic) {
        uint32_t sum = (uint32_t)len | ((uint32_t)cls << 16) | ((uint32_t)id << 24);
        for (size_t i = 0; i < len; i += 4) {
            sum += read_u32(payload + i);
        }
        write_u32(frame + 6 + len, sum);
        return len + 10;
    }
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < 6 + len; i++) {
        a += frame[i];
        b += a;
    }
    frame[6 + len] = a;
    frame[7 + len] = b;
    return len + 8;
}

static esp_err_t exchange(const solar_os_gnss_output_io_t *io,
                           solar_os_gnss_output_protocol_t protocol,
                           uint8_t id, const uint8_t *payload, size_t len,
                           uint8_t *response, size_t expected_len)
{
    uint8_t frame[42];
    const size_t frame_len = solar_os_gnss_output_encode(protocol, 6, id, payload, len, frame);
    esp_err_t ret = io->write(io->ctx, frame, frame_len);
    if (ret != ESP_OK) {
        return ret;
    }
    const bool casic = protocol == SOLAR_OS_GNSS_OUTPUT_CASIC;
    const int64_t deadline = io->now_us(io->ctx) + CONFIG_TIMEOUT_US;
    size_t used = 0, target = 0;
    while (io->now_us(io->ctx) < deadline) {
        uint8_t bytes[64];
        size_t count = 0;
        ret = io->read(io->ctx, bytes, sizeof(bytes), 20, &count);
        if (ret != ESP_OK) {
            return ret;
        }
        for (size_t i = 0; i < count; i++) {
            const uint8_t byte = bytes[i];
            if (used == 0) {
                if (byte == (casic ? 0xba : 0xb5)) {
                    frame[used++] = byte;
                }
                continue;
            }
            if (used == 1 && byte != (casic ? 0xce : 0x62)) {
                used = byte == frame[0] ? 1 : 0;
                continue;
            }
            frame[used++] = byte;
            if (used == 6) {
                const size_t size = frame[casic ? 2 : 4] |
                    ((size_t)frame[casic ? 3 : 5] << 8);
                if (size > 32 || (casic && size % 4 != 0)) {
                    used = 0;
                    continue;
                }
                target = size + (casic ? 10 : 8);
            }
            if (used < 6 || used != target) {
                continue;
            }
            uint8_t check[42];
            const uint8_t cls = frame[casic ? 4 : 2];
            const uint8_t msg = frame[casic ? 5 : 3];
            const size_t size = used - (casic ? 10 : 8);
            const size_t encoded = solar_os_gnss_output_encode(protocol, cls, msg,
                                                               frame + 6, size, check);
            const bool valid = encoded == used && memcmp(check, frame, used) == 0;
            used = 0;
            if (!valid) {
                continue;
            }
            if (cls == 5 && msg == 0 && size >= 2 && frame[6] == 6 &&
                (frame[7] == id || frame[7] == (casic ? 0 : 0x8a))) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (cls == 6 && msg == id && size == expected_len) {
                if (casic && frame[6] == 1) { /* UART1 is unused on L76K. */
                    continue;
                }
                memcpy(response, frame + 6, size);
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t read_config(const solar_os_gnss_output_io_t *io,
                              solar_os_gnss_output_protocol_t protocol,
                              solar_os_gnss_output_t *output)
{
    uint8_t response[9];
    if (protocol == SOLAR_OS_GNSS_OUTPUT_UBX) {
        uint8_t query[8] = {0}; /* VALGET v0, RAM layer, position 0. */
        write_u32(query + 4, NMEA_OUTPUT_KEY);
        const esp_err_t ret = exchange(io, protocol, 0x8b, query, sizeof(query), response, 9);
        if (ret != ESP_OK) {
            return ret;
        }
        if (response[0] != 1 || response[1] != 0 || response[2] != 0 || response[3] != 0 ||
            read_u32(response + 4) != NMEA_OUTPUT_KEY || response[8] > 1) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        output->enabled = response[8] != 0;
    } else {
        const esp_err_t ret = exchange(io, protocol, 0, NULL, 0, response, 8);
        if (ret != ESP_OK) {
            return ret;
        }
        if (response[0] != 0 || (response[1] & 0x11) != 0x11) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        memcpy(output->port_config, response, 8);
        output->enabled = (response[1] & 0x20) != 0;
    }
    output->protocol = protocol;
    return ESP_OK;
}

esp_err_t solar_os_gnss_output_probe(const solar_os_gnss_output_io_t *io,
                                     uint32_t baud,
                                     solar_os_gnss_output_t *output)
{
    memset(output, 0, sizeof(*output));
    esp_err_t ret = read_config(io, SOLAR_OS_GNSS_OUTPUT_UBX, output);
    if (ret == ESP_ERR_TIMEOUT || ret == ESP_ERR_NOT_SUPPORTED) {
        ret = read_config(io, SOLAR_OS_GNSS_OUTPUT_CASIC, output);
    }
    if (ret == ESP_OK && output->protocol == SOLAR_OS_GNSS_OUTPUT_CASIC &&
        read_u32(output->port_config + 4) != baud) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ret == ESP_ERR_TIMEOUT ? ESP_ERR_NOT_SUPPORTED : ret;
}

esp_err_t solar_os_gnss_output_set(const solar_os_gnss_output_io_t *io,
                                   solar_os_gnss_output_t *output,
                                   bool enabled)
{
    uint8_t payload[9] = {0};
    const bool casic = output->protocol == SOLAR_OS_GNSS_OUTPUT_CASIC;
    if (output->protocol == SOLAR_OS_GNSS_OUTPUT_UNKNOWN) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (casic) {
        memcpy(payload, output->port_config, 8);
        payload[1] = enabled ? (payload[1] | 0x20) : (payload[1] & ~0x20);
    } else {
        payload[1] = 1; /* VALSET v0, RAM only; never BBR or flash. */
        write_u32(payload + 4, NMEA_OUTPUT_KEY);
        payload[8] = enabled;
    }
    uint8_t frame[42];
    const size_t len = solar_os_gnss_output_encode(output->protocol, 6, casic ? 0 : 0x8a,
                                                   payload, casic ? 8 : 9, frame);
    esp_err_t ret = io->write(io->ctx, frame, len);
    if (ret != ESP_OK) {
        return ret;
    }
    solar_os_gnss_output_t actual;
    ret = read_config(io, output->protocol, &actual);
    if (ret != ESP_OK) {
        return ret;
    }
    if (actual.enabled != enabled ||
        (casic && memcmp(actual.port_config, payload, 8) != 0)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *output = actual;
    return ESP_OK;
}

esp_err_t solar_os_gnss_output_check_quiet(const solar_os_gnss_output_io_t *io)
{
    /* Drain in-flight output/ACKs for 300 ms, then observe more than the
     * reader's 2.5 s freshness window. Binary periodic output also fails. */
    int64_t deadline = io->now_us(io->ctx) + 300000LL;
    for (unsigned phase = 0; phase < 2; phase++) {
        while (io->now_us(io->ctx) < deadline) {
            uint8_t data[64];
            size_t count = 0;
            const esp_err_t ret = io->read(io->ctx, data, sizeof(data), 20, &count);
            if (ret != ESP_OK) {
                return ret;
            }
            if (phase == 1 && count != 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        deadline = io->now_us(io->ctx) + 2600000LL;
    }
    return ESP_OK;
}
