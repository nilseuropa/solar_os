#include "solar_os_mpeg.h"
#include "solar_os_memory.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *mpeg_alloc(size_t size)
{
    return size && size <= 1536U * 1024U
               ? solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "mpeg.decode")
               : NULL;
}
static void *mpeg_realloc(void *ptr, size_t size)
{
    return size && size <= 256U * 1024U
               ? solar_os_memory_realloc(ptr, size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                         "mpeg.buffer")
               : NULL;
}
#define PLM_MALLOC mpeg_alloc
#define PLM_REALLOC mpeg_realloc
#define PLM_FREE solar_os_memory_free
/* A PES packet can contain 65535 payload bytes plus its header. */
#define PLM_BUFFER_DEFAULT_SIZE (128U * 1024U)
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

struct solar_os_mpeg {
    FILE *file;
    plm_buffer_t *input, *video_buffer, *audio_buffer;
    plm_demux_t *demux;
    plm_video_t *video;
    plm_audio_t *audio;
    solar_os_mpeg_info_t info;
    solar_os_mpeg_cancel_t cancel;
    void *user;
    bool eof, video_pts_set, audio_pts_set;
    double video_pts, audio_pts, time_offset;
    uint32_t start_code;
    uint8_t sequence[4], sequence_bytes;
    uint32_t sequence_width, sequence_height, sequence_rate;
    esp_err_t error;
    bool video_pending, audio_pending;
    bool seek_audio_sync;
    double seek_audio_base;
    bool seek_video_sync;
    unsigned seek_video_reference;
    double seek_video_time;
    solar_os_mpeg_frame_t seek_video;
    solar_os_mpeg_audio_t seek_audio;
    char detail[96];
};

static void fail(solar_os_mpeg_t *d, esp_err_t error, const char *detail)
{
    if (d->error == ESP_OK) {
        d->error = error;
        snprintf(d->detail, sizeof(d->detail), "%s", detail);
    }
}
static bool check(solar_os_mpeg_t *d)
{
    if (d->cancel && d->cancel(d->user))
        fail(d, ESP_ERR_TIMEOUT, "playback cancelled");
    plm_buffer_t *buffers[] = {d->input, d->video_buffer, d->audio_buffer};
    for (unsigned i = 0; i < 3; i++) {
        int error = buffers[i] ? buffers[i]->error : 0;
        if (error == 1)
            fail(d, ESP_ERR_NO_MEM, "MPEG decoder PSRAM allocation failed");
        else if (error == 2)
            fail(d, ESP_ERR_INVALID_SIZE, "MPEG exceeds 640x480 or 256 KiB compressed-track limit");
        else if (error == 3)
            fail(d, ESP_FAIL, "MPEG file read failed");
        else if (error)
            fail(d, ESP_ERR_INVALID_ARG, "invalid MPEG-1 video data");
    }
    return d->error == ESP_OK;
}
static void load_file(plm_buffer_t *buffer, void *user)
{
    solar_os_mpeg_t *d = user;
    /* Demux seeking scans compressed bytes without load_track(). Keep those
     * scans cancellable too; the app's cancel callback also yields. */
    if (d->cancel && d->cancel(d->user)) {
        fail(d, ESP_ERR_TIMEOUT, "playback cancelled");
        plm_buffer_signal_end(buffer);
        return;
    }
    plm_buffer_load_file_callback(buffer, NULL);
}
static void load_track(plm_buffer_t *buffer, void *user)
{
    solar_os_mpeg_t *d = user;
    if (d->eof) {
        buffer->has_ended = TRUE;
        return;
    }
    while (!d->eof && check(d)) {
        plm_packet_t *packet = plm_demux_decode(d->demux);
        if (!packet) {
            d->eof = true;
            plm_buffer_signal_end(d->video_buffer);
            if (d->audio_buffer)
                plm_buffer_signal_end(d->audio_buffer);
            buffer->has_ended = TRUE;
            break;
        }
        plm_buffer_t *target = NULL;
        if (packet->type == PLM_DEMUX_PACKET_VIDEO_1) {
            target = d->video_buffer;
            if (!d->video_pts_set && packet->pts != PLM_PACKET_INVALID_TS) {
                d->video_pts = packet->pts;
                d->video_pts_set = true;
            }
            /* Sequence extensions identify MPEG-2, including across PES cuts. */
            for (size_t i = 0; i < packet->length; i++) {
                if (d->sequence_bytes) {
                    d->sequence[4 - d->sequence_bytes] = packet->data[i];
                    if (!--d->sequence_bytes) {
                        unsigned w = (d->sequence[0] << 4) | (d->sequence[1] >> 4);
                        unsigned h = ((d->sequence[1] & 15) << 8) | d->sequence[2];
                        unsigned rate = d->sequence[3] & 15;
                        if (d->sequence_width &&
                            (w != d->sequence_width || h != d->sequence_height ||
                             rate != d->sequence_rate)) {
                            fail(d, ESP_ERR_NOT_SUPPORTED,
                                 "MPEG sequence dimensions/frame rate cannot change during "
                                 "playback");
                            return;
                        }
                        d->sequence_width = w;
                        d->sequence_height = h;
                        d->sequence_rate = rate;
                    }
                }
                d->start_code = (d->start_code << 8) | packet->data[i];
                if (d->start_code == 0x000001b3U)
                    d->sequence_bytes = 4;
                if (d->start_code == 0x000001b5U) {
                    fail(d, ESP_ERR_NOT_SUPPORTED, "MPEG-2 video is not supported; use MPEG-1");
                    return;
                }
            }
        } else if (packet->type == PLM_DEMUX_PACKET_AUDIO_1 && d->audio_buffer) {
            target = d->audio_buffer;
            if (d->seek_audio_sync && packet->pts != PLM_PACKET_INVALID_TS) {
                d->seek_audio_base = packet->pts - d->audio_pts;
                d->seek_audio_sync = false;
                if (d->audio) plm_audio_set_time(d->audio, d->seek_audio_base);
            }
            if (!d->audio_pts_set && packet->pts != PLM_PACKET_INVALID_TS) {
                d->audio_pts = packet->pts;
                d->audio_pts_set = true;
            }
        }
        if (target && plm_buffer_write(target, packet->data, packet->length) != packet->length) {
            check(d);
            return;
        }
        if (target == buffer)
            return;
    }
}

