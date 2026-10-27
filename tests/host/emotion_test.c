#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_emotion_abi.h"
const solar_os_emotion_api_v1_t *solar_os_emotion_engine_api_v1(void);
int main(int argc, char **argv)
{
    uint16_t palette[4];
    solar_os_emotion_palette_rgb565(0xf800,0x001f,palette);
    assert(palette[0]==0x001f && palette[1]==0x5015 && palette[2]==0xa80a && palette[3]==0xf800);
    solar_os_emotion_palette_rgb565(0,0xffff,palette);
    assert(palette[0]==0xffff && palette[3]==0);
    const solar_os_emotion_api_v1_t *a = solar_os_emotion_engine_api_v1();
    void *s = calloc(1, a->state_bytes), *other = calloc(1, a->state_bytes);
    unsigned char image[64 * 64 + 2], neutral[64 * 64 + 2], second[64 * 64 + 2];
    memset(image, 0x55, sizeof(image));
    assert(!a->init(s, 0, 1) && !a->init(other, 0, 2));
    assert(!a->render(s, 0, neutral, sizeof(neutral), 256, 64, 64, 0));
    assert(!a->react(s, SOLAR_OS_EMOTION_FURIOUS, 0, 0, 0));
    assert(!a->render(s, 250, image + 1, sizeof(image) - 2, 256, 64, 64, 0));
    assert(!memcmp(neutral, image + 1, 4096)); /* zero is a valid reaction deadline */
    assert(!a->init(s, 0, 1));
    for (unsigned p = 0; p < SOLAR_OS_EMOTION_POSE_COUNT; ++p) {
        assert(a->pose_find(a->pose_name(p)) == (int)p);
        assert(!a->set(s, p, 0, 100));
        assert(!a->render(s, 100, image + 1, sizeof(image) - 2, 255, 64, 64, 0));
        assert(image[0] == 0x55 && image[sizeof(image) - 1] == 0x55);
        unsigned lit = 0;
        for (unsigned y = 0; y < 64; ++y) {
            assert(!(image[1 + y * 64 + 63] & 0xc0));
            for (unsigned x = 0; x < 255; ++x)
                lit += !!((image[1 + y * 64 + x / 4] >> ((x % 4) * 2)) & 3);
        }
        assert(lit > 0);
        if (argc > 1) {
            char path[512]; snprintf(path, sizeof(path), "%s/%02u.pgm", argv[1], p);
            FILE *f = fopen(path, "wb"); assert(f);
            fprintf(f, "P5\n255 64\n255\n");
            for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 255; ++x)
                fputc(((image[1 + y * 64 + x / 4] >> ((x % 4) * 2)) & 3) * 85, f);
            fclose(f);
        }
    }
    assert(!a->render(other, 100, second, sizeof(second), 256, 64, 64, 0));
    assert(!memcmp(neutral, second, 4096)); /* independent instances */
    assert(!a->set(s, SOLAR_OS_EMOTION_NEUTRAL, 0, 100));
    assert(!a->react(s, SOLAR_OS_EMOTION_FURIOUS, 500, 100, 100));
    assert(!a->render(s, 100, image, sizeof(image), 256, 64, 64, 0));
    assert(!memcmp(neutral, image, 4096)); /* continuous transition */
    assert(!a->render(s, 300, image, sizeof(image), 256, 64, 64, 0));
    assert(memcmp(neutral, image, 4096));
    assert(!a->render(s, 1000, image, sizeof(image), 256, 64, 64, 0));
    assert(!memcmp(neutral, image, 4096)); /* return even after skipped frames */
    assert(!a->blink(s, 1000));
    assert(!a->render(s, 1070, image, sizeof(image), 256, 64, 64, 0));
    assert(memcmp(neutral, image, 4096));
    assert(!a->render(s, 1200, image, sizeof(image), 256, 64, 64, 0));
    assert(!memcmp(neutral, image, 4096));
    assert(!a->look(s, 1000, -1000));
    assert(!a->render(s, 1200, image, sizeof(image), 256, 64, 64, 0));
    assert(memcmp(neutral, image, 4096));
    assert(a->look(s, 1001, 0) < 0 && a->set(s, 999, 0, 0) < 0);
    assert(a->render(s, 0, image, 1, 256, 64, 64, 0) < 0);
    assert(a->pose_find("unknown") < 0 && !a->pose_name(999));
    int16_t pcm[1024], split[1024];
    for (unsigned cue = 1; cue < SOLAR_OS_EMOTION_CUE_COUNT; ++cue) {
        assert(!a->sound(cue, 16000, 0, pcm, 1024));
        assert(!a->sound(cue, 16000, 0, split, 500));
        assert(!a->sound(cue, 16000, 500, split + 500, 524));
        assert(!memcmp(pcm, split, sizeof(pcm))); /* chunk boundaries do not change sound */
        assert(pcm[0] == 0);
        assert(!a->sound(cue, 16000, 16000, pcm, 1024));
        for (unsigned i = 0; i < 1024; ++i) assert(pcm[i] == 0);
    }
    free(other); free(s);
    puts("emotion engine passed");
}
