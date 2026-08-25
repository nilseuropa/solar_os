#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_osc.h"
#include "solar_os_parameters.h"

static float test_value;

static esp_err_t test_get(void *user, float *value)
{
    (void)user;
    *value = test_value;
    return ESP_OK;
}

static esp_err_t test_set(void *user, float value)
{
    (void)user;
    test_value = value;
    return ESP_OK;
}

static size_t append_string(uint8_t *packet, size_t offset, const char *text)
{
    const size_t length = strlen(text) + 1U;
    const size_t padded = (length + 3U) & ~(size_t)3U;
    memset(packet + offset, 0, padded);
    memcpy(packet + offset, text, length);
    return offset + padded;
}

static void write_u32(uint8_t *packet, size_t offset, uint32_t value)
{
    packet[offset] = (uint8_t)(value >> 24U);
    packet[offset + 1U] = (uint8_t)(value >> 16U);
    packet[offset + 2U] = (uint8_t)(value >> 8U);
    packet[offset + 3U] = (uint8_t)value;
}

static size_t make_message(uint8_t *packet,
                           const char *address,
                           char type,
                           uint32_t value)
{
    size_t offset = append_string(packet, 0U, address);
    char types[3] = {',', type, '\0'};
    offset = append_string(packet, offset, types);
    if (type == 'f' || type == 'i') {
        write_u32(packet, offset, value);
        offset += 4U;
    }
    return offset;
}

static void test_codec_and_dispatch(void)
{
    const solar_os_parameter_definition_t definition = {
        .name = "filter.cutoff",
        .label = "Cutoff",
        .unit = "Hz",
        .minimum = 20.0f,
        .maximum = 20000.0f,
        .step = 1.0f,
        .curve = SOLAR_OS_PARAMETER_CURVE_LOGARITHMIC,
        .get = test_get,
        .set = test_set,
    };
    solar_os_parameter_registration_t registration =
        SOLAR_OS_PARAMETER_REGISTRATION_INIT;
    assert(solar_os_parameters_register("synth", &definition, 1U,
                                        &registration) == ESP_OK);

    uint8_t packet[SOLAR_OS_OSC_PACKET_MAX];
    uint32_t raw = 0U;
    const float requested = 1234.0f;
    memcpy(&raw, &requested, sizeof(raw));
    size_t length = make_message(packet,
                                 "/solaros/parameter/synth/filter/cutoff",
                                 'f', raw);
    solar_os_osc_dispatch_result_t result;
    assert(solar_os_osc_dispatch_packet(packet, length, &result) == ESP_OK);
    assert(result.messages == 1U && result.applied == 1U);
    assert(test_value == 1234.0f);

    length = make_message(packet,
                          "/solaros/parameter/synth/filter/cutoff", 'i', 440U);
    assert(solar_os_osc_dispatch_packet(packet, length, &result) == ESP_OK);
    assert(test_value == 440.0f);

    length = make_message(packet,
                          "/solaros/parameter/missing/value", 'T', 0U);
    assert(solar_os_osc_dispatch_packet(packet, length, &result) == ESP_OK);
    assert(result.unknown_paths == 1U);

    uint8_t element[128];
    const size_t element_len = make_message(
        element, "/solaros/parameter/synth/filter/cutoff", 'i', 880U);
    memcpy(packet, "#bundle\0", 8U);
    memset(packet + 8U, 0, 8U);
    packet[15] = 1U;
    write_u32(packet, 16U, (uint32_t)element_len);
    memcpy(packet + 20U, element, element_len);
    assert(solar_os_osc_dispatch_packet(packet, 20U + element_len, &result) ==
           ESP_OK);
    assert(result.applied == 1U && test_value == 880.0f);

    packet[15] = 2U;
    assert(solar_os_osc_dispatch_packet(packet, 20U + element_len, &result) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_parameters_unregister(&registration) == ESP_OK);
}

static void test_encoding(void)
{
    uint8_t packet[64];
    size_t length = 0U;
    assert(solar_os_osc_encode_float("/x", 1.0f, packet, sizeof(packet),
                                     &length) == ESP_OK);
    const uint8_t expected_float[] = {
        '/', 'x', 0, 0, ',', 'f', 0, 0, 0x3f, 0x80, 0, 0,
    };
    assert(length == sizeof(expected_float));
    assert(memcmp(packet, expected_float, sizeof(expected_float)) == 0);
    assert(solar_os_osc_encode_int("/button", 1, packet, sizeof(packet),
                                   &length) == ESP_OK);
    assert(packet[length - 1U] == 1U);
}

static void test_bindings(void)
{
    solar_os_osc_clear();
    const solar_os_osc_binding_config_t scalar = {
        .name = "ambient",
        .source_type = SOLAR_OS_OSC_SOURCE_STREAM,
        .value_type = SOLAR_OS_OSC_VALUE_SCALAR,
        .source = "temperature",
        .address = "/room/temperature",
        .interval_ms = 500U,
        .delta = 0.1f,
        .edge = SOLAR_OS_OSC_EDGE_BOTH,
    };
    uint32_t scalar_id = 0U;
    assert(solar_os_osc_bind(&scalar, &scalar_id) == ESP_OK);
    assert(solar_os_osc_bind(&scalar, NULL) == ESP_ERR_INVALID_STATE);
    bool send = false;
    bool integer = false;
    assert(solar_os_osc_binding_prepare(scalar_id, 100U, 20.0f,
                                        &send, &integer) == ESP_OK);
    assert(send && !integer);
    solar_os_osc_binding_note_sent(scalar_id, 100U);
    assert(solar_os_osc_binding_prepare(scalar_id, 600U, 20.05f,
                                        &send, &integer) == ESP_OK);
    assert(!send);
    assert(solar_os_osc_binding_prepare(scalar_id, 1100U, 20.11f,
                                        &send, &integer) == ESP_OK);
    assert(send);

    solar_os_osc_binding_config_t dotted_source = scalar;
    strcpy(dotted_source.name, "dotted");
    strcpy(dotted_source.source, "sensor.ambient");
    assert(solar_os_osc_bind(&dotted_source, NULL) == ESP_OK);
    assert(solar_os_osc_unbind("dotted") == ESP_OK);

    const solar_os_osc_binding_config_t event = {
        .name = "button",
        .source_type = SOLAR_OS_OSC_SOURCE_STREAM,
        .value_type = SOLAR_OS_OSC_VALUE_EVENT,
        .source = "gpio17",
        .address = "/surface/button",
        .interval_ms = 20U,
        .edge = SOLAR_OS_OSC_EDGE_BOTH,
    };
    uint32_t event_id = 0U;
    assert(solar_os_osc_bind(&event, &event_id) == ESP_OK);
    assert(solar_os_osc_binding_prepare(event_id, 0U, 0.0f,
                                        &send, &integer) == ESP_OK);
    assert(!send && integer);
    assert(solar_os_osc_binding_prepare(event_id, 20U, 1.0f,
                                        &send, &integer) == ESP_OK);
    assert(send && integer);
    solar_os_osc_binding_note_error(event_id, 40U, ESP_ERR_NOT_FOUND, false);
    solar_os_osc_binding_info_t info;
    assert(solar_os_osc_binding_get(1U, &info));
    assert(!info.source_available && info.source_errors == 1U);
    assert(solar_os_osc_unbind("ambient") == ESP_OK);
    assert(solar_os_osc_binding_count() == 1U);
}

int main(void)
{
    test_codec_and_dispatch();
    test_encoding();
    test_bindings();
    puts("osc tests passed");
    return 0;
}
