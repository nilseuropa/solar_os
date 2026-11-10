#pragma once

#include "solar_os_rtsp_receiver.h"
#include "solar_os_rtp_jpeg.h"

typedef struct solar_os_rtsp_client solar_os_rtsp_client_t;
#define SOLAR_OS_RTSP_CLIENT_ERROR_MAX 112U

typedef struct {
    bool video;
    bool audio;
    bool diagnostics;
    /* Consecutive transient failures to retry (0 preserves single-session
     * behavior). Backoff is cancellable, 500 ms doubling to at most 4 s. */
    uint8_t reconnect_attempts;
    /* Called by the audio owner after PCM has been submitted to the output.
     * Valid until run() returns; widgets must outlive that call. */
    void (*samples)(const int16_t *, size_t, uint8_t, void *);
    void *user;
} solar_os_rtsp_client_options_t;

typedef struct {
    bool negotiated, playing, audio_playing;
    bool video, audio;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t video_frames, video_dropped, audio_dropped;
    uint32_t epoch, reconnects;
    bool reconnecting;
    uint32_t audio_stack_min_free;
    /* Optional diagnostics. Submission gaps are not hardware underrun counts. */
    uint32_t audio_blocks, audio_output_frames, audio_output_rate;
    uint32_t audio_write_max_us, audio_gap_max_us, audio_wait_polls;
    uint32_t audio_concealed, audio_concealed_frames, audio_queued;
    uint32_t audio_rebuffers;
    uint16_t audio_block_frames;
    uint8_t audio_output_channels;
    esp_err_t error;
    /* First causal failure, including operation, socket errno or RTSP status.
     * Bounded snapshot; never contains URL credentials or received payloads. */
    char error_detail[SOLAR_OS_RTSP_CLIENT_ERROR_MAX];
} solar_os_rtsp_client_status_t;

esp_err_t solar_os_rtsp_client_create(const char *url,
                                      const solar_os_rtsp_client_options_t *options,
                                      solar_os_rtsp_client_t **client);
/* Single-use blocking, cancellable operation; call on a network worker. Owns
 * RTSP/UDP sockets and an optional audio worker until it returns. Optional
 * retries happen inside this call. Consumers enabling retries must discard
 * decoded frames whose status epoch no longer matches their acquisition. */
esp_err_t solar_os_rtsp_client_run(solar_os_rtsp_client_t *client);
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
void solar_os_rtsp_client_release_video(solar_os_rtsp_client_t *client);
/* Presentation clock for a decoded frame: negative means wait, positive means
 * late. Uses RTCP/audio when available, otherwise a bounded arrival delay. */
int64_t solar_os_rtsp_client_video_lateness(solar_os_rtsp_client_t *client,
                                           uint32_t timestamp, uint64_t arrived_us);
/* Only after run() returned and the final video lease was released. */
esp_err_t solar_os_rtsp_client_destroy(solar_os_rtsp_client_t *client);
