#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "solar_os_meshcore_channel_key.h"

static void expect_key(const char *name, const uint8_t expected[16])
{
    uint8_t actual[SOLAR_OS_MESHCORE_HASHTAG_KEY_LEN]{};
    assert(solar_os_meshcore_channel_key_derive_hashtag(name, actual));
    assert(std::memcmp(actual, expected, sizeof(actual)) == 0);
}

int main()
{
    static const uint8_t test_key[16] = {
        0x9c, 0xd8, 0xfc, 0xf2, 0x2a, 0x47, 0x33, 0x3b,
        0x59, 0x1d, 0x96, 0xa2, 0xb8, 0x48, 0xb7, 0x3f,
    };
    static const uint8_t hansemesh_key[16] = {
        0x21, 0xdd, 0xee, 0x26, 0xdc, 0x52, 0x15, 0x15,
        0x3c, 0x39, 0x03, 0x16, 0x5f, 0xd4, 0x26, 0x2f,
    };
    uint8_t key[SOLAR_OS_MESHCORE_HASHTAG_KEY_LEN]{};

    expect_key("#test", test_key);
    expect_key("#hansemesh", hansemesh_key);
    assert(!solar_os_meshcore_channel_key_derive_hashtag(nullptr, key));
    assert(!solar_os_meshcore_channel_key_derive_hashtag("", key));
    assert(!solar_os_meshcore_channel_key_derive_hashtag("#", key));
    assert(!solar_os_meshcore_channel_key_derive_hashtag("hansemesh", key));
    assert(!solar_os_meshcore_channel_key_derive_hashtag("#hansemesh", nullptr));

    std::puts("meshcore channel key tests: ok");
    return 0;
}
