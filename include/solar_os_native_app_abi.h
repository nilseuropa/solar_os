#pragma once

#include "solar_os_key_codes.h"
#include "solar_os_native_service_abi.h"

/* Application lifecycle 1 is a synchronous command. Lifecycle 2 is a
 * foreground session whose ELF and guest state remain resident until close. */
#define SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI 2U
#define SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC 0x32505041U
#define SOLAR_OS_NATIVE_APP_MANIFEST_SYMBOL "solar_os_native_app_manifest"
#define SOLAR_OS_NATIVE_APP_RESUMABLE (1U << 0)
#define SOLAR_OS_NATIVE_APP_POINTER_EVENTS (1U << 1)
#define SOLAR_OS_NATIVE_APP_CLASS_TUI 2U
#define SOLAR_OS_NATIVE_APP_CLASS_GUI 3U

/* Fixed-width, pointer-free ELF metadata, checked before executing module
 * code. Memory minima are launch admission requirements, not reservations.
 * Keep internal allocations small; executable code and guest bulk live in
 * PSRAM. Optional caches should tolerate allocation failure. */
typedef struct {
    uint32_t magic, lifecycle_abi, struct_size;
    uint32_t app_class, flags;
    uint32_t worker_stack_bytes, worker_priority;
    uint32_t minimum_internal_bytes, minimum_psram_bytes;
    uint32_t tick_interval_ms;
    char id[32];
    char summary[96];
    /* Optional space-separated lowercase suffixes, e.g. ".gb". The app
     * receives the opened file as its first argument. Empty means no opener. */
    char file_extensions[64];
} solar_os_native_app_manifest_v2_t;

/* Export one const solar_os_native_app_manifest_v2_t with this declaration.
 * Its dynamic symbol lets load distinguish foreground apps from commands
 * independently of filenames and package directories. */
#ifdef __cplusplus
#define SOLAR_OS_NATIVE_APP_LINKAGE extern "C"
#else
#define SOLAR_OS_NATIVE_APP_LINKAGE
#endif
#define SOLAR_OS_NATIVE_APP_MANIFEST SOLAR_OS_NATIVE_APP_LINKAGE \
    __attribute__((used, visibility("default"))) \
    const solar_os_native_app_manifest_v2_t solar_os_native_app_manifest

#define SOLAR_OS_NATIVE_FOREGROUND_SERVICE "foreground"
#define SOLAR_OS_NATIVE_FOREGROUND_ABI 1U
#define SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS 0U
#define SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE 1U
#define SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT 2U

typedef struct {
    uint16_t usage, physical_key;
    uint8_t key, modifiers, action, source;
    bool reset;
} solar_os_native_foreground_key_v1_t;

/* Logical taps (usage == physical_key == 0) receive a synthetic release after
 * a 140 ms pulse measured from read_key consumption. Physical key streams keep
 * their actual release timing. A reset clears all held and pending inputs. */

#define SOLAR_OS_NATIVE_POINTER_ABSOLUTE 0U
#define SOLAR_OS_NATIVE_POINTER_RELATIVE 1U
#define SOLAR_OS_NATIVE_POINTER_MOVE 0U
#define SOLAR_OS_NATIVE_POINTER_PRESS 1U
#define SOLAR_OS_NATIVE_POINTER_RELEASE 2U
typedef struct {
    int16_t x, y, delta_x, delta_y;
    uint8_t source, pointer_id, buttons, mode, action;
    bool reset;
} solar_os_native_foreground_pointer_v1_t;
typedef struct {
    uint16_t year;
    uint8_t month, day, weekday, hour, minute, second;
} solar_os_native_foreground_calendar_v1_t;

typedef void (*solar_os_native_foreground_audio_render_v1_t)(int16_t *samples,
    size_t frames, uint32_t sample_rate, void *user);
typedef struct {
    uint64_t present_us;
    uint32_t frames, replaced_frames;
} solar_os_native_foreground_video_stats_v1_t;

/* Calls belong to the app's worker. Presentation copies submitted pixels;
 * audio closes on its opening task. Poll stop and suspend at guest safe
 * points, including while paused. suspend quiesces host-owned foreground
 * resources and blocks until resume or close, retaining guest state and ELF.
 * After resume, clear held inputs, rebase guest timing, reopen optional audio
 * and submit a full frame. No module callback may survive ELF unload. */
