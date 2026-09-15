#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UBX_PAYLOAD_MAX 256U

typedef struct {
    uint8_t state;
    uint8_t message_class;
    uint8_t message_id;
    uint16_t payload_len;
    uint16_t payload_pos;
    uint8_t checksum_a;
    uint8_t checksum_b;
    uint8_t received_checksum_a;
    uint8_t payload[UBX_PAYLOAD_MAX];
} ubx_parser_t;

void ubx_parser_reset(ubx_parser_t *parser);
bool ubx_parser_feed(ubx_parser_t *parser, uint8_t byte);
size_t ubx_encode_poll(uint8_t message_class,
                       uint8_t message_id,
                       uint8_t output[8]);
