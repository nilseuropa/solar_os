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
const void *solar_os_native_media_widgets_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_media_widgets_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
