#include "solar_os_flash_stub.h"

#include "miniz.h"
#include "solar_os_memory.h"

uint8_t *solar_os_flash_stub_unpack(const uint8_t *packed, size_t size)
{
    if (!packed || !size) return NULL;
    const size_t packed_size = (size_t)packed[0] | (size_t)packed[1] << 8U;
    if (!packed_size) return NULL;
    uint8_t *output = solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "flash.stub");
    if (!output) return NULL;
    tinfl_decompressor *decoder = solar_os_memory_alloc(sizeof(*decoder),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "flash.stub.decoder");
    if (!decoder) {
        solar_os_memory_free(output);
        return NULL;
    }
    tinfl_init(decoder);
    size_t consumed = packed_size, produced = size;
    const tinfl_status status = tinfl_decompress(decoder, packed + 2, &consumed, output, output, &produced,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    solar_os_memory_free(decoder);
    if (status != TINFL_STATUS_DONE || consumed != packed_size || produced != size) {
        solar_os_memory_free(output);
        return NULL;
    }
    return output;
}

void solar_os_flash_stub_release(uint8_t *data)
{
    solar_os_memory_free(data);
}
