#include "solar_os_native_media.h"
#include <string.h>
#include "esp_err.h"
#include "solar_os_config.h"
#include "solar_os_native_image_abi.h"
#include "solar_os_native_media_abi.h"
#include "solar_os_native_vision_abi.h"
#include "solar_os_native_inference_abi.h"
#include "solar_os_memory.h"

static void *native_memory_alloc(size_t bytes)
{ return bytes ? solar_os_memory_alloc(bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.buffer") : NULL; }
static const solar_os_native_memory_api_v1_t memory_api = {
    .abi_version = SOLAR_OS_NATIVE_MEMORY_ABI, .struct_size = sizeof(memory_api),
    .alloc = native_memory_alloc, .free = solar_os_memory_free,
};

#if SOLAR_OS_PACKAGE_SERVICE_IMAGE || SOLAR_OS_PACKAGE_SERVICE_SCRIPT_MEDIA || \
    SOLAR_OS_PACKAGE_SERVICE_VISION || SOLAR_OS_PACKAGE_SERVICE_IMLIB || SOLAR_OS_PACKAGE_SERVICE_INFERENCE
static int native_result(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
#include "solar_os_raster_image.h"
static int native_image_open(const char *path, solar_os_raster_image_t **image)
{ return native_result(solar_os_raster_image_open(path, image)); }
static int native_image_decode(const uint8_t *data, size_t length, solar_os_raster_image_t **image)
{ return native_result(solar_os_raster_image_decode(data, length, image)); }
static int native_image_from_pixels(const uint8_t *data, size_t length,
    uint32_t width, uint32_t height, solar_os_raster_image_format_t format,
    size_t stride, solar_os_raster_image_t **image)
{ return native_result(solar_os_raster_image_from_pixels(data, length, width, height, format, stride, image)); }
static int native_image_pixels(const solar_os_raster_image_t *image, solar_os_raster_image_pixels_t *pixels)
{ return native_result(solar_os_raster_image_pixels(image, pixels)); }
static int native_image_convert(const solar_os_raster_image_t *image,
    const solar_os_raster_image_convert_options_t *options, uint8_t *data, size_t length, size_t stride)
{ return native_result(solar_os_raster_image_convert(image, options, data, length, stride)); }
static const solar_os_native_image_api_v1_t image_api = {
    .abi_version = SOLAR_OS_NATIVE_IMAGE_ABI, .struct_size = sizeof(image_api),
    .open = native_image_open, .decode = native_image_decode,
    .from_pixels = native_image_from_pixels, .retain = solar_os_raster_image_retain,
    .release = solar_os_raster_image_release, .pixels = native_image_pixels, .convert = native_image_convert,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_VISION
#include "solar_os_vision.h"
static int native_qrcodes(solar_os_raster_image_t *image,
    const solar_os_raster_image_convert_options_t *options,
    solar_os_native_cancel_fn cancel, void *user, solar_os_vision_qr_results_t **result)
{ return native_result(solar_os_vision_qrcodes(image, options, cancel, user, result)); }
static const solar_os_native_qr_api_v1_t qr_api = {
    .abi_version = SOLAR_OS_NATIVE_QR_ABI, .struct_size = sizeof(qr_api),
    .qrcodes = native_qrcodes, .result_free = solar_os_vision_qr_results_free,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_IMLIB
#include "solar_os_imlib.h"
static void native_imlib_defaults(solar_os_imlib_options_t *options)
{ if (options) *options = solar_os_imlib_default_options(); }
static int native_imlib_run(solar_os_raster_image_t *image, solar_os_raster_image_t *reference,
    solar_os_imlib_operation_t op, const solar_os_imlib_options_t *options,
    solar_os_native_cancel_fn cancel, void *user, solar_os_imlib_result_t **result)
{ return native_result(solar_os_imlib_run(image, reference, op, options, cancel, user, result)); }
static const solar_os_native_imlib_api_v1_t imlib_api = {
    .abi_version = SOLAR_OS_NATIVE_IMLIB_ABI, .struct_size = sizeof(imlib_api),
    .defaults = native_imlib_defaults, .run = native_imlib_run, .result_free = solar_os_imlib_result_free,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_MEDIA
#include "solar_os_script_media.h"
static int native_media_create(const char *owner, solar_os_native_cancel_fn cancel, void *user,
    solar_os_native_media_t **session)
{ return native_result(solar_os_script_media_create(owner, cancel, user, session)); }
static int native_media_destroy(solar_os_native_media_t *session)
{
    esp_err_t error = solar_os_script_media_close_all(session);
    if (error) return native_result(error);
    /* All IDs are cleared; destroy can now free without hiding provider errors. */
    solar_os_script_media_destroy(session);
    return SOLAR_OS_NATIVE_OK;
}
static int native_video_source(const char *id)
{
    solar_os_stream_info_t info;
    esp_err_t error = solar_os_stream_get_info(id, &info);
    if (error) return native_result(error);
    return info.type == SOLAR_OS_STREAM_TYPE_VIDEO ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
}
static int native_media_open(solar_os_native_media_t *session, const char *id,
    const solar_os_native_video_options_v1_t *options, uint32_t *source)
{
    if (!source) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *source = 0;
    solar_os_stream_open_options_t o = {.direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .timeout_ms = options ? options->timeout_ms : 5000,
        .requested_video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG,
            .width = options ? options->width : 0, .height = options ? options->height : 0,
            .jpeg_quality = options ? options->jpeg_quality : 0}};
    /* Check before opening: a video service must never start an unrelated UART,
     * microphone, or GPIO stream using video-union options. */
    int error = native_video_source(id);
    if (error) return error;
    return native_result(solar_os_script_media_open(session, id, &o, source));
}
static int native_media_close(solar_os_native_media_t *session, uint32_t source)
{ return native_result(solar_os_script_media_close(session, source)); }
static int native_media_acquire(solar_os_native_media_t *session, uint32_t source, uint32_t *frame)
{ return native_result(solar_os_script_media_acquire(session, source, frame)); }
static int native_media_frame(solar_os_native_media_t *session, uint32_t frame,
    solar_os_native_video_frame_v1_t *view)
{
    if (!view) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(view, 0, sizeof(*view));
    solar_os_script_media_frame_t f;
    esp_err_t error = solar_os_script_media_frame(session, frame, &f);
    if (!error) *view = (solar_os_native_video_frame_v1_t){.data = f.jpeg.data,
        .length = f.jpeg.length, .width = f.jpeg.width, .height = f.jpeg.height,
        .timestamp_us = f.jpeg.timestamp_us, .network = f.network,
        .rtp_timestamp = f.rtp_timestamp, .arrived_us = f.arrived_us};
    return native_result(error);
}
static int native_media_release(solar_os_native_media_t *session, uint32_t frame)
{ return native_result(solar_os_script_media_release(session, frame)); }
static int native_media_snapshot(solar_os_native_media_t *session, const char *id,
    uint16_t width, uint16_t height, uint8_t quality, uint32_t *frame)
{
    if (!frame) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *frame = 0;
    int error = native_video_source(id);
    return error ? error : native_result(solar_os_script_media_snapshot(session, id, width, height, quality, frame));
}
static int native_media_rtsp_open(solar_os_native_media_t *session, const char *url, uint32_t *source)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    return native_result(solar_os_script_media_rtsp_open(session, url, true, false, source));
#else
    (void)session; (void)url; if (source) *source = 0;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}
static int native_media_rtsp_read(solar_os_native_media_t *session, uint32_t source,
    uint32_t timeout_ms, uint32_t *frame)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    return native_result(solar_os_script_media_rtsp_read(session, source, timeout_ms, frame));
#else
    (void)session; (void)source; (void)timeout_ms; if (frame) *frame = 0;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}
static int native_media_rtsp_ended(solar_os_native_media_t *session, uint32_t source, bool *ended)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    solar_os_rtsp_client_status_t status;
    return native_result(solar_os_script_media_rtsp_status(session, source, &status, ended));
#else
    (void)session; (void)source; if (ended) *ended = true;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}
static const solar_os_native_media_api_v1_t media_api = {
    .abi_version = SOLAR_OS_NATIVE_MEDIA_ABI, .struct_size = sizeof(media_api),
    .capabilities = SOLAR_OS_NATIVE_MEDIA_LOCAL
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
        | SOLAR_OS_NATIVE_MEDIA_RTSP
#endif
    , .create = native_media_create, .destroy = native_media_destroy,
    .open = native_media_open, .close = native_media_close, .acquire = native_media_acquire,
    .frame = native_media_frame, .release_frame = native_media_release, .snapshot = native_media_snapshot,
    .rtsp_open = native_media_rtsp_open, .rtsp_read = native_media_rtsp_read, .rtsp_ended = native_media_rtsp_ended,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
#include "solar_os_inference.h"
#include "solar_os_model_bundle.h"
#include "solar_os_tensor_image.h"
static int native_inference_create(solar_os_native_cancel_fn cancel, void *user, solar_os_inference_t **client)
{ return native_result(solar_os_inference_create(cancel, user, client)); }
static int native_inference_load(solar_os_inference_t *client, const char *path, uint32_t timeout, uint32_t *model)
{ return native_result(solar_os_inference_load(client, path, timeout, model)); }
static int native_inference_load_bundle(solar_os_inference_t *client, const char *path, uint32_t timeout, uint32_t *model)
{ return native_result(solar_os_inference_load_bundle(client, path, timeout, model)); }
static int native_inference_list(uint32_t *models, size_t capacity, size_t *count)
{ return native_result(solar_os_inference_list(models, capacity, count)); }
static int native_inference_find(const char *path, uint32_t *model)
{ return native_result(solar_os_inference_find(path, model)); }
static int native_inference_info(solar_os_inference_t *client, uint32_t model, const solar_os_inference_model_info_t **info)
{ return native_result(solar_os_inference_info(client, model, info)); }
static int native_inference_run(solar_os_inference_t *client, uint32_t model,
    const solar_os_inference_input_t *inputs, size_t count, uint32_t timeout, solar_os_inference_result_t **result)
{ return native_result(solar_os_inference_run(client, model, inputs, count, timeout, result)); }
static int native_inference_run_bundle(solar_os_inference_t *client, uint32_t model,
    const solar_os_inference_value_t *inputs, size_t count, uint32_t timeout, solar_os_inference_result_t **result)
{ return native_result(solar_os_inference_run_bundle(client, model, inputs, count, timeout, result)); }
static int native_inference_reset(solar_os_inference_t *client, uint32_t model)
{ return native_result(solar_os_inference_reset(client, model)); }
static int native_inference_mode(solar_os_inference_t *client, uint32_t model, solar_os_inference_mode_t mode)
{ return native_result(solar_os_inference_set_mode(client, model, mode)); }
static int native_inference_unload(solar_os_inference_t *client, uint32_t model)
{ return native_result(solar_os_inference_close(client, model)); }
static int native_inference_unload_all(solar_os_inference_t *client)
{ return native_result(solar_os_inference_close_all(client)); }
static int native_inference_retain(uint32_t model)
{ return native_result(solar_os_inference_retain(model)); }
static int native_inference_release(uint32_t model)
{ return native_result(solar_os_inference_release(model)); }
static int native_tensor_inspect(const solar_os_raster_image_pixels_t *source,
    const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
    solar_os_tensor_image_transform_t *transform)
{ return native_result(solar_os_tensor_image_inspect(source, port, options, transform)); }
static int native_tensor_prepare(const solar_os_raster_image_pixels_t *source,
    const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
    uint8_t *data, size_t length, solar_os_tensor_image_transform_t *transform,
    solar_os_native_cancel_fn cancel, void *user)
{ return native_result(solar_os_tensor_image_prepare(source, port, options, data, length, transform, cancel, user)); }
static const solar_os_native_inference_api_v1_t inference_api = {
    .abi_version = SOLAR_OS_NATIVE_INFERENCE_ABI, .struct_size = sizeof(inference_api),
    .create = native_inference_create, .destroy = solar_os_inference_destroy,
    .load = native_inference_load, .load_bundle = native_inference_load_bundle,
    .list = native_inference_list, .find = native_inference_find, .info = native_inference_info,
    .run = native_inference_run, .run_bundle = native_inference_run_bundle,
    .reset = native_inference_reset, .set_mode = native_inference_mode,
    .unload = native_inference_unload, .unload_all = native_inference_unload_all,
    .retain = native_inference_retain, .release = native_inference_release,
    .result_free = solar_os_inference_result_free,
};
static const solar_os_native_tensor_image_api_v1_t tensor_image_api = {
    .abi_version = SOLAR_OS_NATIVE_TENSOR_IMAGE_ABI, .struct_size = sizeof(tensor_image_api),
    .defaults = solar_os_tensor_image_defaults, .inspect = native_tensor_inspect, .prepare = native_tensor_prepare,
};
#endif

const void *solar_os_native_media_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
    if (!name) return NULL;
#define OFFER(table, service, version) \
    if (!strcmp(name, service) && abi == version && minimum <= sizeof(table)) return &table
    OFFER(memory_api, SOLAR_OS_NATIVE_MEMORY_SERVICE, SOLAR_OS_NATIVE_MEMORY_ABI);
#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
    OFFER(image_api, SOLAR_OS_NATIVE_IMAGE_SERVICE, SOLAR_OS_NATIVE_IMAGE_ABI);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SCRIPT_MEDIA
    OFFER(media_api, SOLAR_OS_NATIVE_MEDIA_SERVICE, SOLAR_OS_NATIVE_MEDIA_ABI);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_VISION
    OFFER(qr_api, SOLAR_OS_NATIVE_QR_SERVICE, SOLAR_OS_NATIVE_QR_ABI);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_IMLIB
    OFFER(imlib_api, SOLAR_OS_NATIVE_IMLIB_SERVICE, SOLAR_OS_NATIVE_IMLIB_ABI);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
    OFFER(inference_api, SOLAR_OS_NATIVE_INFERENCE_SERVICE, SOLAR_OS_NATIVE_INFERENCE_ABI);
    OFFER(tensor_image_api, SOLAR_OS_NATIVE_TENSOR_IMAGE_SERVICE, SOLAR_OS_NATIVE_TENSOR_IMAGE_ABI);
#endif
#undef OFFER
    (void)abi; (void)minimum;
    return NULL;
}
