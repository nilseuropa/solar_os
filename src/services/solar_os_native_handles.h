#pragma once
#include "solar_os_native_service_abi.h"

enum {
    SOLAR_OS_NATIVE_HANDLE_SHA256 = 1,
    SOLAR_OS_NATIVE_HANDLE_RANDOM,
    SOLAR_OS_NATIVE_HANDLE_PORT,
    SOLAR_OS_NATIVE_HANDLE_FILE,
    SOLAR_OS_NATIVE_HANDLE_DIRECTORY,
    SOLAR_OS_NATIVE_HANDLE_NET,
    SOLAR_OS_NATIVE_HANDLE_HTTP,
    SOLAR_OS_NATIVE_HANDLE_HTTP_RESPONSE,
    SOLAR_OS_NATIVE_HANDLE_WIFI_LEASE,
    SOLAR_OS_NATIVE_HANDLE_SSH,
    SOLAR_OS_NATIVE_HANDLE_SCP,
    SOLAR_OS_NATIVE_HANDLE_SFTPSYNC,
    SOLAR_OS_NATIVE_HANDLE_REMOTE_FILES,
    SOLAR_OS_NATIVE_HANDLE_STREAM,
    SOLAR_OS_NATIVE_HANDLE_PARAMETER_CLAIM,
    SOLAR_OS_NATIVE_HANDLE_RESOURCES,
    SOLAR_OS_NATIVE_HANDLE_GPIO,
    SOLAR_OS_NATIVE_HANDLE_PWM,
    SOLAR_OS_NATIVE_HANDLE_BUS,
    SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE,
    SOLAR_OS_NATIVE_HANDLE_BACKLIGHT_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_HAPTIC_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_CHARGER_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_DSP_FIR,
    SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR,
    SOLAR_OS_NATIVE_HANDLE_DSP_FFT,
    SOLAR_OS_NATIVE_HANDLE_EXPR,
    SOLAR_OS_NATIVE_HANDLE_TUI,
    SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER,
    SOLAR_OS_NATIVE_HANDLE_INPUT_KEYBOARD_CAPTURE,
    SOLAR_OS_NATIVE_HANDLE_INPUT_POINTER_CAPTURE,
    SOLAR_OS_NATIVE_HANDLE_INPUT_OBSERVER,
    SOLAR_OS_NATIVE_HANDLE_INPUT_FILTER,
    SOLAR_OS_NATIVE_HANDLE_INPUT_KEYMAP,
    SOLAR_OS_NATIVE_HANDLE_HID_REPORT,
    SOLAR_OS_NATIVE_HANDLE_PARAMETER_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_DISPLAY_EXPORT,
    SOLAR_OS_NATIVE_HANDLE_DISPLAY_LEASE,
    SOLAR_OS_NATIVE_HANDLE_CLOCK_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_RTC_LEASE,
    SOLAR_OS_NATIVE_HANDLE_RTC_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_AUDIO_DECODER,
    SOLAR_OS_NATIVE_HANDLE_AUDIO_PCM,
    SOLAR_OS_NATIVE_HANDLE_RADIO,
    SOLAR_OS_NATIVE_HANDLE_RADIO_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_MIDI_SUBSCRIPTION,
    SOLAR_OS_NATIVE_HANDLE_STREAM_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_MEMORY_RECLAIMER,
    SOLAR_OS_NATIVE_HANDLE_DOCUMENT,
    SOLAR_OS_NATIVE_HANDLE_DOCUMENT_LAYOUT,
    SOLAR_OS_NATIVE_HANDLE_STORAGE_BROWSER,
    SOLAR_OS_NATIVE_HANDLE_SSH_EXEC,
    SOLAR_OS_NATIVE_HANDLE_DISPLAY_LINK,
    SOLAR_OS_NATIVE_HANDLE_DISPLAY_LAYER,
    SOLAR_OS_NATIVE_HANDLE_GPIO_LINE,
    SOLAR_OS_NATIVE_HANDLE_SIGNAL_WIDGET,
    SOLAR_OS_NATIVE_HANDLE_GPIO_CONTROLLER_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_PACKAGE_CATALOG,
    SOLAR_OS_NATIVE_HANDLE_SENSOR_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_CAMERA,
    SOLAR_OS_NATIVE_HANDLE_CAMERA_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_MEDIA_SAMPLE,
    SOLAR_OS_NATIVE_HANDLE_NFC_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_PROCESSOR,
    SOLAR_OS_NATIVE_HANDLE_EMOTION,
    SOLAR_OS_NATIVE_HANDLE_MPEG,
    SOLAR_OS_NATIVE_HANDLE_JET,
    SOLAR_OS_NATIVE_HANDLE_SPEECH_WORKER,
    SOLAR_OS_NATIVE_HANDLE_QUEUE,
    SOLAR_OS_NATIVE_HANDLE_RTP_JPEG_RECEIVER,
    SOLAR_OS_NATIVE_HANDLE_RTSP_CLIENT,
    SOLAR_OS_NATIVE_HANDLE_MJPEG,
    SOLAR_OS_NATIVE_HANDLE_RADIO_SCAN,
    SOLAR_OS_NATIVE_HANDLE_MEDIA_PLAYLIST,
    SOLAR_OS_NATIVE_HANDLE_SPI_DEVICE,
    SOLAR_OS_NATIVE_HANDLE_DISPLAY_PROVIDER,
};
typedef int (*solar_os_native_handle_destroy_fn)(void *state);
int solar_os_native_handle_create(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, solar_os_native_handle_t *out);
void *solar_os_native_handle_acquire(solar_os_native_handle_t id, uint32_t kind);
/* Borrow a handle for one operation. Shared and exclusive borrows, and close,
 * cannot overlap an exclusive borrow. */
int solar_os_native_handle_acquire_exclusive(solar_os_native_handle_t id, uint32_t kind, void **state);
void solar_os_native_handle_release_exclusive(solar_os_native_handle_t id);
void solar_os_native_handle_release(solar_os_native_handle_t id);
int solar_os_native_handle_close(solar_os_native_handle_t id);
int solar_os_native_handle_close_kind(solar_os_native_handle_t id, uint32_t kind);
uint32_t solar_os_native_handle_count(void);

/* Kernel-owned resources. The owner key must outlive every owned handle.
 * Quiesce creation before close_owner. Cleanup is last-created first and stops
 * on BUSY/failure, so the lifecycle can retry before freeing borrowed state. */
int solar_os_native_handle_create_owned(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, const void *owner, solar_os_native_handle_t *out);
void *solar_os_native_handle_acquire_owned(solar_os_native_handle_t id, uint32_t kind, const void *owner);
int solar_os_native_handle_close_owned(solar_os_native_handle_t id, uint32_t kind, const void *owner);
int solar_os_native_handle_close_owner(const void *owner);
