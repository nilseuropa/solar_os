#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_native_mpeg_abi.h"

typedef struct solar_os_mpeg solar_os_mpeg_t;
typedef struct {
    const uint8_t *y, *cb, *cr;
    uint32_t width, height, y_stride, chroma_stride;
    double time;
} solar_os_mpeg_frame_t;
typedef solar_os_native_cancel_fn solar_os_mpeg_cancel_t;

/* Single-owner decoder. Returned planes/samples last until the next decode of
 * that track. MPEG-1 program streams, one video and optional MP2 track only.
 * Compressed track buffers are capped at 256 KiB each; video at 640x480. */
esp_err_t solar_os_mpeg_open(const char *path, solar_os_mpeg_cancel_t cancel, void *user,
                             solar_os_mpeg_t **decoder, char *detail, size_t detail_size);
void solar_os_mpeg_close(solar_os_mpeg_t *decoder);
void solar_os_mpeg_set_cancel(solar_os_mpeg_t *decoder, solar_os_mpeg_cancel_t cancel, void *user);
void solar_os_mpeg_info(const solar_os_mpeg_t *decoder, solar_os_mpeg_info_t *info);
/* Seek via timestamped intra frames, with bounded reference/audio warm-up.
 * Files without usable seek timestamps use sequential decode/discard instead.
 * Maintains audio alignment; both packet scanning and decoding are cancellable.
 * The next video/audio reads return the first retained frames at that position. */
esp_err_t solar_os_mpeg_seek(solar_os_mpeg_t *decoder, double seconds, double *position);
esp_err_t solar_os_mpeg_video(solar_os_mpeg_t *decoder, solar_os_mpeg_frame_t *frame, bool *ended);
esp_err_t solar_os_mpeg_audio(solar_os_mpeg_t *decoder, solar_os_mpeg_audio_t *audio, bool *ended);
const char *solar_os_mpeg_error(const solar_os_mpeg_t *decoder);
/* Fused nearest-neighbour scaling/conversion, with no full-size RGB image.
 * Color is wire-order RGB565; monochrome uses the video luma plane directly.
 * The S3 kernel accelerates color conversion, not MPEG entropy decoding. */
void solar_os_mpeg_raster(const solar_os_mpeg_frame_t *frame, uint8_t *output, uint32_t width,
                          uint32_t height, bool color, bool simd);
bool solar_os_mpeg_simd_available(void);
/* Fast, allocation-free bit-exact kernel check; false selects the fallback. */
bool solar_os_mpeg_simd_selftest(void);