typedef struct {
    uint32_t abi_version, struct_size;
    uint64_t (*time_us)(void);
    void (*sleep_ms)(uint32_t ms);
    void (*restart)(void);
    int (*video_open)(uint16_t width, uint16_t height,
        const uint16_t *palette_rgb565, uint16_t preferred_fps);
    int (*present)(const uint8_t *pixels, size_t length);
    int (*read_key)(solar_os_native_foreground_key_v1_t *key, bool *available);
    int (*file_size)(const char *path, uint32_t *size);
    int (*read_file)(const char *path, uint32_t offset, void *buffer,
        size_t length, size_t *read_length);
    int (*replace_file)(const char *path, const void *buffer, size_t length);
    int (*audio_open)(uint32_t *sample_rate, uint8_t *channels);
    int (*audio_write)(const int16_t *samples, size_t frames);
    void (*audio_close)(void);
    void (*set_exit_code)(int code);
    bool (*stop_requested)(void);
    int (*text_colors)(uint16_t *foreground, uint16_t *background);
    int (*video_open_ex)(uint16_t width, uint16_t height,
        const uint16_t *palette_rgb565, uint16_t preferred_fps,
        uint8_t background_index);
    int (*resolve_path)(const char *path, char *resolved, size_t resolved_size);
    int (*enable_suspend)(void);
    bool (*suspend_requested)(void);
    int (*suspend)(void);
    /* Appended methods. INDEX2 uses four shades of the current display theme.
     * Synth callbacks run under the host mutex on the audio worker. The app
     * uses synth_lock/unlock around shared synth state. close joins callbacks;
     * the mutex survives suspend and is deleted before module unload. */
    int (*video_open_index2)(uint16_t width, uint16_t height, uint16_t fps);
    int (*synth_open)(solar_os_native_foreground_audio_render_v1_t render,
        void *user, size_t block_frames, uint32_t sample_rate);
    bool (*synth_lock)(void);
    void (*synth_unlock)(void);
    void (*synth_close)(void);
    int (*rtc_read)(uint8_t registers[5]);
    uint8_t (*buttons_source)(void);
    int (*video_stats)(solar_os_native_foreground_video_stats_v1_t *stats);
    /* Rotated foreground surface. Valid before video_open; dimensions
     * describe the caller's selected display. */
    int (*video_dimensions)(uint16_t *width, uint16_t *height);
    /* Exclusive fixed-size raw disk images. Byte offsets permit header reads;
     * transfers are bounded to 64 KiB. Writes sync before returning. Claims
     * survive suspension and the host closes leaked handles on worker exit. */
    int (*disk_open)(const char *path, uint32_t *handle, uint32_t *size);
    int (*disk_read)(uint32_t handle, uint32_t offset, void *data, size_t length);
    int (*disk_write)(uint32_t handle, uint32_t offset, const void *data, size_t length);
    int (*disk_sync)(uint32_t handle);
    int (*disk_close)(uint32_t handle);
    /* Opt-in pointer queue, routed to this app's selected display. Buttons
     * are a held-state mask (bit 0 left, bit 1 right). Absolute coordinates
     * use the rotated host surface; relative deltas are unscaled. On queue
     * loss or session switches, reset releases buttons and position anchors. */
    int (*read_pointer)(solar_os_native_foreground_pointer_v1_t *pointer, bool *available);
    /* Host local wall clock. Guest writes must never alter the host clock. */
    int (*calendar_read)(solar_os_native_foreground_calendar_v1_t *calendar);
    /* Optional host-owned frame buffers. Acquire once after video_open, then
     * write a complete frame and exchange to queue it without copying pixels.
     * Exchange replaces *pixels with the next writable buffer. Previous
     * contents of that buffer are unspecified. The submitted buffer is read-only
     * until the next exchange, then must no longer be accessed. Never free these
     * buffers or mix exchange with present. Buffers and the acquired writing
     * pointer survive suspend; the host restores the last frame on resume and
     * frees all buffers after main returns. */
    int (*video_acquire)(uint8_t **pixels, size_t *length);
    int (*video_exchange)(uint8_t **pixels);
    /* CPU-work checkpoint. The host can coalesce successive requests to yield
     * periodically; use sleep_ms for timed waits. Keep polling stop/suspend. */
    void (*cooperate)(void);
} solar_os_native_foreground_api_v1_t;

static inline bool solar_os_native_foreground_has_exchange(
    const solar_os_native_foreground_api_v1_t *app)
{
    return app && app->struct_size >=
        offsetof(solar_os_native_foreground_api_v1_t, video_exchange) + sizeof(app->video_exchange) &&
        app->video_acquire && app->video_exchange;
}

static inline void solar_os_native_foreground_cooperate(
    const solar_os_native_foreground_api_v1_t *app)
{
    if (app->struct_size >= offsetof(solar_os_native_foreground_api_v1_t, cooperate) +
        sizeof(app->cooperate) && app->cooperate)
        app->cooperate();
    else
        app->sleep_ms(1);
}
