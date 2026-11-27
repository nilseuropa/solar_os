#include "solar_os_native_display_receiver.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_DLP
#include "solar_os_dlp_receiver.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"

typedef struct {
    solar_os_dlp_receiver_t receiver;
    uint8_t *storage;
    uint64_t last_pump_us;
    const solar_os_native_display_receiver_io_v1_t *io;
} native_receiver_t;

static int receiver_destroy(void *opaque)
{
    native_receiver_t *state = opaque;
    solar_os_memory_free(state->storage);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int receiver_read(void *opaque, uint8_t *data, size_t size)
{
    native_receiver_t *state = opaque;
    return state->io->read(state->io->user, data, size);
}

static int receiver_write(void *opaque, const uint8_t *data, size_t size)
{
    native_receiver_t *state = opaque;
    return state->io->write(state->io->user, data, size);
}

static int receiver_present(void *opaque, unsigned format, const uint8_t *pixels,
    size_t size, uint16_t width, uint16_t height)
{
    native_receiver_t *state = opaque;
    switch (state->io->present(state->io->user, format, pixels, size, width, height)) {
    case SOLAR_OS_NATIVE_OK: return DLP_OK;
    case SOLAR_OS_NATIVE_ERROR_BUSY: return DLP_BUSY;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: return DLP_TIMEOUT;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return DLP_NO_MEMORY;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: return DLP_UNSUPPORTED;
    default: return DLP_IO_ERROR;
    }
}

static int receiver_open(const solar_os_native_display_link_info_v1_t *in, uint64_t boot,
    uint32_t packet_idle_us, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !boot || in->width > UINT16_MAX || in->height > UINT16_MAX ||
        in->preferred_format > DLP_RGB565_BE ||
        !(in->formats & DLP_FORMAT_BIT(DLP_MONO1)) ||
        (in->formats & ~(DLP_FORMAT_BIT(DLP_MONO1) | DLP_FORMAT_BIT(DLP_RGB565_BE))) ||
        (in->capabilities & (DLP_BRIGHTNESS | DLP_POWER_SAVE)))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    dlp_info_t info = {
        .width = (uint16_t)in->width, .height = (uint16_t)in->height,
        .preferred_format = (uint16_t)in->preferred_format,
        .formats = in->formats, .capabilities = in->capabilities,
        .max_clock_hz = in->max_clock_hz, .min_gap_us = in->min_gap_us,
        .max_rect_bytes = in->max_rect_bytes, .command_ms = in->command_ms,
        .present_ms = in->present_ms, .nominal_refresh_ms = in->nominal_refresh_ms,
    };
    memcpy(info.identity, in->identity, sizeof(info.identity));
    memcpy(info.model, in->model, sizeof(info.model));
    memcpy(info.firmware, in->firmware, sizeof(info.firmware));
    if (!dlp_info_valid(&info)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const unsigned largest = in->formats & DLP_FORMAT_BIT(DLP_RGB565_BE) ? DLP_RGB565_BE : DLP_MONO1;
    const uint64_t frame_bytes = (uint64_t)dlp_row_bytes(info.width, largest) * info.height;
    if (frame_bytes > SIZE_MAX / 2U) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    const size_t capacity = (size_t)frame_bytes;

    native_receiver_t *state = solar_os_memory_alloc(sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.display.receiver");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->storage = solar_os_memory_alloc(capacity * 2U,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.display.receiver.frames");
    state->last_pump_us = 0;
    state->io = NULL;
    if (!state->storage) {
        solar_os_memory_free(state);
        return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    const solar_os_dlp_receiver_io_t io = {.user = state, .packet_idle_us = packet_idle_us,
        .read = receiver_read, .write = receiver_write, .present = receiver_present};
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (solar_os_dlp_receiver_init(&state->receiver, &info, boot, state->storage, capacity, &io)) {
        result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_DLP_RECEIVER,
            state, receiver_destroy, out);
    }
    if (result) (void)receiver_destroy(state);
    return result;
}

static int receiver_pump(solar_os_native_handle_t handle, uint64_t now_us,
    const solar_os_native_display_receiver_io_v1_t *io)
{
    if (!io || !io->read || !io->write || !io->present) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *opaque;
    int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_DLP_RECEIVER, &opaque);
    if (result) return result;
    native_receiver_t *state = opaque;
    if (now_us < state->last_pump_us) {
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    } else {
        const solar_os_native_display_receiver_io_v1_t callbacks = *io;
        state->last_pump_us = now_us;
        state->io = &callbacks;
        const int outcome = solar_os_dlp_receiver_pump(&state->receiver, now_us);
        state->io = NULL;
        if (outcome == DLP_TIMEOUT) result = SOLAR_OS_NATIVE_ERROR_TIMEOUT;
        else if (outcome != DLP_OK) result = SOLAR_OS_NATIVE_ERROR_IO;
    }
    solar_os_native_handle_release_exclusive(handle);
    return result;
}

static int receiver_reset(solar_os_native_handle_t handle, uint64_t boot)
{
    if (!boot) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *opaque;
    int result = solar_os_native_handle_acquire_exclusive(handle, SOLAR_OS_NATIVE_HANDLE_DLP_RECEIVER, &opaque);
    if (result) return result;
    native_receiver_t *state = opaque;
    if (boot == state->receiver.device.boot) result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    else solar_os_dlp_receiver_reset(&state->receiver, boot);
    solar_os_native_handle_release_exclusive(handle);
    return result;
}

static int receiver_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_DLP_RECEIVER);
}

static const solar_os_native_display_receiver_api_v1_t display_receiver_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_RECEIVER_ABI,
    .struct_size = sizeof(display_receiver_api),
    .open = receiver_open, .pump = receiver_pump, .reset = receiver_reset, .close = receiver_close,
};
#endif

const void *solar_os_native_display_receiver_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_display_receiver_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
