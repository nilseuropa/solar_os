#pragma once

#include "solar_os_rtsp_receiver.h"
#include "solar_os_rtp_jpeg.h"
#include "solar_os_audio.h"
#include "solar_os_native_media_abi.h"

typedef struct solar_os_rtsp_client solar_os_rtsp_client_t;

typedef void (*solar_os_rtsp_client_samples_fn)(const int16_t *, size_t, uint8_t, void *);
typedef void (*solar_os_rtsp_client_device_fn)(const solar_os_audio_device_info_t *, void *);

typedef struct {
    solar_os_rtsp_client_samples_fn samples;
    solar_os_rtsp_client_device_fn device;
    void *user;
} solar_os_rtsp_client_callbacks_t;

typedef struct {
    bool video;
    bool audio;
    bool diagnostics;
    /* Consecutive transient failures to retry (0 preserves single-session
     * behavior). Backoff is cancellable, 500 ms doubling to at most 4 s. */
    uint8_t reconnect_attempts;
    /* Called by the audio owner after PCM has been submitted to the output.
     * Valid until run() returns; widgets must outlive that call. */
    solar_os_rtsp_client_samples_fn samples;
    void *user;
    /* Optional app-owned output level. Other clients retain global volume. */
    bool use_volume;
    uint8_t volume;
    /* Attach on audio open, detach with NULL before releasing the output. */
    solar_os_rtsp_client_device_fn device;
} solar_os_rtsp_client_options_t;

esp_err_t solar_os_rtsp_client_create(const char *url,
                                      const solar_os_rtsp_client_options_t *options,
                                      solar_os_rtsp_client_t **client);
/* Single-use blocking, cancellable operation; call on a network worker. Owns
 * RTSP/UDP sockets and an optional audio worker until it returns. Optional
 * retries happen inside this call. Consumers enabling retries must discard
 * decoded frames whose status epoch no longer matches their acquisition. */
esp_err_t solar_os_rtsp_client_run(solar_os_rtsp_client_t *client);
/* Use these callbacks for this run. The audio owner joins before return. */
esp_err_t solar_os_rtsp_client_run_with_callbacks(solar_os_rtsp_client_t *client,
    const solar_os_rtsp_client_callbacks_t *callbacks);
void solar_os_rtsp_client_cancel(solar_os_rtsp_client_t *client);
/* Local live pause: keep receiving, discard audio, and resume at the live edge. */
void solar_os_rtsp_client_set_paused(solar_os_rtsp_client_t *client, bool paused);
void solar_os_rtsp_client_status(solar_os_rtsp_client_t *client,
                                 solar_os_rtsp_client_status_t *status);
/* One compressed frame, no queue. Take immediately for decoding, not at its
 * presentation deadline. Reception drops video while the decoder holds it.
 * Optional arrived_us receives the completed frame's monotonic arrival time. */
bool solar_os_rtsp_client_take_video(solar_os_rtsp_client_t *client,
                                    solar_os_rtp_jpeg_frame_t *frame, uint64_t *arrived_us);
bool solar_os_rtsp_client_take_video_with_epoch(solar_os_rtsp_client_t *client,
    solar_os_rtp_jpeg_frame_t *frame, uint64_t *arrived_us, uint32_t *epoch);
void solar_os_rtsp_client_release_video(solar_os_rtsp_client_t *client);
/* Presentation clock for a decoded frame: negative means wait, positive means
 * late. Uses RTCP/audio when available, otherwise a bounded arrival delay. */
int64_t solar_os_rtsp_client_video_lateness(solar_os_rtsp_client_t *client,
                                           uint32_t timestamp, uint64_t arrived_us);
/* Only after run() returned and the final video lease was released. */
esp_err_t solar_os_rtsp_client_destroy(solar_os_rtsp_client_t *client);