void solar_os_mpeg_close(solar_os_mpeg_t *d)
{
    if (!d)
        return;
    plm_video_destroy(d->video);
    plm_audio_destroy(d->audio);
    plm_demux_destroy(d->demux);
    plm_buffer_destroy(d->video_buffer);
    plm_buffer_destroy(d->audio_buffer);
    plm_buffer_destroy(d->input);
    if (d->file)
        fclose(d->file);
    solar_os_memory_free(d);
}

esp_err_t solar_os_mpeg_open(const char *path, solar_os_mpeg_cancel_t cancel, void *user,
                             solar_os_mpeg_t **out, char *detail, size_t detail_size)
{
    if (!path || !out)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    solar_os_mpeg_t *d =
        solar_os_memory_calloc(1, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "mpeg.state");
    if (!d) {
        if (detail && detail_size)
            snprintf(detail, detail_size, "MPEG state PSRAM allocation failed");
        return ESP_ERR_NO_MEM;
    }
    d->cancel = cancel;
    d->user = user;
    d->file = fopen(path, "rb");
    if (!d->file) {
        fail(d, errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL, strerror(errno));
        goto error;
    }
    d->input = plm_buffer_create_with_file(d->file, FALSE);
    if (!d->input)
        goto memory;
    plm_buffer_set_load_callback(d->input, load_file, d);
    if (!check(d))
        goto error;
    d->demux = plm_demux_create(d->input, FALSE);
    if (!d->demux)
        goto memory;
    bool program_header = plm_demux_has_headers(d->demux);
    if (!check(d) || !program_header) {
        fail(d, ESP_ERR_NOT_SUPPORTED, "expected an MPEG-1 program stream (.mpg/.mpeg)");
        goto error;
    }
    if (plm_demux_get_num_video_streams(d->demux) != 1 ||
        plm_demux_get_num_audio_streams(d->demux) > 1) {
        fail(d, ESP_ERR_NOT_SUPPORTED, "expected one MPEG-1 video and at most one MP2 audio track");
        goto error;
    }
    d->video_buffer = plm_buffer_create_with_capacity(16384);
    if (!d->video_buffer)
        goto memory;
    plm_buffer_set_load_callback(d->video_buffer, load_track, d);
    if (plm_demux_get_num_audio_streams(d->demux)) {
        d->audio_buffer = plm_buffer_create_with_capacity(16384);
        if (!d->audio_buffer)
            goto memory;
        plm_buffer_set_load_callback(d->audio_buffer, load_track, d);
    }
    d->video = plm_video_create_with_buffer(d->video_buffer, FALSE);
    if (!d->video)
        goto memory;
    bool video_header = plm_video_has_header(d->video);
    if (!check(d) || !video_header) {
        fail(d, ESP_ERR_INVALID_ARG, "missing or truncated MPEG-1 video header");
        goto error;
    }
    if (d->audio_buffer) {
        d->audio = plm_audio_create_with_buffer(d->audio_buffer, FALSE);
        if (!d->audio)
            goto memory;
        bool audio_header = plm_audio_has_header(d->audio);
        if (!check(d) || !audio_header) {
            fail(d, ESP_ERR_NOT_SUPPORTED, "audio must be MPEG-1 Layer II (MP2)");
            goto error;
        }
    }
    if (d->audio_pts_set && d->video_pts_set) {
        const double wrap = (double)(1ULL << 33) / 90000.0;
        d->time_offset = d->video_pts - d->audio_pts;
        if (d->time_offset > wrap / 2)
            d->time_offset -= wrap;
        if (d->time_offset < -wrap / 2)
            d->time_offset += wrap;
        if (d->time_offset > 10 || d->time_offset < -10) {
            fail(d, ESP_ERR_NOT_SUPPORTED, "audio/video track start offset exceeds 10 seconds");
            goto error;
        }
    }
    d->info = (solar_os_mpeg_info_t){
        .width = d->video->width,
        .height = d->video->height,
        .fps = d->video->framerate,
        .audio = d->audio != NULL,
        .sample_rate = d->audio ? plm_audio_get_samplerate(d->audio) : 0,
    };
    *out = d;
    return ESP_OK;
memory:
    fail(d, ESP_ERR_NO_MEM, "MPEG decoder PSRAM allocation failed");
error:;
    esp_err_t err = d->error;
    if (detail && detail_size)
        snprintf(detail, detail_size, "%s", d->detail);
    solar_os_mpeg_close(d);
    return err;
}

