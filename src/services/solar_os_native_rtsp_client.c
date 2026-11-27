#include "solar_os_native_rtsp_client.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
#include "solar_os.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_rtsp_client_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_rtsp_client.h"

#define RTSP_CLIENT_KIND SOLAR_OS_NATIVE_HANDLE_RTSP_CLIENT

static int client_destroy(void *opaque)
{ return solar_os_native_result(solar_os_rtsp_client_destroy(opaque)); }

static int client_open(const char *url, const solar_os_native_rtsp_options_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_rtsp_client_options_t options = {.video = in->video, .audio = in->audio,
        .diagnostics = in->diagnostics, .use_volume = in->use_volume, .volume = in->volume,
        .reconnect_attempts = in->reconnect_attempts};
    solar_os_rtsp_client_t *client;
    int result = solar_os_native_result(solar_os_rtsp_client_create(url, &options, &client));
    if (result) return result;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    result = owner ? solar_os_native_handle_create_owned(RTSP_CLIENT_KIND, client, client_destroy, owner, out) :
        solar_os_native_handle_create(RTSP_CLIENT_KIND, client, client_destroy, out);
    if (result) (void)client_destroy(client);
    return result;
}

static void client_samples(const int16_t *samples, size_t count, uint8_t channels, void *opaque)
{
    const solar_os_native_rtsp_callbacks_v1_t *callbacks = opaque;
    callbacks->samples(samples, count, channels, callbacks->user);
}

static void client_device(const solar_os_audio_device_info_t *device, void *opaque)
{
    const solar_os_native_rtsp_callbacks_v1_t *callbacks = opaque;
    callbacks->device(device ? device->id : NULL, callbacks->user);
}

static int client_run(solar_os_native_handle_t id, const solar_os_native_rtsp_callbacks_v1_t *in)
{
    solar_os_rtsp_client_t *client = solar_os_native_handle_acquire(id, RTSP_CLIENT_KIND);
    if (!client) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_native_rtsp_callbacks_v1_t callbacks = in ? *in : (solar_os_native_rtsp_callbacks_v1_t){0};
    const solar_os_rtsp_client_callbacks_t worker = {.samples = callbacks.samples ? client_samples : NULL,
        .device = callbacks.device ? client_device : NULL, .user = &callbacks};
    const esp_err_t error = solar_os_rtsp_client_run_with_callbacks(client, &worker);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int client_cancel(solar_os_native_handle_t id)
{
    solar_os_rtsp_client_t *client = solar_os_native_handle_acquire(id, RTSP_CLIENT_KIND);
    if (!client) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_rtsp_client_cancel(client);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int client_set_paused(solar_os_native_handle_t id, bool paused)
{
    solar_os_rtsp_client_t *client = solar_os_native_handle_acquire(id, RTSP_CLIENT_KIND);
    if (!client) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_rtsp_client_set_paused(client, paused);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int client_status(solar_os_native_handle_t id, solar_os_rtsp_client_status_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_rtsp_client_status_t){0};
    solar_os_rtsp_client_t *client = solar_os_native_handle_acquire(id, RTSP_CLIENT_KIND);
    if (!client) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_rtsp_client_status(client, out);
    solar_os_native_handle_release(id);
    out->error = solar_os_native_result(out->error);
    return SOLAR_OS_NATIVE_OK;
}

static int client_visit_video(solar_os_native_handle_t id, solar_os_native_rtsp_video_fn visit, void *user,
    bool *received)
{
    if (!received) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *received = false;
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_rtsp_client_t *client = solar_os_native_handle_acquire(id, RTSP_CLIENT_KIND);
    if (!client) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_native_rtsp_video_v1_t frame;
    int result = SOLAR_OS_NATIVE_OK;
    *received = solar_os_rtsp_client_take_video_with_epoch(client, &frame.jpeg, &frame.arrived_us, &frame.epoch);
    if (*received) {
        result = visit(&frame, user);
        solar_os_rtsp_client_release_video(client);
    }
    solar_os_native_handle_release(id);
    return result;
}

static int client_video_lateness(solar_os_native_handle_t id, uint32_t timestamp, uint64_t arrived_us,
    int64_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_rtsp_client_t *client = solar_os_native_handle_acquire(id, RTSP_CLIENT_KIND);
    if (!client) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = solar_os_rtsp_client_video_lateness(client, timestamp, arrived_us);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int client_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, RTSP_CLIENT_KIND); }

static const solar_os_native_rtsp_client_api_v1_t rtsp_client_api = {
    .abi_version = SOLAR_OS_NATIVE_RTSP_CLIENT_ABI, .struct_size = sizeof(rtsp_client_api),
    .open = client_open, .run = client_run, .cancel = client_cancel, .set_paused = client_set_paused,
    .status = client_status, .visit_video = client_visit_video, .video_lateness = client_video_lateness,
    .close = client_close,
};
#endif

const void *solar_os_native_rtsp_client_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_rtsp_client_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
