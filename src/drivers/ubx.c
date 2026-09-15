#include "ubx.h"

#include <string.h>

enum {
    UBX_SYNC1,
    UBX_SYNC2,
    UBX_CLASS,
    UBX_ID,
    UBX_LENGTH_LOW,
    UBX_LENGTH_HIGH,
    UBX_PAYLOAD,
    UBX_CHECKSUM_A,
    UBX_CHECKSUM_B,
};

static void checksum_add(ubx_parser_t *parser, uint8_t byte)
{
    parser->checksum_a = (uint8_t)(parser->checksum_a + byte);
    parser->checksum_b = (uint8_t)(parser->checksum_b + parser->checksum_a);
}

void ubx_parser_reset(ubx_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

bool ubx_parser_feed(ubx_parser_t *parser, uint8_t byte)
{
    if (parser == NULL) {
        return false;
    }

    switch (parser->state) {
    case UBX_SYNC1:
        if (byte == 0xB5U) {
            parser->state = UBX_SYNC2;
        }
        break;
    case UBX_SYNC2:
        if (byte == 0x62U) {
            parser->state = UBX_CLASS;
            parser->checksum_a = 0U;
            parser->checksum_b = 0U;
        } else {
            parser->state = byte == 0xB5U ? UBX_SYNC2 : UBX_SYNC1;
        }
        break;
    case UBX_CLASS:
        parser->message_class = byte;
        checksum_add(parser, byte);
        parser->state = UBX_ID;
        break;
    case UBX_ID:
        parser->message_id = byte;
        checksum_add(parser, byte);
        parser->state = UBX_LENGTH_LOW;
        break;
    case UBX_LENGTH_LOW:
        parser->payload_len = byte;
        checksum_add(parser, byte);
        parser->state = UBX_LENGTH_HIGH;
        break;
    case UBX_LENGTH_HIGH:
        parser->payload_len |= (uint16_t)byte << 8U;
        checksum_add(parser, byte);
        parser->payload_pos = 0U;
        if (parser->payload_len > UBX_PAYLOAD_MAX) {
            ubx_parser_reset(parser);
        } else {
            parser->state = parser->payload_len == 0U ? UBX_CHECKSUM_A : UBX_PAYLOAD;
        }
        break;
    case UBX_PAYLOAD:
        parser->payload[parser->payload_pos++] = byte;
        checksum_add(parser, byte);
        if (parser->payload_pos == parser->payload_len) {
            parser->state = UBX_CHECKSUM_A;
        }
        break;
    case UBX_CHECKSUM_A:
        parser->received_checksum_a = byte;
        parser->state = UBX_CHECKSUM_B;
        break;
    case UBX_CHECKSUM_B: {
        const bool valid = parser->received_checksum_a == parser->checksum_a &&
            byte == parser->checksum_b;
        parser->state = UBX_SYNC1;
        return valid;
    }
    default:
        ubx_parser_reset(parser);
        break;
    }
    return false;
}

size_t ubx_encode_poll(uint8_t message_class,
                       uint8_t message_id,
                       uint8_t output[8])
{
    if (output == NULL) {
        return 0U;
    }
    output[0] = 0xB5U;
    output[1] = 0x62U;
    output[2] = message_class;
    output[3] = message_id;
    output[4] = 0U;
    output[5] = 0U;
    uint8_t checksum_a = 0U;
    uint8_t checksum_b = 0U;
    for (size_t i = 2U; i < 6U; i++) {
        checksum_a = (uint8_t)(checksum_a + output[i]);
        checksum_b = (uint8_t)(checksum_b + checksum_a);
    }
    output[6] = checksum_a;
    output[7] = checksum_b;
    return 8U;
}
