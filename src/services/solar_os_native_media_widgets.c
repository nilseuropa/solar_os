#include "solar_os_native_media_widgets.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_media_widgets_abi.h"
#if SOLAR_OS_PACKAGE_SERVICE_MEDIA_WIDGETS
#include "solar_os_media_widgets.h"
static const solar_os_native_media_widgets_api_v1_t media_widgets_api = {
    .abi_version = SOLAR_OS_NATIVE_MEDIA_WIDGETS_ABI, .struct_size = sizeof(media_widgets_api),
    .transport_button_draw = solar_os_media_transport_button_draw,
    .list_layout = solar_os_media_list_layout, .playlist_draw = solar_os_media_playlist_draw,
    .player_layout = solar_os_media_player_layout, .player_header_draw = solar_os_media_player_header_draw,
    .player_status_draw = solar_os_media_player_status_draw, .player_controls_draw = solar_os_media_player_controls_draw,
    .controls_draw = solar_os_media_controls_draw, .player_button_at = solar_os_media_player_button_at,
    .cassette_reset = solar_os_cassette_widget_reset, .cassette_update = solar_os_cassette_widget_update,
    .cassette_draw = solar_os_cassette_widget_draw,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SIGNAL_WIDGETS
#include "solar_os_signal_widgets.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"

static int signal_destroy(void *widget)
{
    solar_os_signal_widget_destroy(widget);
    return SOLAR_OS_NATIVE_OK;
}

static int signal_create(uint32_t kind, size_t samples, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_signal_widget_t *widget;
    const esp_err_t error = solar_os_signal_widget_create(kind, samples, &widget);
    if (error) return solar_os_native_result(error);
    const int status = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_SIGNAL_WIDGET, widget, signal_destroy, out);
    if (status) solar_os_signal_widget_destroy(widget);
    return status;
}

static int signal_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_SIGNAL_WIDGET); }

static int signal_reset(solar_os_native_handle_t id)
{
    solar_os_signal_widget_t *widget = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_SIGNAL_WIDGET);
    if (!widget) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_signal_widget_reset(widget);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int signal_submit(solar_os_native_handle_t id, const int16_t *samples, size_t frames, uint8_t channels)
{
    solar_os_signal_widget_t *widget = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_SIGNAL_WIDGET);
    if (!widget) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_signal_widget_submit_s16(widget, samples, frames, channels);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int signal_draw(solar_os_native_handle_t id, solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    if (!gfx || x < INT16_MIN || y < INT16_MIN || x > INT16_MAX || y > INT16_MAX ||
        width <= 0 || height <= 0 || width > INT16_MAX || height > INT16_MAX ||
        x + width - 1 > INT16_MAX || y + height - 1 > INT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_signal_widget_t *widget = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_SIGNAL_WIDGET);
    if (!widget) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_signal_widget_draw(widget, gfx, x, y, width, height);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_signal_widget_api_v1_t signal_widget_api = {
    .abi_version = SOLAR_OS_NATIVE_SIGNAL_WIDGET_ABI, .struct_size = sizeof(signal_widget_api),
    .create = signal_create, .close = signal_close, .reset = signal_reset,
    .submit_s16 = signal_submit, .draw = signal_draw,
};
#endif
const void *solar_os_native_media_widgets_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_media_widgets_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
