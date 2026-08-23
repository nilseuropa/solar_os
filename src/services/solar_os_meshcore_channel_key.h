#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_MESHCORE_HASHTAG_KEY_LEN 16U

bool solar_os_meshcore_channel_key_derive_hashtag(
    const char *name,
    uint8_t key[SOLAR_OS_MESHCORE_HASHTAG_KEY_LEN]);

#ifdef __cplusplus
}
#endif
