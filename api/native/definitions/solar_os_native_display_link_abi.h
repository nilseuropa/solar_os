#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_DISPLAY_LINK_SERVICE "display.link"
#define SOLAR_OS_NATIVE_DISPLAY_LINK_ABI 1U

#define SOLAR_OS_NATIVE_DISPLAY_LINK_PREFERRED (-1)
#define SOLAR_OS_NATIVE_DISPLAY_LINK_MONO1 0U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_GRAY2 1U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_GRAY4 2U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_GRAY8 3U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_RGB565_BE 4U

#define SOLAR_OS_NATIVE_DISPLAY_LINK_PRESERVE_CONTENT 1U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_STAGED_FRAME 2U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_BRIGHTNESS 4U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_POWER_SAVE 8U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_READY 16U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_RLE 32U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_SYNC 64U
#define SOLAR_OS_NATIVE_DISPLAY_LINK_LZ4 128U

typedef struct {
    int32_t format;
    uint32_t timeout_ms;
    bool compressed;
} solar_os_native_display_link_options_v1_t;

typedef struct {
    uint32_t width, height, format, preferred_format, formats, capabilities;
    uint32_t max_clock_hz, min_gap_us, max_rect_bytes, command_ms, present_ms, nominal_refresh_ms;
    size_t stride, frame_bytes;
    uint8_t identity[16];
    char model[32], firmware[32];
} solar_os_native_display_link_info_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* The named endpoint must provide exclusive duplex bytes. NULL options
     * select the peer's preferred format, a 5 s timeout and raw/RLE encoding.
     * This call can return a cleanup-only handle with an error. Close any
     * nonzero handle on its owning task, including after an open failure. */
    int (*open)(const char *stream, const char *owner,
                const solar_os_native_display_link_options_v1_t *options,
                solar_os_native_cancel_fn cancel, void *user, solar_os_native_handle_t *link);
    int (*info)(solar_os_native_handle_t link, solar_os_native_display_link_info_v1_t *info);
    /* Present one complete packed image in the reported format and stride.
     * Pixel data and cancellation callbacks are borrowed only for this call. */
    int (*present)(solar_os_native_handle_t link, const void *pixels, size_t bytes,
                   solar_os_native_cancel_fn cancel, void *user);
    /* A refused close keeps the handle valid for retry. */
    int (*close)(solar_os_native_handle_t link);
} solar_os_native_display_link_api_v1_t;
