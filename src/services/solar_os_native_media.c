#include "solar_os_native_media.h"
#include "solar_os_native_data.h"
#include "solar_os_native_emotion.h"
#include "solar_os_native_jet.h"
#include "solar_os_native_media_protocol.h"
#include "solar_os_native_mjpeg.h"
#include "solar_os_native_mpeg.h"
#include "solar_os_native_rtsp_auth.h"
#include "solar_os_native_rtsp_client.h"
#include "solar_os_native_speech.h"
#include <string.h>
#include "esp_err.h"
#include "solar_os_config.h"
#include "solar_os_native_image_abi.h"
#include "solar_os_native_media_abi.h"
#include "solar_os_native_vision_abi.h"
#include "solar_os_native_inference_abi.h"
#include "solar_os_memory.h"

static void *native_memory_alloc_class(size_t bytes, uint32_t memory_class, const char *tag)
{
    if (memory_class >= SOLAR_OS_MEMORY_CLASS_COUNT) return NULL;
    return solar_os_memory_alloc(bytes, memory_class, tag);
}

static void *native_memory_calloc_class(size_t count, size_t bytes, uint32_t memory_class,
                                        const char *tag)
{
    if (memory_class >= SOLAR_OS_MEMORY_CLASS_COUNT) return NULL;
    return solar_os_memory_calloc(count, bytes, memory_class, tag);
}

static void *native_memory_realloc_class(void *buffer, size_t bytes, uint32_t memory_class,
                                         const char *tag)
{
    if (!bytes) {
        solar_os_memory_free(buffer);
        return NULL;
    }
    if (memory_class >= SOLAR_OS_MEMORY_CLASS_COUNT) return NULL;
    return solar_os_memory_realloc(buffer, bytes, memory_class, tag);
}

