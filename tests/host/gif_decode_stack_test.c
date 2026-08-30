#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "solar_os_stb_image.h"

typedef struct {
    uint8_t *data;
    size_t length;
} gif_fixture_t;

/* Two highly compressed 480x320 frames with a deep GIF LZW prefix chain. */

static int base64_value(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static gif_fixture_t load_fixture(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long encoded_length = ftell(file);
    assert(encoded_length > 0);
    rewind(file);

    char *encoded = malloc((size_t)encoded_length);
    uint8_t *decoded = malloc((size_t)encoded_length);
    assert(encoded != NULL && decoded != NULL);
    assert(fread(encoded, 1, (size_t)encoded_length, file) ==
           (size_t)encoded_length);
    fclose(file);

    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t decoded_length = 0;
    for (long i = 0; i < encoded_length; i++) {
        if (isspace((unsigned char)encoded[i]) || encoded[i] == '=') {
            continue;
        }
        const int value = base64_value((unsigned char)encoded[i]);
        assert(value >= 0);
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded[decoded_length++] =
                (uint8_t)(accumulator >> bits);
            accumulator &= bits == 0 ? 0U : (UINT32_C(1) << bits) - 1U;
        }
    }
    free(encoded);
    return (gif_fixture_t){.data = decoded, .length = decoded_length};
}

static void *decode_on_small_stack(void *argument)
{
    const gif_fixture_t *fixture = argument;
    solar_os_stb_gif_animation_t animation = {0};
    assert(solar_os_stb_decode_gif_rgb(
               fixture->data, fixture->length,
               512U * 512U, 2U * 1024U * 1024U,
               480, 320, NULL, &animation) == ESP_OK);
    assert(animation.width == 480);
    assert(animation.height == 320);
    assert(animation.frame_count == 2);
    solar_os_stb_gif_animation_free(&animation);
    return NULL;
}

int main(void)
{
    gif_fixture_t fixture = load_fixture("deep_animation.gif.b64");
    pthread_attr_t attributes;
    assert(pthread_attr_init(&attributes) == 0);
    assert(pthread_attr_setstacksize(&attributes, 16384) == 0);
    pthread_t thread;
    assert(pthread_create(
               &thread, &attributes, decode_on_small_stack, &fixture) == 0);
    assert(pthread_join(thread, NULL) == 0);
    pthread_attr_destroy(&attributes);
    free(fixture.data);
    puts("GIF small-stack decode tests: ok");
    return 0;
}
