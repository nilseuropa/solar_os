#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ubx.h"

static void test_poll_encoding(void)
{
    uint8_t packet[8];
    assert(ubx_encode_poll(0x0AU, 0x04U, packet) == sizeof(packet));
    const uint8_t expected[] = {0xB5U, 0x62U, 0x0AU, 0x04U, 0U, 0U, 0x0EU, 0x34U};
    assert(memcmp(packet, expected, sizeof(expected)) == 0);

    assert(ubx_encode_poll(0x01U, 0x07U, packet) == sizeof(packet));
    const uint8_t nav_pvt[] = {0xB5U, 0x62U, 0x01U, 0x07U, 0U, 0U, 0x08U, 0x19U};
    assert(memcmp(packet, nav_pvt, sizeof(nav_pvt)) == 0);
}

static void test_stream_parser(void)
{
    const uint8_t stream[] = {
        '$', 'G', 'N', 'R', 'M', 'C', '\n',
        0xB5U, 0x62U, 0x01U, 0x07U, 0x03U, 0x00U,
        0x11U, 0x22U, 0x33U, 0x71U, 0xEAU,
    };
    ubx_parser_t parser;
    ubx_parser_reset(&parser);
    size_t messages = 0U;
    for (size_t i = 0; i < sizeof(stream); i++) {
        messages += ubx_parser_feed(&parser, stream[i]) ? 1U : 0U;
    }
    assert(messages == 1U);
    assert(parser.message_class == 0x01U);
    assert(parser.message_id == 0x07U);
    assert(parser.payload_len == 3U);
    assert(memcmp(parser.payload, "\x11\x22\x33", 3U) == 0);
}

static void test_bad_checksum_is_rejected(void)
{
    const uint8_t packet[] = {
        0xB5U, 0x62U, 0x0AU, 0x04U, 0U, 0U, 0x0EU, 0x35U,
    };
    ubx_parser_t parser;
    ubx_parser_reset(&parser);
    bool complete = false;
    for (size_t i = 0; i < sizeof(packet); i++) {
        complete |= ubx_parser_feed(&parser, packet[i]);
    }
    assert(!complete);
}

static void test_mon_ver_extension_payload_fits(void)
{
    ubx_parser_t parser;
    ubx_parser_reset(&parser);
    uint8_t checksum_a = 0U;
    uint8_t checksum_b = 0U;
    const uint8_t header[] = {0x0AU, 0x04U, 0xDCU, 0x00U};
    for (size_t i = 0; i < sizeof(header); i++) {
        checksum_a = (uint8_t)(checksum_a + header[i]);
        checksum_b = (uint8_t)(checksum_b + checksum_a);
    }
    const uint8_t sync[] = {0xB5U, 0x62U};
    for (size_t i = 0; i < sizeof(sync); i++) {
        assert(!ubx_parser_feed(&parser, sync[i]));
    }
    for (size_t i = 0; i < sizeof(header); i++) {
        assert(!ubx_parser_feed(&parser, header[i]));
    }
    for (uint16_t i = 0; i < 220U; i++) {
        const uint8_t value = (uint8_t)i;
        checksum_a = (uint8_t)(checksum_a + value);
        checksum_b = (uint8_t)(checksum_b + checksum_a);
        assert(!ubx_parser_feed(&parser, value));
    }
    assert(!ubx_parser_feed(&parser, checksum_a));
    assert(ubx_parser_feed(&parser, checksum_b));
    assert(parser.message_class == 0x0AU);
    assert(parser.message_id == 0x04U);
    assert(parser.payload_len == 220U);
}

int main(void)
{
    test_poll_encoding();
    test_stream_parser();
    test_bad_checksum_is_rejected();
    test_mon_ver_extension_payload_fits();
    puts("ubx tests: ok");
    return 0;
}
