#include "solar_os_meshcore_channel_key.h"

#include <cstring>

#include "SHA256.h"

extern "C" bool solar_os_meshcore_channel_key_derive_hashtag(
    const char *name,
    uint8_t key[SOLAR_OS_MESHCORE_HASHTAG_KEY_LEN])
{
    if (name == nullptr || name[0] != '#' || name[1] == '\0' ||
        key == nullptr) {
        return false;
    }

    SHA256 sha256;
    sha256.update(name, std::strlen(name));
    sha256.finalize(key, SOLAR_OS_MESHCORE_HASHTAG_KEY_LEN);
    return true;
}
