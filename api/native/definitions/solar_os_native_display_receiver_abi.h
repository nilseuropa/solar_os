#pragma once
#include "solar_os_native_display_link_abi.h"

#define SOLAR_OS_NATIVE_DISPLAY_RECEIVER_SERVICE "display.receiver"
#define SOLAR_OS_NATIVE_DISPLAY_RECEIVER_ABI 1U

typedef struct {
    void *user;
    /* A positive result transfers bytes. Zero would block. A negative
     * result stops the pump with an I/O error. */
    int (*read)(void *user, uint8_t *data, size_t capacity);
    int (*write)(void *user, const uint8_t *data, size_t size);
    /* OK commits the frame. BUSY defers completion to a later pump. All
     * other results reject it. The framebuffer is borrowed for this call. */
    int (*present)(void *user, uint32_t format, const uint8_t *pixels, size_t size,
        uint16_t width, uint16_t height);
} solar_os_native_display_receiver_io_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* Advertise MONO1 and optionally RGB565_BE. The peer selects the format.
     * Input format, stride and frame_bytes are ignored. The receiver owns
     * both framebuffers. Boot ID must be nonzero. A zero idle timeout uses
     * one second. */
    int (*open)(const solar_os_native_display_link_info_v1_t *info, uint64_t boot,
        uint32_t packet_idle_us, solar_os_native_handle_t *receiver);
    /* Bounded work. Call with monotonic time in microseconds. I/O and
     * presentation callbacks run synchronously and are never retained. */
    int (*pump)(solar_os_native_handle_t receiver, uint64_t now_us,
        const solar_os_native_display_receiver_io_v1_t *io);
    /* Fence a previous connection with a different nonzero boot ID. */
    int (*reset)(solar_os_native_handle_t receiver, uint64_t boot);
    int (*close)(solar_os_native_handle_t receiver);
} solar_os_native_display_receiver_api_v1_t;
