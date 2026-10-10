#include "solar_os_rtsp_receiver.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool copy_text(char *dst, size_t capacity, const char *src, size_t len)
{
    if (len >= capacity) return false;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static bool decimal(const char *text, uint32_t *number)
{
    if (!isdigit((unsigned char)*text)) return false;
    uint64_t n = 0;
    while (isdigit((unsigned char)*text)) {
        n = n * 10U + (unsigned)(*text++ - '0');
        if (n > UINT32_MAX) return false;
    }
    if (*text != '\0') return false;
    *number = (uint32_t)n;
    return true;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool percent_decode(char *dst, size_t capacity, const char *src, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= len) return false;
            int hi = hex_nibble(src[i + 1]), lo = hex_nibble(src[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (c <= 32 || c == 127) return false;
        if (o + 1 >= capacity) return false;
        dst[o++] = (char)c;
    }
    if (!o) return false;
    dst[o] = '\0';
    return true;
}

esp_err_t solar_os_rtsp_url_parse(const char *url, solar_os_rtsp_url_t *parsed)
{
    if (url == NULL || parsed == NULL || strncmp(url, "rtsp://", 7) != 0)
        return ESP_ERR_INVALID_ARG;
    memset(parsed, 0, sizeof(*parsed));
    for (const char *p = url; *p; p++)
        if ((unsigned char)*p <= 32 || *p == '#' || *p == '[' || *p == ']')
            return ESP_ERR_NOT_SUPPORTED;
    if (strlen(url) >= SOLAR_OS_RTSP_URL_MAX) return ESP_ERR_INVALID_SIZE;
    const char *start = url + 7, *slash = strchr(start, '/');
    const char *authority_end = slash != NULL ? slash : start + strlen(start);
    const char *at = memchr(start, '@', (size_t)(authority_end - start));
    const char *host_start = start;
    if (at != NULL) {
        const char *colon = memchr(start, ':', (size_t)(at - start));
        const char *user_end = colon != NULL ? colon : at;
        if (user_end == start ||
            !percent_decode(parsed->user, sizeof(parsed->user), start, (size_t)(user_end - start)))
            return ESP_ERR_INVALID_ARG;
        if (colon != NULL && !percent_decode(parsed->password, sizeof(parsed->password),
                                            colon + 1, (size_t)(at - colon - 1)) &&
            at != colon + 1)
            return ESP_ERR_INVALID_ARG;
        parsed->has_userinfo = true;
        host_start = at + 1;
    }
    const char *colon = memchr(host_start, ':', (size_t)(authority_end - host_start));
    const char *host_end = colon != NULL ? colon : authority_end;
    if (host_end == host_start || memchr(host_start, '@', (size_t)(authority_end - host_start)) ||
        !copy_text(parsed->host, sizeof(parsed->host), host_start, (size_t)(host_end - host_start)))
        return ESP_ERR_INVALID_ARG;
    parsed->port = 554;
    if (colon != NULL) {
        char port[6]; uint32_t n;
        if (!copy_text(port, sizeof(port), colon + 1, (size_t)(authority_end - colon - 1)) ||
            !decimal(port, &n) || n == 0 || n > 65535) return ESP_ERR_INVALID_ARG;
        parsed->port = (uint16_t)n;
    }
    return ESP_OK;
}

esp_err_t solar_os_rtsp_url_request_uri(const char *url, char *uri, size_t capacity)
{
    solar_os_rtsp_url_t parsed;
    esp_err_t err = solar_os_rtsp_url_parse(url, &parsed);
    if (err != ESP_OK) return err;
    const char *slash = strchr(url + 7, '/');
    const char *path = slash ? slash : "";
    int n = parsed.port == 554 ? snprintf(uri, capacity, "rtsp://%s%s", parsed.host, path) :
        snprintf(uri, capacity, "rtsp://%s:%u%s", parsed.host, parsed.port, path);
    if (n < 0 || (size_t)n >= capacity || (size_t)n >= SOLAR_OS_RTSP_URI_MAX)
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t solar_os_rtsp_url_redact(const char *url, char *text, size_t capacity)
{
    solar_os_rtsp_url_t parsed;
    esp_err_t err = solar_os_rtsp_url_parse(url, &parsed);
    if (err != ESP_OK) return err;
    const char *slash = strchr(url + 7, '/');
    const char *path = slash ? slash : "";
    int n;
    if (parsed.has_userinfo && parsed.port != 554)
        n = snprintf(text, capacity, "%s@%s:%u%s", parsed.user, parsed.host, parsed.port, path);
    else if (parsed.has_userinfo)
        n = snprintf(text, capacity, "%s@%s%s", parsed.user, parsed.host, path);
    else if (parsed.port != 554)
        n = snprintf(text, capacity, "%s:%u%s", parsed.host, parsed.port, path);
    else n = snprintf(text, capacity, "%s%s", parsed.host, path);
    return n > 0 && (size_t)n < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_rtsp_url_normalize(const char *address, char *url, size_t capacity)
{
    if (!address || !*address || !url || !capacity) return ESP_ERR_INVALID_ARG;
    const char *prefix = strstr(address, "://") ? "" : "rtsp://";
    int n = snprintf(url, capacity, "%s%s", prefix, address);
    if (n < 0 || (size_t)n >= capacity) return ESP_ERR_INVALID_SIZE;
    solar_os_rtsp_url_t parsed;
    return solar_os_rtsp_url_parse(url, &parsed);
}

esp_err_t solar_os_rtsp_uri_resolve(const char *base, const char *control,
                                   char *uri, size_t capacity)
{
    if (!base || !control || !uri) return ESP_ERR_INVALID_ARG;
    solar_os_rtsp_url_t parsed;
    esp_err_t err = solar_os_rtsp_url_parse(base, &parsed);
    if (err != ESP_OK) return err;
    int n;
    if (!strcmp(control, "*") || !*control) n = snprintf(uri, capacity, "%s", base);
    else if (!strncmp(control, "rtsp://", 7)) n = snprintf(uri, capacity, "%s", control);
    else if (*control == '/') {
        const char *slash = strchr(base + 7, '/');
        n = snprintf(uri, capacity, "%.*s%s", (int)(slash ? slash - base : (ptrdiff_t)strlen(base)), base, control);
    } else {
        /* RTSP control URLs append to Content-Base, including the slash when
         * the server omits it (the convention used by live camera servers). */
        n = snprintf(uri, capacity, "%s%s%s", base,
                     base[strlen(base) - 1] == '/' ? "" : "/", control);
    }
    if (n < 0 || (size_t)n >= capacity) return ESP_ERR_INVALID_SIZE;
    return solar_os_rtsp_url_parse(uri, &parsed);
}

esp_err_t solar_os_rtsp_response_parse(const uint8_t *data, size_t length,
                                      solar_os_rtsp_response_t *r)
{
    if (!data || !r) return ESP_ERR_INVALID_ARG;
    memset(r, 0, sizeof(*r));
    size_t header = solar_os_rtsp_header_length(data, length);
    if (!header) return length >= SOLAR_OS_RTSP_RESPONSE_MAX ? ESP_ERR_INVALID_SIZE : ESP_ERR_TIMEOUT;
    if (header > SOLAR_OS_RTSP_RESPONSE_MAX) return ESP_ERR_INVALID_SIZE;
    char line[512];
    size_t pos = 0;
    bool cseq = false, body = false, first = true;
    while (pos + 1 < header) {
        size_t end = pos;
        while (end + 1 < header && !(data[end] == '\r' && data[end + 1] == '\n')) end++;
        if (!copy_text(line, sizeof(line), (const char *)data + pos, end - pos)) return ESP_ERR_INVALID_SIZE;
        pos = end + 2;
        if (first) {
            first = false;
            if (strncmp(line, "RTSP/1.0 ", 9) || strlen(line) < 12 ||
                !isdigit((unsigned char)line[9]) || !isdigit((unsigned char)line[10]) ||
                !isdigit((unsigned char)line[11]) || (line[12] && line[12] != ' ')) return ESP_ERR_INVALID_RESPONSE;
            r->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
            continue;
        }
        if (!*line) break;
        char *colon = strchr(line, ':');
        if (!colon) return ESP_ERR_INVALID_RESPONSE;
        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') colon++;
        size_t len = strlen(colon);
        while (len && (colon[len - 1] == ' ' || colon[len - 1] == '\t')) colon[--len] = '\0';
        uint32_t n;
        if (!strcasecmp(line, "CSeq")) {
            if (cseq || !decimal(colon, &r->cseq)) return ESP_ERR_INVALID_RESPONSE;
            cseq = true;
        } else if (!strcasecmp(line, "Content-Length")) {
            if (body || !decimal(colon, &n) || n > SOLAR_OS_RTSP_RESPONSE_MAX - header)
                return ESP_ERR_INVALID_SIZE;
            body = true; r->body_length = n;
        } else if (!strcasecmp(line, "Session")) {
            char *semi = strchr(colon, ';');
            if (semi) *semi = '\0';
            if (!copy_text(r->session, sizeof(r->session), colon, strlen(colon))) return ESP_ERR_INVALID_SIZE;
        } else if (!strcasecmp(line, "Content-Base") || !strcasecmp(line, "Content-Location")) {
            if (!copy_text(r->content_base, sizeof(r->content_base), colon, len)) return ESP_ERR_INVALID_SIZE;
        } else if (!strcasecmp(line, "Transport")) {
            if (!copy_text(r->transport, sizeof(r->transport), colon, len)) return ESP_ERR_INVALID_SIZE;
        } else if (!strcasecmp(line, "WWW-Authenticate")) {
            if (!r->www_authenticate[0] || !strncasecmp(colon, "Digest", 6)) {
                if (!copy_text(r->www_authenticate, sizeof(r->www_authenticate), colon, len))
                    return ESP_ERR_INVALID_SIZE;
            }
        }
    }
    if (!cseq) return ESP_ERR_INVALID_RESPONSE;
    r->header_length = header;
    return length < header + r->body_length ? ESP_ERR_TIMEOUT : ESP_OK;
}

/* SDP media sections are processed independently; unsupported codecs are
 * skipped, never guessed as JPEG or PCM. RTSP SDP uses port zero as a
 * placeholder; actual ports are negotiated by SETUP. */
static void finish_track(solar_os_rtsp_remote_track_t *candidate,
                         solar_os_rtsp_description_t *d)
{
    /* JPEG dimensions are in the RTP payload, not required in SDP. The
     * publisher's stricter media-unit validation is not a receiver gate. */
    if (!candidate->present || !candidate->uri[0]) return;
    solar_os_rtsp_remote_track_t *dst = candidate->media.kind == SOLAR_OS_MEDIA_TRACK_VIDEO ? &d->video : &d->audio;
    if (!dst->present) *dst = *candidate;
}

esp_err_t solar_os_rtsp_description_parse(const uint8_t *sdp, size_t length,
                                         const char *base, solar_os_rtsp_description_t *d)
{
    if (!sdp || !base || !d) return ESP_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d));
    solar_os_rtsp_remote_track_t t = {0};
    char line[512]; bool section = false, supported = false;
    esp_err_t err = solar_os_rtsp_uri_resolve(base, "*", d->aggregate, sizeof(d->aggregate));
    if (err != ESP_OK) return err;
    for (size_t pos = 0; pos < length;) {
        size_t end = pos;
        while (end < length && sdp[end] != '\n') end++;
        size_t len = end - pos;
        if (len && sdp[pos + len - 1] == '\r') len--;
        if (!copy_text(line, sizeof(line), (const char *)sdp + pos, len)) return ESP_ERR_INVALID_SIZE;
        pos = end + 1;
        if (!strncmp(line, "m=", 2)) {
            if (supported) finish_track(&t, d);
            memset(&t, 0, sizeof(t)); section = true; supported = false;
            char kind[16], proto[20]; unsigned port, pt; char extra;
            if (sscanf(line + 2, "%15s %u %19s %u%c", kind, &port, proto, &pt, &extra) != 4 ||
                port > 65535 || strcmp(proto, "RTP/AVP") || pt > 127) continue;
            if (strcmp(kind, "video") && strcmp(kind, "audio")) continue;
            t.present = true; t.media.payload_type = pt;
            t.media.kind = !strcmp(kind, "video") ? SOLAR_OS_MEDIA_TRACK_VIDEO : SOLAR_OS_MEDIA_TRACK_AUDIO;
            if (pt == 26 && t.media.kind == SOLAR_OS_MEDIA_TRACK_VIDEO) {
                t.media.codec = SOLAR_OS_MEDIA_CODEC_JPEG; t.media.clock_rate = 90000;
                supported = true;
            } else if ((pt == 10 || pt == 11) && t.media.kind == SOLAR_OS_MEDIA_TRACK_AUDIO) {
                t.media.codec = SOLAR_OS_MEDIA_CODEC_L16; t.media.clock_rate = 44100;
                t.media.format.audio = (solar_os_media_audio_format_t){44100, pt == 10 ? 2 : 1, 16};
                supported = true;
            }
        } else if (!strncmp(line, "a=rtpmap:", 9) && t.present) {
            unsigned pt, rate, channels = 1; char codec[24];
            int n = sscanf(line + 9, "%u %23[^/]/%u/%u", &pt, codec, &rate, &channels);
            if (n < 3 || pt != t.media.payload_type) continue;
            supported = false;
            if (t.media.kind == SOLAR_OS_MEDIA_TRACK_VIDEO && !strcasecmp(codec, "JPEG") && rate == 90000) {
                t.media.codec = SOLAR_OS_MEDIA_CODEC_JPEG; supported = true;
            } else if (t.media.kind == SOLAR_OS_MEDIA_TRACK_AUDIO && !strcasecmp(codec, "L16") &&
                       rate >= 8000 && rate <= 48000 && channels >= 1 && channels <= 2) {
                t.media.codec = SOLAR_OS_MEDIA_CODEC_L16;
                t.media.format.audio = (solar_os_media_audio_format_t){rate, channels, 16}; supported = true;
            }
            t.media.clock_rate = rate;
        } else if (!strncmp(line, "a=control:", 10)) {
            char *dst = section ? t.uri : d->aggregate;
            err = solar_os_rtsp_uri_resolve(base, line + 10, dst, SOLAR_OS_RTSP_URI_MAX);
            if (err != ESP_OK) return err;
        }
    }
    if (supported) finish_track(&t, d);
    return d->audio.present || d->video.present ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_rtsp_transport_parse(const char *text, uint16_t client,
                                       uint16_t *rtp, uint16_t *rtcp)
{
    if (!text || !rtp || !rtcp) return ESP_ERR_INVALID_ARG;
    *rtp = *rtcp = 0;
    char fields[256];
    if (!copy_text(fields, sizeof(fields), text, strlen(text))) return ESP_ERR_INVALID_SIZE;
    char *save, *f = strtok_r(fields, ";", &save);
    if (!f || (strcasecmp(f, "RTP/AVP") && strcasecmp(f, "RTP/AVP/UDP"))) return ESP_ERR_NOT_SUPPORTED;
    bool unicast = false, client_seen = false;
    while ((f = strtok_r(NULL, ";", &save))) {
        while (*f == ' ') f++;
        if (!strcasecmp(f, "unicast")) unicast = true;
        else if (!strncasecmp(f, "interleaved=", 12) || !strcasecmp(f, "multicast")) return ESP_ERR_NOT_SUPPORTED;
        else if (!strncasecmp(f, "client_port=", 12) || !strncasecmp(f, "server_port=", 12)) {
            unsigned a, b; char extra;
            if (sscanf(f + 12, "%u-%u%c", &a, &b, &extra) != 2 || !a || a > 65534 || b != a + 1)
                return ESP_ERR_INVALID_RESPONSE;
            if (!strncasecmp(f, "client", 6)) {
                if (a != client) return ESP_ERR_INVALID_RESPONSE;
                client_seen = true;
            } else { *rtp = a; *rtcp = b; }
        }
    }
    return unicast && client_seen ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

esp_err_t solar_os_rtsp_sender_clock_feed(solar_os_rtsp_sender_clock_t *clock,
                                         const uint8_t *rtcp, size_t length)
{
    if (!clock || !rtcp) return ESP_ERR_INVALID_ARG;
    bool found = false;
    for (size_t pos = 0; pos < length;) {
        if (length - pos < 4 || rtcp[pos] >> 6 != 2) return ESP_ERR_INVALID_RESPONSE;
        size_t n = (((size_t)rtcp[pos + 2] << 8 | rtcp[pos + 3]) + 1) * 4;
        if (n > length - pos) return ESP_ERR_INVALID_SIZE;
        if (rtcp[pos + 1] == 200) {
            if (n < 28 + (rtcp[pos] & 31U) * 24U) return ESP_ERR_INVALID_SIZE;
            uint32_t ssrc = be32(rtcp + pos + 4);
            if (!clock->valid || clock->ssrc == ssrc) {
                clock->ssrc = ssrc; clock->timestamp = be32(rtcp + pos + 16);
                clock->ntp_us = (uint64_t)be32(rtcp + pos + 8) * 1000000U +
                    ((uint64_t)be32(rtcp + pos + 12) * 1000000U >> 32);
                clock->valid = true; found = true;
            }
        }
        pos += n;
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

uint64_t solar_os_rtsp_sender_time(const solar_os_rtsp_sender_clock_t *clock,
                                  uint32_t timestamp, uint32_t rate)
{
    if (!clock || !clock->valid || !rate) return 0;
    int64_t result = (int64_t)clock->ntp_us + (int64_t)(int32_t)(timestamp - clock->timestamp) * 1000000 / rate;
    return result > 0 ? (uint64_t)result : 0;
}

void solar_os_rtsp_audio_jitter_init(solar_os_rtsp_audio_jitter_t *j,
                                     const solar_os_media_track_t *track)
{
    memset(j, 0, sizeof(*j));
    j->rate = track->clock_rate; j->channels = track->format.audio.channels;
    j->payload_type = track->payload_type;
}

esp_err_t solar_os_rtsp_audio_jitter_feed(solar_os_rtsp_audio_jitter_t *j,
                                         const uint8_t *rtp, size_t length, uint64_t now)
{
    solar_os_rtp_header_t h; const uint8_t *data; size_t len;
    esp_err_t err = solar_os_rtp_header_decode(rtp, length, &h, &data, &len);
    if (err != ESP_OK) return err;
    if (!j->rate || !j->channels || h.payload_type != j->payload_type || !len ||
        len > SOLAR_OS_RTSP_AUDIO_PAYLOAD_MAX || len % (2U * j->channels)) {
        j->dropped++; return ESP_ERR_INVALID_SIZE;
    }
    if (!j->started) {
        j->started = true; j->origin = j->next_timestamp = h.timestamp;
        j->origin_us = now + SOLAR_OS_RTSP_JITTER_US; j->ssrc = h.ssrc;
    }
    if (h.ssrc != j->ssrc || (int32_t)(h.timestamp - j->next_timestamp) < 0) {
        j->dropped++; return ESP_ERR_INVALID_STATE;
    }
    int64_t due = (int64_t)j->origin_us +
        (int64_t)(int32_t)(h.timestamp - j->origin) * 1000000 / j->rate;
    if ((int64_t)now - due > 100000 || due - (int64_t)now > 1000000) {
        /* Rebuffer both a stalled clock and a forward timestamp jump. A
         * resumed publisher may burst its accumulated media time; retaining
         * that offset would fill the queue while waiting seconds for audio.
         * Ordinary bounded reordering/bursts stay on the existing clock. */
        for (size_t i = 0; i < SOLAR_OS_RTSP_AUDIO_SLOTS; i++) {
            if (j->slots[i].used) { j->slots[i].used = false; j->dropped++; }
        }
        j->origin = j->next_timestamp = h.timestamp;
        j->origin_us = now + SOLAR_OS_RTSP_JITTER_US;
        j->rebuffers++;
    }
    solar_os_rtsp_audio_packet_t *free_slot = NULL;
    for (size_t i = 0; i < SOLAR_OS_RTSP_AUDIO_SLOTS; i++) {
        if (j->slots[i].used && j->slots[i].timestamp == h.timestamp) return ESP_OK;
        if (!j->slots[i].used) free_slot = &j->slots[i];
    }
    if (!free_slot) { j->dropped++; return ESP_ERR_NO_MEM; }
    free_slot->used = true; free_slot->sequence = h.sequence;
    free_slot->timestamp = h.timestamp; free_slot->length = len;
    memcpy(free_slot->payload, data, len);
    return ESP_OK;
}

bool solar_os_rtsp_audio_jitter_pop(solar_os_rtsp_audio_jitter_t *j,
                                    uint64_t now, solar_os_rtsp_audio_packet_t *out)
{
    if (!j->started) return false;
    solar_os_rtsp_audio_packet_t *next = NULL;
    for (size_t i = 0; i < SOLAR_OS_RTSP_AUDIO_SLOTS; i++) {
        solar_os_rtsp_audio_packet_t *p = &j->slots[i];
        if (!p->used) continue;
        int64_t due = (int64_t)j->origin_us + (int64_t)(int32_t)(p->timestamp - j->origin) * 1000000 / j->rate;
        if ((int32_t)(p->timestamp - j->next_timestamp) < 0 || (int64_t)now - due > 100000) {
            p->used = false; j->dropped++; continue;
        }
        if (!next || (int32_t)(p->timestamp - next->timestamp) < 0) next = p;
    }
    if (!next) return false;
    int64_t due = (int64_t)j->origin_us + (int64_t)(int32_t)(j->next_timestamp - j->origin) * 1000000 / j->rate;
    if ((int64_t)now < due) return false;
    uint32_t gap = next->timestamp - j->next_timestamp;
    if (gap && gap <= j->rate / 10) {
        size_t frames = SOLAR_OS_RTSP_AUDIO_PAYLOAD_MAX / (2U * j->channels);
        if (frames > gap) frames = gap;
        memset(out, 0, sizeof(*out)); out->length = frames * 2U * j->channels;
        out->timestamp = j->next_timestamp; j->next_timestamp += frames; j->concealed++;
        j->concealed_frames += frames;
        return true;
    }
    due = (int64_t)j->origin_us + (int64_t)(int32_t)(next->timestamp - j->origin) * 1000000 / j->rate;
    if ((int64_t)now < due) return false;
    *out = *next; next->used = false;
    j->next_timestamp = out->timestamp + out->length / (2U * j->channels);
    return true;
}
