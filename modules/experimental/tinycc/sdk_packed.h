#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../../protocol/display_link/include/sdlp_lz4_block.h"

typedef struct {
    const char *name;
    uint32_t bytes, first_block;
} solar_os_cc_embedded_header_t;

#include "sdk_packed.inc"

/* Whole aligned blocks decode directly into the compiler's input buffer.
 * Nonsequential file reads can borrow one temporary block. */
static bool solar_os_cc_read_embedded_header(const solar_os_cc_embedded_header_t *header,
    uint32_t offset, void *destination, size_t bytes)
{
    if (offset > header->bytes || bytes > header->bytes - offset) return false;
    uint8_t *out = destination;
    while (bytes) {
        const uint32_t block = offset / SOLAR_OS_CC_SDK_BLOCK_BYTES;
        const uint32_t block_start = block * SOLAR_OS_CC_SDK_BLOCK_BYTES;
        const uint32_t skip = offset - block_start;
        const uint32_t index = header->first_block + block;
        size_t plain = header->bytes - block_start;
        if (plain > SOLAR_OS_CC_SDK_BLOCK_BYTES) plain = SOLAR_OS_CC_SDK_BLOCK_BYTES;
        size_t chunk = plain - skip;
        if (chunk > bytes) chunk = bytes;
        const uint8_t *packed = sdk_data + sdk_offsets[index];
        const size_t packed_bytes = sdk_offsets[index + 1U] - sdk_offsets[index];

        if (packed_bytes == plain) {
            memcpy(out, packed + skip, chunk);
        } else if (chunk == plain) {
            if (!solar_os_lz4_decode_block(out, plain, packed, packed_bytes)) return false;
        } else {
            uint8_t *temporary = malloc(plain);
            if (!temporary) return false;
            const bool ok = solar_os_lz4_decode_block(temporary, plain, packed, packed_bytes);
            if (ok) memcpy(out, temporary + skip, chunk);
            free(temporary);
            if (!ok) return false;
        }
        out += chunk;
        bytes -= chunk;
        offset += chunk;
    }
    return true;
}