void solar_os_mpeg_info(const solar_os_mpeg_t *d, solar_os_mpeg_info_t *info)
{
    if (d && info)
        *info = d->info;
}

void solar_os_mpeg_set_cancel(solar_os_mpeg_t *d, solar_os_mpeg_cancel_t cancel, void *user)
{
    if (!d) return;
    d->cancel = cancel;
    d->user = user;
}

const char *solar_os_mpeg_error(const solar_os_mpeg_t *d)
{
    return d ? d->detail : "MPEG decoder unavailable";
}
esp_err_t solar_os_mpeg_video(solar_os_mpeg_t *d, solar_os_mpeg_frame_t *frame, bool *ended)
{
    if (!d || !frame || !ended)
        return ESP_ERR_INVALID_ARG;
    *ended = false;
    if (!check(d))
        return d->error;
    if (d->video_pending) {
        *frame = d->seek_video;
        d->video_pending = false;
        return ESP_OK;
    }
    plm_frame_t *f = plm_video_decode(d->video);
    if (!check(d))
        return d->error;
    if (!f) {
        *ended = d->eof;
        if (!*ended)
            fail(d, ESP_ERR_INVALID_ARG, "MPEG video picture is truncated or malformed");
        return d->error;
    }
    if (d->seek_video_sync) {
        /* An open GOP may output its leading B pictures before the intra
         * picture whose PTS anchored the jump. Rebase once using their
         * temporal reference; these warm-up pictures are discarded. */
        double first = d->seek_video_time;
        if (d->video->picture_type == PLM_VIDEO_PICTURE_TYPE_B)
            first += ((int)d->video->temporal_reference - (int)d->seek_video_reference) / d->info.fps;
        f->time = first;
        plm_video_set_time(d->video, first + 1.0 / d->info.fps);
        d->seek_video_sync = false;
    }
    *frame = (solar_os_mpeg_frame_t){.y = f->y.data,
                                     .cb = f->cb.data,
                                     .cr = f->cr.data,
                                     .width = f->width,
                                     .height = f->height,
                                     .y_stride = f->y.width,
                                     .chroma_stride = f->cb.width,
                                     .time = f->time + d->time_offset};
    return ESP_OK;
}
esp_err_t solar_os_mpeg_audio(solar_os_mpeg_t *d, solar_os_mpeg_audio_t *audio, bool *ended)
{
    if (!d || !audio || !ended)
        return ESP_ERR_INVALID_ARG;
    *ended = d->audio == NULL;
    if (*ended)
        return ESP_OK;
    if (!check(d))
        return d->error;
    if (d->audio_pending) {
        *audio = d->seek_audio;
        d->audio_pending = false;
        return ESP_OK;
    }
    plm_samples_t *s = plm_audio_decode(d->audio);
    if (!check(d))
        return d->error;
    if (!s) {
        *ended = d->eof;
        if (!*ended)
            fail(d, ESP_ERR_INVALID_ARG, "MP2 audio frame is truncated or malformed");
        return d->error;
    }
    *audio = (solar_os_mpeg_audio_t){.samples = s->interleaved,
                                     .frames = s->count,
                                     .sample_rate = d->info.sample_rate,
                                     .time = s->time};
    return ESP_OK;
}

