#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_meshcore_stream_codec.h"

int main(void)
{
    uint8_t first_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE] = {0};
    uint8_t second_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE] = {0};
    second_key[31] = 1U;
    const uint32_t first_id = solar_os_meshcore_stream_peer_id(first_key);
    const uint32_t second_id = solar_os_meshcore_stream_peer_id(second_key);
    assert(first_id != 0U && first_id != UINT32_MAX);
    assert(second_id != 0U && second_id != UINT32_MAX);
    assert(first_id != second_id);
    assert(first_id == solar_os_meshcore_stream_peer_id(first_key));
    assert(solar_os_meshcore_stream_peer_id(NULL) == 0U);

    const uint8_t frame[] = {0x10U, 0x04U, 0xaaU, 0x55U};
    uint8_t envelope[SOLAR_OS_MESHCORE_STREAM_ENVELOPE_MAX] = {0};
    size_t envelope_len = 0U;
    assert(solar_os_meshcore_stream_envelope_encode(
               frame,
               sizeof(frame),
               envelope,
               sizeof(envelope),
               &envelope_len) == ESP_OK);
    assert(envelope_len ==
           SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE + sizeof(frame));
    assert(solar_os_meshcore_stream_envelope_matches(
        envelope, envelope_len));

    const uint8_t *decoded = NULL;
    size_t decoded_len = 0U;
    assert(solar_os_meshcore_stream_envelope_decode(
               envelope,
               envelope_len,
               &decoded,
               &decoded_len) == ESP_OK);
    assert(decoded_len == sizeof(frame));
    assert(memcmp(decoded, frame, sizeof(frame)) == 0);

    memset(&envelope[envelope_len], 0, 11U);
    assert(solar_os_meshcore_stream_envelope_decode(
               envelope,
               envelope_len + 11U,
               &decoded,
               &decoded_len) == ESP_OK);
    assert(decoded_len == sizeof(frame));
    envelope[envelope_len + 10U] = 1U;
    assert(solar_os_meshcore_stream_envelope_decode(
               envelope,
               envelope_len + 11U,
               &decoded,
               &decoded_len) == ESP_ERR_INVALID_ARG);
    envelope[envelope_len + 10U] = 0U;

    envelope[2]++;
    assert(solar_os_meshcore_stream_envelope_decode(
               envelope,
               envelope_len,
               &decoded,
               &decoded_len) == ESP_ERR_INVALID_ARG);
    envelope[2]--;
    assert(solar_os_meshcore_stream_envelope_decode(
               envelope,
               SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE,
               &decoded,
               &decoded_len) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_meshcore_stream_envelope_encode(
               frame,
               sizeof(frame),
               envelope,
               sizeof(frame),
               &envelope_len) == ESP_ERR_INVALID_ARG);

    puts("meshcore stream codec tests: ok");
    return 0;
}
