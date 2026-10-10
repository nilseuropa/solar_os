#define IMLIB_TEST_ENTRY original_imlib_suite
#include "imlib_test.c"
#include "solar_os_native_media.h"
#include "solar_os_native_image_abi.h"
#include "solar_os_native_vision_abi.h"

int main(void)
{
    const solar_os_native_image_api_v1_t *images = solar_os_native_media_get_service("image", 1, sizeof(*images));
    const solar_os_native_imlib_api_v1_t *imlib = solar_os_native_media_get_service("vision.imlib", 1, sizeof(*imlib));
    const solar_os_native_qr_api_v1_t *qr = solar_os_native_media_get_service("vision.qr", 1, sizeof(*qr));
    assert(images && imlib && qr);
    const solar_os_native_memory_api_v1_t *memory = solar_os_native_media_get_service("memory", 1, sizeof(*memory));
    assert(memory && !memory->alloc(0));
    void *buffer = memory->alloc(1024); assert(buffer); memory->free(buffer);
    assert(!solar_os_native_media_get_service(NULL, 1, 0));
    assert(!solar_os_native_media_get_service("image", 2, 0));
    assert(!solar_os_native_media_get_service("image", 1, sizeof(*images)+1));
    assert(!solar_os_native_media_get_service("inference", 1, 0));
    assert(!solar_os_native_media_get_service("media.video", 1, 0));
    uint8_t pixels[16*12] = {0};
    for (unsigned y = 3; y < 7; ++y) for (unsigned x = 4; x < 10; ++x) pixels[y*16+x] = 200;
    solar_os_raster_image_t *image = NULL;
    assert(!images->from_pixels(pixels, sizeof(pixels), 16, 12, SOLAR_OS_RASTER_IMAGE_GRAY8, 0, &image));
    solar_os_imlib_options_t o; imlib->defaults(&o); o.threshold_count = 1;
    o.thresholds[0] = (solar_os_imlib_threshold_t){128,255,0,0,0,0};
    solar_os_imlib_result_t *result = NULL;
    assert(!imlib->run(image, NULL, SOLAR_OS_IMLIB_BLOBS, &o, NULL, NULL, &result));
    assert(result->count == 1 && result->blobs[0].pixels == 24 && result->blobs[0].x == 4);
    imlib->result_free(result);
    assert(!imlib->run(image, NULL, SOLAR_OS_IMLIB_BINARY, &o, NULL, NULL, &result));
    solar_os_raster_image_t *mask = result->image; images->retain(mask);
    imlib->result_free(result); images->release(image);
    solar_os_raster_image_pixels_t view;
    assert(!images->pixels(mask, &view) && view.width == 16 && view.data[(3*16+4)*3] == 255);
    unsigned checks = 0;
    assert(imlib->run(mask, NULL, SOLAR_OS_IMLIB_GAUSSIAN, NULL, abort_request, &checks, &result) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!result && !workers); images->release(mask);
    char path[] = "../fixtures/vision/multiple.png";
    assert(!images->open(path, &image));
    solar_os_vision_qr_results_t *codes = NULL;
    assert(!qr->qrcodes(image, NULL, NULL, NULL, &codes) && codes->count == 2);
    images->release(image); assert(codes->codes[0].length > 0); qr->result_free(codes);
    failures = 0;
    assert(images->from_pixels(pixels, sizeof(pixels), 16, 12, SOLAR_OS_RASTER_IMAGE_GRAY8, 0, &image) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    failures = -1; assert(!allocations && !workers);
    puts("native image/imlib/QR tables: discovery, ownership, cancellation, allocation failure passed");
    return 0;
}
