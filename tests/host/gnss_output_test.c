#include <stdio.h>
#include "gnss_output_fixture.h"

int main(void)
{
    uint8_t frame[42];
    const uint8_t poll[] = {0xba, 0xce, 0, 0, 6, 0, 0, 0, 6, 0};
    assert(solar_os_gnss_output_encode(SOLAR_OS_GNSS_OUTPUT_CASIC, 6, 0, NULL, 0, frame) == 10);
    assert(memcmp(frame, poll, sizeof(poll)) == 0); /* Vendor CFG-PRT example. */
    assert(solar_os_gnss_output_encode(SOLAR_OS_GNSS_OUTPUT_CASIC, 6, 0, poll, 1, frame) == 0);
    assert(solar_os_gnss_output_encode(SOLAR_OS_GNSS_OUTPUT_UBX, 6, 0, poll, 33, frame) == 0);
    for (unsigned protocol = SOLAR_OS_GNSS_OUTPUT_UBX;
         protocol <= SOLAR_OS_GNSS_OUTPUT_CASIC; protocol++) {
        int64_t now = 0;
        gnss_output_fixture_t f = {.protocol = protocol, .clock = &now,
                                   .baud = 38400, .text_enabled = true};
        solar_os_gnss_output_io_t io = fixture_io(&f);
        solar_os_gnss_output_t output;
        assert(solar_os_gnss_output_probe(&io, f.baud, &output) == ESP_OK);
        assert(output.protocol == protocol && output.enabled);
        assert(solar_os_gnss_output_set(&io, &output, false) == ESP_OK);
        assert(!output.enabled && !f.text_enabled && f.sets == 1);
        assert(solar_os_gnss_output_check_quiet(&io) == ESP_OK);
        f.periodic_binary = true;
        assert(solar_os_gnss_output_check_quiet(&io) == ESP_ERR_INVALID_RESPONSE);
        f.periodic_binary = false;
        /* A fresh driver can discover and restore a muted receiver. */
        memset(&output, 0, sizeof(output));
        assert(solar_os_gnss_output_probe(&io, f.baud, &output) == ESP_OK && !output.enabled);
        assert(solar_os_gnss_output_set(&io, &output, true) == ESP_OK && output.enabled);
        f.ignore_set = true;
        assert(solar_os_gnss_output_set(&io, &output, false) == ESP_ERR_INVALID_RESPONSE);
        assert(output.enabled && f.text_enabled);
        f.ignore_set = false;
        f.nack = true;
        assert(solar_os_gnss_output_set(&io, &output, false) == ESP_ERR_NOT_SUPPORTED);
        f.nack = false;
        f.corrupt = true;
        assert(solar_os_gnss_output_probe(&io, f.baud, &output) == ESP_ERR_NOT_SUPPORTED);
        f.corrupt = false;
        f.write_fail = true;
        assert(solar_os_gnss_output_probe(&io, f.baud, &output) == ESP_FAIL);
    }
    int64_t now = 0;
    gnss_output_fixture_t f = {.clock = &now, .protocol = SOLAR_OS_GNSS_OUTPUT_UBX,
                               .wrong_key = true};
    solar_os_gnss_output_io_t io = fixture_io(&f);
    solar_os_gnss_output_t output;
    assert(solar_os_gnss_output_probe(&io, 9600, &output) == ESP_ERR_INVALID_RESPONSE);
    f.protocol = SOLAR_OS_GNSS_OUTPUT_CASIC;
    f.baud = 38400;
    assert(solar_os_gnss_output_probe(&io, 9600, &output) == ESP_ERR_INVALID_RESPONSE);
    f.protocol = SOLAR_OS_GNSS_OUTPUT_UNKNOWN;
    assert(solar_os_gnss_output_probe(&io, 9600, &output) == ESP_ERR_NOT_SUPPORTED);
    assert(f.sets == 0); /* Unknown receivers receive queries, never configuration writes. */
    puts("GNSS output control tests: ok");
    return 0;
}