esp_err_t solar_os_mpeg_seek(solar_os_mpeg_t *d, double seconds, double *position)
{
    if (!d || !isfinite(seconds) || !position)
        return ESP_ERR_INVALID_ARG;
    if (!check(d)) return d->error;
    if (seconds < 0) seconds = 0;
    d->video_pending = d->audio_pending = false;
    plm_demux_rewind(d->demux);
    plm_video_rewind(d->video);
    d->eof = false;
    d->start_code = 0;
    d->sequence_bytes = 0;
    d->seek_audio_sync = false;
    d->seek_audio_base = 0;
    d->seek_video_sync = false;
    /* Search timestamped packets without reconstructing pixels. Restart from
     * an earlier intra picture, leaving warm-up for MP2 synthesis and video
     * reference pictures. Files without usable PTS use the sequential path. */
    if (seconds > 1.0 && d->video_pts_set && plm_buffer_get_size(d->input) >= 256) {
        double duration = plm_demux_get_duration(d->demux, PLM_DEMUX_PACKET_VIDEO_1);
        if (!check(d)) return d->error;
        if (isfinite(duration) && duration > 0) {
            /* Rewind/duration probing moves the input but leaves the demux's
             * last PTS stale. Its byte-position estimate must start at zero. */
            d->demux->last_decoded_pts = d->video_pts;
            plm_packet_t *packet = plm_demux_seek(d->demux,
                fmin(seconds - d->time_offset - 0.5, duration), PLM_DEMUX_PACKET_VIDEO_1, TRUE);
            if (!check(d)) return d->error;
            if (packet && packet->pts != PLM_PACKET_INVALID_TS) {
                plm_video_set_time(d->video, packet->pts - d->video_pts);
                d->seek_video_time = packet->pts - d->video_pts;
                for (size_t i = 0; i + 6 <= packet->length; i++) {
                    if (packet->data[i] == 0 && packet->data[i + 1] == 0 &&
                        packet->data[i + 2] == 1 && packet->data[i + 3] == 0) {
                        d->seek_video_reference = (packet->data[i + 4] << 2) | (packet->data[i + 5] >> 6);
                        d->seek_video_sync = true;
                        break;
                    }
                }
                plm_buffer_write(d->video_buffer, packet->data, packet->length);
                d->seek_audio_sync = d->audio != NULL;
            } else {
                plm_demux_rewind(d->demux);
            }
        }
    }
    if (d->audio) {
        /* Rewind alone retains the MP2 synthesis-filter history. Recreate
         * the small decoder so a backward seek cannot leak old samples. */
        plm_audio_destroy(d->audio);
        d->audio = NULL;
        plm_buffer_rewind(d->audio_buffer);
        d->audio = plm_audio_create_with_buffer(d->audio_buffer, FALSE);
        if (!d->audio) {
            fail(d, ESP_ERR_NO_MEM, "MP2 decoder allocation failed while seeking");
            return d->error;
        }
        if (!check(d)) return d->error;
        plm_audio_set_time(d->audio, d->seek_audio_base);
    }
    bool vend = false, aend = !d->audio, found = false;
    double audio_until = -1;
    solar_os_mpeg_frame_t frame = {0};
    solar_os_mpeg_audio_t audio = {0};
    /* Interleave both tracks while discarding, so neither compressed queue
     * grows with the seek distance. Never decode video without references. */
    while (!vend) {
        esp_err_t err = solar_os_mpeg_video(d, &frame, &vend);
        if (err != ESP_OK) return err;
        if (vend) break;
        found = true;
        while (!aend && audio_until < frame.time) {
            err = solar_os_mpeg_audio(d, &audio, &aend);
            if (err != ESP_OK) return err;
            if (!aend) audio_until = audio.time + (double)audio.frames / audio.sample_rate;
        }
        if (frame.time >= seconds) break;
    }
    if (!found) {
        fail(d, ESP_ERR_INVALID_ARG, "MPEG file contains no complete video pictures");
        return d->error;
    }
    *position = frame.time;
    d->seek_video = frame;
    d->video_pending = true;
    if (audio_until > frame.time && !aend) {
        uint32_t skip = frame.time > audio.time ?
            (uint32_t)((frame.time - audio.time) * audio.sample_rate) : 0;
        if (skip < audio.frames) {
            audio.samples += skip * 2U;
            audio.frames -= skip;
            audio.time += (double)skip / audio.sample_rate;
            d->seek_audio = audio;
            d->audio_pending = true;
        }
    }
    return ESP_OK;
}