static void *native_memory_alloc(size_t bytes)
{ return bytes ? native_memory_alloc_class(bytes, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.buffer") : NULL; }
static void *native_memory_alloc_internal(size_t bytes)
{
    if (!bytes) return NULL;
    void *buffer = solar_os_memory_alloc(bytes, SOLAR_OS_MEMORY_INTERNAL_PREFERRED, "native.hot");
    if (buffer && solar_os_memory_is_external(buffer)) { solar_os_memory_free(buffer); return NULL; }
    return buffer;
}
static bool native_memory_is_external(const void *buffer)
{ return solar_os_memory_is_external((void *)buffer); }
static size_t native_memory_free_bytes(bool external)
{
    solar_os_memory_status_t status;
    solar_os_memory_get_status(&status);
    return external ? status.external.free : status.internal.free;
}
static size_t native_memory_largest_free_bytes(bool external)
{
    solar_os_memory_status_t status;
    solar_os_memory_get_status(&status);
    return external ? status.external.largest_free : status.internal.largest_free;
}
static solar_os_native_memory_buffer_t *native_buffer_create_tagged(size_t bytes, const char *owner)
{ return (solar_os_native_memory_buffer_t *)solar_os_memory_buffer_create(bytes, owner); }
static solar_os_native_memory_buffer_t *native_buffer_create(size_t bytes)
{ return native_buffer_create_tagged(bytes, "native.movable"); }
static void *native_buffer_pin(solar_os_native_memory_buffer_t *buffer)
{ return solar_os_memory_buffer_pin((solar_os_memory_buffer_t *)buffer); }
static bool native_buffer_unpin(solar_os_native_memory_buffer_t *buffer)
{ return solar_os_memory_buffer_unpin((solar_os_memory_buffer_t *)buffer); }
static bool native_buffer_destroy(solar_os_native_memory_buffer_t *buffer)
{ return solar_os_memory_buffer_destroy((solar_os_memory_buffer_t *)buffer); }
static const solar_os_native_memory_api_v1_t memory_api = {
    .abi_version = SOLAR_OS_NATIVE_MEMORY_ABI, .struct_size = sizeof(memory_api),
    .alloc = native_memory_alloc, .free = solar_os_memory_free, .alloc_internal = native_memory_alloc_internal,
    .is_external = native_memory_is_external, .free_bytes = native_memory_free_bytes,
    .largest_free_bytes = native_memory_largest_free_bytes,
    .buffer_create = native_buffer_create, .buffer_pin = native_buffer_pin,
    .buffer_unpin = native_buffer_unpin, .buffer_destroy = native_buffer_destroy,
    .alloc_class = native_memory_alloc_class, .calloc_class = native_memory_calloc_class,
    .realloc_class = native_memory_realloc_class, .buffer_create_tagged = native_buffer_create_tagged,
};

#if SOLAR_OS_PACKAGE_SERVICE_IMAGE || SOLAR_OS_PACKAGE_SERVICE_SCRIPT_MEDIA || \
    SOLAR_OS_PACKAGE_SERVICE_VISION || SOLAR_OS_PACKAGE_SERVICE_IMLIB || SOLAR_OS_PACKAGE_SERVICE_INFERENCE || \
    SOLAR_OS_PACKAGE_DRIVER_HIMAX_HX6538
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

#if SOLAR_OS_PACKAGE_DRIVER_HIMAX_HX6538
#include "solar_os_camera.h"
#include "solar_os_sscma.h"
#include "himax_hx6538.h"
static int native_sscma_open(const char *source, const char *owner, uint32_t *out)
{
    if (!out || !source) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_camera_owner_t token = {0};
    esp_err_t error = solar_os_camera_acquire(owner, &token);
    if (!error) {
        const solar_os_sscma_exchange_t probe = {.source = source};
        error = solar_os_camera_control(&token, SOLAR_OS_NATIVE_SSCMA_SERVICE, &probe);
        if (error) (void)solar_os_camera_release_owner(&token);
        else *out = token.generation;
    }
    return native_result(error);
}
static int native_sscma_start(uint32_t generation)
{
    const solar_os_camera_owner_t token = {generation};
    const solar_os_camera_config_t config = solar_os_camera_default_config();
    return native_result(solar_os_camera_start(&token, &config));
}
static int native_sscma_exchange(uint32_t generation, const char *source,
    const char *command, const char *name, bool event, uint32_t timeout,
    solar_os_native_cancel_fn cancel, void *user, char **reply)
{
    if (!reply || !command) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *reply = NULL;
    const solar_os_camera_owner_t token = {generation};
    const solar_os_sscma_exchange_t request = {.source = source, .command = command,
        .response_name = name, .event = event, .timeout_ms = timeout, .cancel = cancel,
        .user = user, .reply = reply};
    return native_result(solar_os_camera_control(&token, SOLAR_OS_NATIVE_SSCMA_SERVICE, &request));
}
static int native_sscma_image(char *reply, size_t capacity, const uint8_t **jpeg,
    size_t *length, uint16_t *width, uint16_t *height)
{ return native_result(solar_os_himax_decode_image(reply, capacity, jpeg, length, width, height)); }
static int native_sscma_close(uint32_t generation)
{
    solar_os_camera_owner_t token = {generation};
    esp_err_t error = solar_os_camera_stop(&token);
    /* A disconnected chip cannot acknowledge stop. Invalidate its local
     * buffers/cache so the next owner performs full initialization, rather
     * than retaining the camera and module forever waiting for transport. */
    if (error) error = solar_os_camera_invalidate(&token);
    if (!error) error = solar_os_camera_release_owner(&token);
    return native_result(error);
}
static int native_sscma_spi(uint32_t generation, const void *tx, void *rx, size_t length)
{
    const solar_os_camera_owner_t token = {generation};
    const solar_os_sscma_spi_t request = {.tx = tx, .rx = rx, .length = length};
    return native_result(solar_os_camera_control(&token, SOLAR_OS_SSCMA_SPI_PROTOCOL, &request));
}
static int native_sscma_send(uint32_t generation, const char *source, const char *command)
{
    if (!source || !command || !*command) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_camera_owner_t token = {generation};
    const solar_os_sscma_exchange_t request = {.source = source, .command = command};
    return native_result(solar_os_camera_control(&token, SOLAR_OS_NATIVE_SSCMA_SERVICE, &request));
}
static int native_sscma_reset(uint32_t generation)
{
    const solar_os_camera_owner_t token = {generation};
    esp_err_t error = solar_os_camera_invalidate(&token);
    const solar_os_sscma_spi_t request = {.reset = true};
    if (!error) error = solar_os_camera_control(&token, SOLAR_OS_SSCMA_SPI_PROTOCOL, &request);
    return native_result(error);
}
static const solar_os_native_sscma_api_v1_t sscma_api = {
    .abi_version = SOLAR_OS_NATIVE_SSCMA_ABI, .struct_size = sizeof(sscma_api),
    .open = native_sscma_open, .start = native_sscma_start, .exchange = native_sscma_exchange,
    .image = native_sscma_image, .close = native_sscma_close,
    .spi = native_sscma_spi, .send = native_sscma_send, .reset = native_sscma_reset,
};
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
static int native_image_draw(const solar_os_raster_image_t *image, solar_os_gfx_t *gfx,
    int32_t x, int32_t y, uint32_t width, uint32_t height)
{ return native_result(solar_os_raster_image_draw(image, gfx, x, y, width, height)); }
static int native_image_present(solar_os_raster_image_t *image, solar_os_gfx_t *gfx,
    int32_t x, int32_t y, uint32_t width, uint32_t height)
{ return native_result(solar_os_raster_image_present(image, gfx, x, y, width, height)); }
static const solar_os_native_image_api_v1_t image_api = {
    .abi_version = SOLAR_OS_NATIVE_IMAGE_ABI, .struct_size = sizeof(image_api),
    .open = native_image_open, .decode = native_image_decode,
    .from_pixels = native_image_from_pixels, .retain = solar_os_raster_image_retain,
    .release = solar_os_raster_image_release, .pixels = native_image_pixels, .convert = native_image_convert,
    .draw = native_image_draw, .present = native_image_present,
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
    .operation_name = solar_os_imlib_operation_name, .is_transform = solar_os_imlib_is_transform,
    .option_allowed = solar_os_imlib_option_allowed,
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
    return native_result(solar_os_script_media_open_typed(session, id, SOLAR_OS_STREAM_TYPE_VIDEO, &o, source));
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
static int native_media_reset(solar_os_native_media_t *session)
{
    return native_result(solar_os_script_media_close_all(session));
}

static int native_media_save_frame(solar_os_native_media_t *session, uint32_t frame,
                                   const char *path, int *file_errno)
{
    return native_result(solar_os_script_media_save(session, frame, path, file_errno));
}

static int native_media_rtsp_open_tracks(solar_os_native_media_t *session,
    const char *url, bool video, bool audio, uint32_t *source)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    if (!source) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *source = 0;
    if (!video && !audio) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return native_result(solar_os_script_media_rtsp_open(session, url, video, audio, source));
#else
    (void)session; (void)url; (void)video; (void)audio;
    if (source) *source = 0;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int native_media_rtsp_open(solar_os_native_media_t *session, const char *url, uint32_t *source)
{
    return native_media_rtsp_open_tracks(session, url, true, false, source);
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
static int native_media_rtsp_status(solar_os_native_media_t *session, uint32_t source,
                                    solar_os_rtsp_client_status_t *status, bool *ended)
{
    if (status) *status = (solar_os_rtsp_client_status_t){0};
    if (ended) *ended = true;
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    const esp_err_t error = solar_os_script_media_rtsp_status(session, source, status, ended);
    if (error == ESP_OK) status->error = native_result(status->error);
    return native_result(error);
#else
    (void)session; (void)source;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int native_media_rtsp_ended(solar_os_native_media_t *session, uint32_t source, bool *ended)
{
    solar_os_rtsp_client_status_t status;
    return native_media_rtsp_status(session, source, &status, ended);
}

static int native_media_rtsp_lateness(solar_os_native_media_t *session, uint32_t source,
                                      uint32_t frame, int64_t *lateness_us)
{
    if (lateness_us) *lateness_us = 0;
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    return native_result(solar_os_script_media_rtsp_lateness(session, source, frame, lateness_us));
#else
    (void)session; (void)source; (void)frame;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int native_media_rtsp_normalize_url(const char *address, char *url, size_t capacity)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    return native_result(solar_os_rtsp_url_normalize(address, url, capacity));
#else
    (void)address;
    if (url && capacity) *url = 0;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int native_media_rtsp_redact_url(const char *url, char *label, size_t capacity)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    if (!label || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return native_result(solar_os_rtsp_url_redact(url, label, capacity));
#else
    (void)url;
    if (label && capacity) *label = 0;
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
    .reset = native_media_reset,
    .save_frame = native_media_save_frame,
    .rtsp_open_tracks = native_media_rtsp_open_tracks,
    .rtsp_status = native_media_rtsp_status,
    .rtsp_lateness = native_media_rtsp_lateness,
    .rtsp_normalize_url = native_media_rtsp_normalize_url,
    .rtsp_redact_url = native_media_rtsp_redact_url,
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
static int native_mode_parse(const char *name, solar_os_inference_mode_t *mode)
{ return native_result(solar_os_inference_mode_parse(name, mode)); }
static int native_dtype_parse(const char *name, solar_os_tensor_dtype_t *dtype)
{ return native_result(solar_os_tensor_dtype_parse(name, dtype)); }
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
    .mode_name = solar_os_inference_mode_name, .mode_parse = native_mode_parse,
    .dtype_name = solar_os_tensor_dtype_name, .dtype_parse = native_dtype_parse,
    .dtype_bytes = solar_os_tensor_dtype_bytes,
};
static const solar_os_native_tensor_image_api_v1_t tensor_image_api = {
    .abi_version = SOLAR_OS_NATIVE_TENSOR_IMAGE_ABI, .struct_size = sizeof(tensor_image_api),
    .defaults = solar_os_tensor_image_defaults, .inspect = native_tensor_inspect, .prepare = native_tensor_prepare,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_MEDIA_SAMPLE
#include "solar_os.h"
#include "solar_os_media_sample.h"
#include "solar_os_media_sample_source.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

static int media_sample_destroy(void *state)
{
    solar_os_media_sample_release(state);
    return SOLAR_OS_NATIVE_OK;
}

static int media_sample_handle(solar_os_media_sample_t *sample, solar_os_native_handle_t *out)
{
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE;
    const int result = owner ? solar_os_native_handle_create_owned(kind, sample, media_sample_destroy, owner, out) :
        solar_os_native_handle_create(kind, sample, media_sample_destroy, out);
    if (result) solar_os_media_sample_release(sample);
    return result;
}

static int media_sample_copy(uint32_t kind, const void *data, size_t length,
    const solar_os_media_sample_metadata_t *metadata, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_media_sample_t *sample = NULL;
    esp_err_t error;
    switch (kind) {
    case SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE: error = solar_os_media_sample_copy_image(data, length, metadata, &sample); break;
    case SOLAR_OS_NATIVE_MEDIA_SAMPLE_AUDIO: error = solar_os_media_sample_copy_audio(data, length, metadata, &sample); break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return error ? solar_os_native_result(error) : media_sample_handle(sample, out);
}

static int media_sample_open(uint32_t kind, const char *resolved_path, size_t max_bytes,
    solar_os_media_sample_cancel_fn cancel, void *user, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_media_sample_t *sample = NULL;
    esp_err_t error;
    switch (kind) {
    case SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE:
        error = solar_os_media_sample_open_image_cancellable(resolved_path, max_bytes, cancel, user, &sample);
        break;
    case SOLAR_OS_NATIVE_MEDIA_SAMPLE_AUDIO:
        error = solar_os_media_sample_open_audio_bounded(resolved_path, max_bytes, cancel, user, &sample);
        break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return error ? solar_os_native_result(error) : media_sample_handle(sample, out);
}

static int media_sample_read_source(const char *source, const solar_os_media_sample_source_options_t *options,
    solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
#if SOLAR_OS_PACKAGE_SERVICE_MEDIA_SAMPLE_SOURCE
    solar_os_media_sample_t *sample = NULL;
    const esp_err_t error = solar_os_media_sample_source_read(source, options, &sample);
    return error ? solar_os_native_result(error) : media_sample_handle(sample, out);
#else
    (void)source; (void)options;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int media_sample_visit(solar_os_native_handle_t id, solar_os_native_media_sample_visit_fn visit, void *user)
{
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_media_sample_t *sample = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE);
    if (!sample) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const solar_os_media_sample_image_t *image = solar_os_media_sample_image(sample);
    const solar_os_media_sample_audio_t *audio = solar_os_media_sample_audio(sample);
    const solar_os_native_media_sample_view_v1_t view = image ? (solar_os_native_media_sample_view_v1_t){
        .kind = SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE, .data = image->data, .length = image->length,
        .encoding = image->mime_type, .metadata = image->metadata,
    } : (solar_os_native_media_sample_view_v1_t){
        .kind = SOLAR_OS_NATIVE_MEDIA_SAMPLE_AUDIO, .data = audio->data, .length = audio->length,
        .encoding = audio->format, .metadata = audio->metadata,
    };
    const int result = visit(&view, user);
    solar_os_native_handle_release(id);
    return result;
}

static int media_sample_retain(solar_os_native_handle_t id, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_media_sample_t *sample = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE);
    if (!sample) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_media_sample_retain(sample);
    solar_os_native_handle_release(id);
    return media_sample_handle(sample, out);
}

static int media_sample_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE);
}

static const solar_os_native_media_sample_api_v1_t media_sample_api = {
    .abi_version = SOLAR_OS_NATIVE_MEDIA_SAMPLE_ABI, .struct_size = sizeof(media_sample_api),
    .copy = media_sample_copy, .open = media_sample_open, .read_source = media_sample_read_source,
    .visit = media_sample_visit, .retain = media_sample_retain, .close = media_sample_close,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_PIPELINE
#include "freertos/FreeRTOS.h"
#include "solar_os_pipeline.h"
#include "solar_os_processor.h"

#define PROCESSOR_HANDLE_KIND SOLAR_OS_NATIVE_HANDLE_PROCESSOR
_Static_assert(SOLAR_OS_NATIVE_PROCESSOR_UPLOAD_REPLACE == SOLAR_OS_PROCESSOR_UPLOAD_REPLACE, "processor upload flag");

enum {
    PROCESSOR_IDLE,
    PROCESSOR_ACTIVE,
    PROCESSOR_CANCELLING,
    PROCESSOR_CLOSING,
};

typedef struct {
    solar_os_processor_t *processor;
    unsigned activity;
} native_processor_t;

static portMUX_TYPE processor_lock = portMUX_INITIALIZER_UNLOCKED;

static bool processor_cancelled(void *opaque)
{
    const native_processor_t *state = opaque;
    portENTER_CRITICAL(&processor_lock);
    const bool cancelled = state->activity == PROCESSOR_CANCELLING;
    portEXIT_CRITICAL(&processor_lock);
    return cancelled;
}

static int processor_destroy(void *opaque)
{
    native_processor_t *state = opaque;
    portENTER_CRITICAL(&processor_lock);
    state->activity = PROCESSOR_CLOSING;
    portEXIT_CRITICAL(&processor_lock);
    const esp_err_t error = solar_os_processor_close_checked(state->processor);
    if (error) return error == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int processor_create(const char *kind, uint32_t model, uint32_t timeout, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    native_processor_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "processor.native");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const esp_err_t error = solar_os_processor_create(kind, model, timeout, processor_cancelled, state, &state->processor);
    if (error) {
        solar_os_memory_free(state);
        return solar_os_native_result(error);
    }
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const int result = owner ? solar_os_native_handle_create_owned(PROCESSOR_HANDLE_KIND, state,
        processor_destroy, owner, out) : solar_os_native_handle_create(PROCESSOR_HANDLE_KIND, state, processor_destroy, out);
    if (result) {
        solar_os_processor_destroy(state->processor);
        solar_os_memory_free(state);
    }
    return result;
}

static int processor_begin(solar_os_native_handle_t id, native_processor_t **out)
{
    *out = NULL;
    native_processor_t *state = solar_os_native_handle_acquire(id, PROCESSOR_HANDLE_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    portENTER_CRITICAL(&processor_lock);
    const unsigned activity = state->activity;
    if (activity == PROCESSOR_IDLE) state->activity = PROCESSOR_ACTIVE;
    portEXIT_CRITICAL(&processor_lock);
    if (activity != PROCESSOR_IDLE) {
        solar_os_native_handle_release(id);
        return activity == PROCESSOR_CLOSING ? SOLAR_OS_NATIVE_ERROR_INVALID_STATE : SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static void processor_end(solar_os_native_handle_t id, native_processor_t *state)
{
    portENTER_CRITICAL(&processor_lock);
    state->activity = PROCESSOR_IDLE;
    portEXIT_CRITICAL(&processor_lock);
    solar_os_native_handle_release(id);
}

static bool processor_captures(solar_os_native_handle_t id)
{
    native_processor_t *state = solar_os_native_handle_acquire(id, PROCESSOR_HANDLE_KIND);
    if (!state) return false;
    const bool result = solar_os_processor_captures(state->processor);
    solar_os_native_handle_release(id);
    return result;
}

static int processor_bind(solar_os_native_handle_t id, const char *source)
{
    native_processor_t *state;
    const int result = processor_begin(id, &state);
    if (result) return result;
    const esp_err_t error = solar_os_processor_bind_source(state->processor, source);
    processor_end(id, state);
    return solar_os_native_result(error);
}

static int processor_capture(solar_os_native_handle_t id, const char *source, uint64_t sequence,
    solar_os_native_handle_t *out, char **json)
{
    if (!out || !json) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    *json = NULL;
    native_processor_t *state;
    int result = processor_begin(id, &state);
    if (result) return result;
    solar_os_media_sample_t *sample;
    const esp_err_t error = solar_os_processor_capture(state->processor, source, sequence, &sample, json);
    if (error) result = solar_os_native_result(error);
    else {
        result = media_sample_handle(sample, out);
        if (result) {
            solar_os_memory_free(*json);
            *json = NULL;
        }
    }
    processor_end(id, state);
    return result;
}

static int processor_run_image(solar_os_native_handle_t id, solar_os_raster_image_t *image, char **json)
{
    if (!json) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *json = NULL;
    native_processor_t *state;
    const int result = processor_begin(id, &state);
    if (result) return result;
    const esp_err_t error = solar_os_processor_run(state->processor, image, json);
    processor_end(id, state);
    return solar_os_native_result(error);
}

static int processor_run_sample(solar_os_native_handle_t id, solar_os_native_handle_t sample,
    char **json, uint64_t *decode_us)
{
    if (!json || !decode_us) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *json = NULL;
    *decode_us = 0;
    native_processor_t *state;
    const int result = processor_begin(id, &state);
    if (result) return result;
    const solar_os_media_sample_t *image = solar_os_native_handle_acquire(sample, SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE);
    const esp_err_t error = image ? solar_os_processor_run_sample(state->processor, image, json, decode_us) : ESP_ERR_NOT_FOUND;
    if (image) solar_os_native_handle_release(sample);
    processor_end(id, state);
    return solar_os_native_result(error);
}

static int processor_cancel(solar_os_native_handle_t id)
{
    native_processor_t *state = solar_os_native_handle_acquire(id, PROCESSOR_HANDLE_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    portENTER_CRITICAL(&processor_lock);
    const bool requested = state->activity == PROCESSOR_ACTIVE || state->activity == PROCESSOR_CANCELLING;
    if (requested) state->activity = PROCESSOR_CANCELLING;
    portEXIT_CRITICAL(&processor_lock);
    solar_os_native_handle_release(id);
    return requested ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
}

static int processor_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, PROCESSOR_HANDLE_KIND); }

static int processor_query(const char *kind, const char *source, const char *query, char **json)
{ return solar_os_native_result(solar_os_processor_query(kind, source, query, json)); }
static int processor_select(const char *kind, const char *source, uint32_t slot,
    solar_os_native_cancel_fn cancel, void *user, char **json)
{ return solar_os_native_result(solar_os_processor_select(kind, source, slot, cancel, user, json)); }
static int processor_upload(const char *kind, const char *source, const char *bundle, uint32_t slot,
    uint32_t flags, solar_os_native_cancel_fn cancel, solar_os_native_processor_progress_fn progress, void *user, char **json)
{ return solar_os_native_result(solar_os_processor_upload(kind, source, bundle, slot, flags, cancel, progress, user, json)); }

static const solar_os_native_processor_api_v1_t processor_api = {
    .abi_version = SOLAR_OS_NATIVE_PROCESSOR_ABI, .struct_size = sizeof(processor_api),
    .kind_valid = solar_os_processor_kind_valid, .query = processor_query, .select = processor_select, .upload = processor_upload,
    .create = processor_create, .captures = processor_captures, .bind_source = processor_bind, .capture = processor_capture,
    .run_image = processor_run_image, .run_sample = processor_run_sample, .cancel = processor_cancel, .close = processor_close,
};

static int pipeline_start(const solar_os_pipeline_config_t *config, uint32_t *id)
{ return solar_os_native_result(solar_os_pipeline_start(config, id)); }

static int pipeline_list(uint32_t *ids, size_t capacity, size_t *count)
{ return solar_os_native_result(solar_os_pipeline_list(ids, capacity, count)); }

static int pipeline_status(uint32_t id, solar_os_pipeline_status_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_pipeline_status_t){0};
    const esp_err_t error = solar_os_pipeline_status(id, out);
    if (!error) out->last_error = solar_os_native_result(out->last_error);
    return solar_os_native_result(error);
}

static int pipeline_result(uint32_t id, uint64_t after, char **json, solar_os_native_handle_t *out)
{
    if (out) *out = 0;
    solar_os_media_sample_t *sample = NULL;
    const esp_err_t error = solar_os_pipeline_result_sample(id, after, json, out ? &sample : NULL);
    if (error || !sample) return solar_os_native_result(error);
    const int result = media_sample_handle(sample, out);
    if (result && json) {
        solar_os_memory_free(*json);
        *json = NULL;
    }
    return result;
}

static int pipeline_stop(uint32_t id)
{ return solar_os_native_result(solar_os_pipeline_stop(id)); }

static int pipeline_destroy(uint32_t id)
{ return solar_os_native_result(solar_os_pipeline_destroy(id)); }

static const solar_os_native_pipeline_api_v1_t pipeline_api = {
    .abi_version = SOLAR_OS_NATIVE_PIPELINE_ABI, .struct_size = sizeof(pipeline_api),
    .start = pipeline_start, .list = pipeline_list, .status = pipeline_status, .result = pipeline_result,
    .stop = pipeline_stop, .destroy = pipeline_destroy,
};
#endif

const void *solar_os_native_media_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
    if (!name) return NULL;
#include "generated/native_media_providers.inc"
#include "generated/native_media_dispatch.inc"
    (void)abi; (void)minimum;
    return NULL;
}
