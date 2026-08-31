#include "solar_os_gameboy_presenter.h"

#include "solar_os_frame_presenter.h"
#include "solar_os_gameboy_video.h"
#include "solar_os_display.h"

static solar_os_frame_presenter_t *presenter;

static uint16_t gameboy_palette_rgb565[4] = {
    0xffffU,
    0xad55U,
    0x52aaU,
    0x0000U,
};

static void gameboy_refresh_palette(void)
{
    uint32_t foreground = 0x000000U;
    uint32_t background = 0xffffffU;
    (void)solar_os_display_get_colors(&foreground, &background);
    solar_os_gameboy_video_theme_palette(
        foreground, background, gameboy_palette_rgb565);
}

esp_err_t solar_os_gameboy_presenter_init(solar_os_gfx_t *gfx)
{
    if (presenter != NULL) {
        solar_os_gameboy_presenter_deinit();
        if (presenter != NULL) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    gameboy_refresh_palette();
    const solar_os_frame_presenter_config_t config = {
        .gfx = gfx,
        .format = SOLAR_OS_DISPLAY_FORMAT_INDEX2,
        .width = SOLAR_OS_GAMEBOY_BITMAP_WIDTH,
        .height = SOLAR_OS_GAMEBOY_BITMAP_HEIGHT,
        .stride = SOLAR_OS_GAMEBOY_BITMAP_STRIDE,
        .palette_rgb565 = gameboy_palette_rgb565,
        .palette_size = 4U,
        .preferred_fps = 25U,
        .fit = SOLAR_OS_FRAME_FIT_HEIGHT,
        .allow_mono_fallback = true,
        .request_high_refresh = true,
        .reverse_direct_palette = true,
        .clear_background_on_resume = true,
        .background_index = 0U,
    };
    return solar_os_frame_presenter_init(&presenter, &config);
}

esp_err_t solar_os_gameboy_presenter_resume(void)
{
    gameboy_refresh_palette();
    return solar_os_frame_presenter_resume(presenter);
}

void solar_os_gameboy_presenter_suspend(void)
{
    solar_os_frame_presenter_suspend(presenter);
}

void solar_os_gameboy_presenter_deinit(void)
{
    if (solar_os_frame_presenter_deinit(presenter) == ESP_OK) {
        presenter = NULL;
    }
}

bool solar_os_gameboy_presenter_queue(const uint8_t *bitmap)
{
    return solar_os_frame_presenter_submit(
        presenter, bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
}

void solar_os_gameboy_presenter_take_stats(
    solar_os_gameboy_presenter_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    solar_os_frame_presenter_stats_t common = {0};
    solar_os_frame_presenter_take_stats(presenter, &common);
    *stats = (solar_os_gameboy_presenter_stats_t) {
        .present_us = common.present_us,
        .presented_frames = common.presented_frames,
        .dropped_frames = common.replaced_frames,
        .last_error = common.last_error,
    };
}
