#include "solar_os_native_mpeg.h"
#include <string.h>
#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_MPEG
#include "solar_os.h"
#include "solar_os_mpeg.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

#define MPEG_HANDLE_KIND SOLAR_OS_NATIVE_HANDLE_MPEG

static int mpeg_destroy(void *opaque)
{
    solar_os_mpeg_close(opaque);
    return SOLAR_OS_NATIVE_OK;
}

static int mpeg_open(const char *path, solar_os_native_cancel_fn cancel, void *user,
    solar_os_native_handle_t *out, char *detail, size_t capacity)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!path || !*path || (!detail && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (detail && capacity) detail[0] = 0;
    solar_os_mpeg_t *decoder = NULL;
    const esp_err_t error = solar_os_mpeg_open(path, cancel, user, &decoder, detail, capacity);
    if (error) return solar_os_native_result(error);
    solar_os_mpeg_set_cancel(decoder, NULL, NULL);
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const int result = owner ? solar_os_native_handle_create_owned(MPEG_HANDLE_KIND, decoder, mpeg_destroy, owner, out) :
        solar_os_native_handle_create(MPEG_HANDLE_KIND, decoder, mpeg_destroy, out);
    if (result) solar_os_mpeg_close(decoder);
    return result;
}

static int mpeg_info(solar_os_native_handle_t id, solar_os_mpeg_info_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_mpeg_info_t){0};
    void *decoder;
    const int result = solar_os_native_handle_acquire_exclusive(id, MPEG_HANDLE_KIND, &decoder);
    if (result) return result;
    solar_os_mpeg_info(decoder, out);
    solar_os_native_handle_release_exclusive(id);
    return SOLAR_OS_NATIVE_OK;
}

static int mpeg_error_detail(solar_os_native_handle_t id, char *detail, size_t capacity)
{
    if (!detail || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    detail[0] = 0;
    void *decoder;
    int result = solar_os_native_handle_acquire_exclusive(id, MPEG_HANDLE_KIND, &decoder);
    if (result) return result;
    const char *message = solar_os_mpeg_error(decoder);
    const size_t size = strlen(message) + 1;
    if (size <= capacity) memcpy(detail, message, size);
    else result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int mpeg_seek(solar_os_native_handle_t id, double seconds, double *position,
    solar_os_native_cancel_fn cancel, void *user)
{
    if (!position) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *position = 0;
    void *decoder;
    const int result = solar_os_native_handle_acquire_exclusive(id, MPEG_HANDLE_KIND, &decoder);
    if (result) return result;
    solar_os_mpeg_set_cancel(decoder, cancel, user);
    const esp_err_t error = solar_os_mpeg_seek(decoder, seconds, position);
    solar_os_mpeg_set_cancel(decoder, NULL, NULL);
    solar_os_native_handle_release_exclusive(id);
    return solar_os_native_result(error);
}

static int mpeg_video(solar_os_native_handle_t id, solar_os_native_mpeg_video_fn visit,
    void *user, solar_os_native_cancel_fn cancel, void *cancel_user, bool *ended)
{
    if (!visit || !ended) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *ended = false;
    void *decoder;
    int result = solar_os_native_handle_acquire_exclusive(id, MPEG_HANDLE_KIND, &decoder);
    if (result) return result;
    solar_os_mpeg_set_cancel(decoder, cancel, cancel_user);
    solar_os_mpeg_frame_t frame;
    const esp_err_t error = solar_os_mpeg_video(decoder, &frame, ended);
    solar_os_mpeg_set_cancel(decoder, NULL, NULL);
    result = solar_os_native_result(error);
    if (!result && !*ended) {
        const solar_os_native_mpeg_frame_v1_t view = {.y = frame.y, .cb = frame.cb, .cr = frame.cr,
            .y_bytes = (size_t)frame.height * frame.y_stride,
            .chroma_bytes = (size_t)((frame.height + 1) / 2) * frame.chroma_stride,
            .width = frame.width, .height = frame.height, .y_stride = frame.y_stride,
            .chroma_stride = frame.chroma_stride, .time = frame.time};
        result = visit(&view, user);
    }
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int mpeg_audio(solar_os_native_handle_t id, solar_os_native_mpeg_audio_fn visit,
    void *user, solar_os_native_cancel_fn cancel, void *cancel_user, bool *ended)
{
    if (!visit || !ended) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *ended = false;
    void *decoder;
    int result = solar_os_native_handle_acquire_exclusive(id, MPEG_HANDLE_KIND, &decoder);
    if (result) return result;
    solar_os_mpeg_set_cancel(decoder, cancel, cancel_user);
    solar_os_mpeg_audio_t audio;
    const esp_err_t error = solar_os_mpeg_audio(decoder, &audio, ended);
    solar_os_mpeg_set_cancel(decoder, NULL, NULL);
    result = solar_os_native_result(error);
    if (!result && !*ended) result = visit(&audio, user);
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int mpeg_raster(const solar_os_native_mpeg_frame_v1_t *frame, uint8_t *data,
    size_t capacity, uint32_t width, uint32_t height, bool color, bool simd)
{
    if (!frame || !data || !width || !height || !frame->y || !frame->cb || !frame->cr ||
        !frame->width || !frame->height || frame->y_stride < frame->width)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const uint32_t chroma_width = frame->width / 2 + frame->width % 2;
    const uint32_t chroma_height = frame->height / 2 + frame->height % 2;
    if (frame->chroma_stride < chroma_width) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (frame->y_bytes < frame->width || (frame->y_bytes - frame->width) / frame->y_stride < frame->height - 1 ||
        frame->chroma_bytes < chroma_width || (frame->chroma_bytes - chroma_width) / frame->chroma_stride < chroma_height - 1 ||
        capacity / (color ? 2U : 1U) / width < height || width > UINT32_MAX - frame->width || width > UINT32_MAX - 8U)
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    const solar_os_mpeg_frame_t source = {.y = frame->y, .cb = frame->cb, .cr = frame->cr,
        .width = frame->width, .height = frame->height, .y_stride = frame->y_stride,
        .chroma_stride = frame->chroma_stride, .time = frame->time};
    solar_os_mpeg_raster(&source, data, width, height, color, simd);
    return SOLAR_OS_NATIVE_OK;
}

static int mpeg_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, MPEG_HANDLE_KIND); }

static const solar_os_native_mpeg_api_v1_t mpeg_api = {
    .abi_version = SOLAR_OS_NATIVE_MPEG_ABI, .struct_size = sizeof(mpeg_api),
    .open = mpeg_open, .close = mpeg_close, .info = mpeg_info, .error_detail = mpeg_error_detail,
    .seek = mpeg_seek, .video = mpeg_video, .audio = mpeg_audio, .raster = mpeg_raster,
    .simd_available = solar_os_mpeg_simd_available, .simd_selftest = solar_os_mpeg_simd_selftest,
};
#endif

const void *solar_os_native_mpeg_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_mpeg_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
