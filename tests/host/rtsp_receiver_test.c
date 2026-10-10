#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_rtsp_receiver.h"

static void test_urls(void)
{
    solar_os_rtsp_url_t u;
    assert(solar_os_rtsp_url_parse("rtsp://cam/media", &u) == ESP_OK);
    assert(!strcmp(u.host, "cam") && u.port == 554);
    assert(solar_os_rtsp_url_parse("rtsp://127.0.0.1:8554/media", &u) == ESP_OK && u.port == 8554);
    assert(solar_os_rtsp_url_parse("http://cam/media", &u) != ESP_OK);
    assert(solar_os_rtsp_url_parse("rtsp://cam:0/media", &u) != ESP_OK);
    assert(solar_os_rtsp_url_parse("rtsp://cam:99999/media", &u) != ESP_OK);
    assert(solar_os_rtsp_url_parse("rtsp://user:password@cam/media", &u) == ESP_OK);
    assert(!strcmp(u.user, "user") && !strcmp(u.password, "password") && !strcmp(u.host, "cam"));
    assert(solar_os_rtsp_url_parse("rtsp://user:p%40ss@cam:8554/media", &u) == ESP_OK);
    assert(!strcmp(u.password, "p@ss") && u.port == 8554);
    assert(solar_os_rtsp_url_parse("rtsp://@cam/media", &u) != ESP_OK);
    assert(solar_os_rtsp_url_parse("rtsp://cam/media\r\nBad: header", &u) != ESP_OK);
    char uri[192];
    assert(solar_os_rtsp_url_normalize("192.168.1.238", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://192.168.1.238"));
    assert(solar_os_rtsp_url_parse(uri, &u) == ESP_OK && u.port == 554);
    assert(solar_os_rtsp_url_normalize("192.168.1.238/media", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://192.168.1.238/media"));
    assert(solar_os_rtsp_url_normalize("192.168.1.192:8554/youtube", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://192.168.1.192:8554/youtube"));
    assert(solar_os_rtsp_url_parse(uri, &u) == ESP_OK && u.port == 8554);
    assert(solar_os_rtsp_url_normalize("192.168.1.238:554", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://192.168.1.238:554"));
    assert(solar_os_rtsp_url_normalize("rtsp://192.168.1.238/media", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://192.168.1.238/media"));
    assert(solar_os_rtsp_url_normalize("http://192.168.1.238/media", uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_url_normalize("192.168.1.238:0/media", uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_url_normalize("192.168.1.238:65536/media", uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_url_normalize("user:pass@192.168.1.238/media", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://user:pass@192.168.1.238/media"));
    char request[192];
    assert(solar_os_rtsp_url_request_uri(uri, request, sizeof(request)) == ESP_OK);
    assert(!strcmp(request, "rtsp://192.168.1.238/media"));
    char redacted[192];
    assert(solar_os_rtsp_url_redact(uri, redacted, sizeof(redacted)) == ESP_OK);
    assert(!strcmp(redacted, "user@192.168.1.238/media"));
    assert(solar_os_rtsp_url_normalize("192.168.1.238/media\r\nBad: header", uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_url_normalize("[::1]/media", uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_url_normalize("", uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_url_normalize(NULL, uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_url_normalize("192.168.1.238/media", uri, 12) == ESP_ERR_INVALID_SIZE);
    char address[SOLAR_OS_RTSP_URI_MAX];
    memset(address, 'a', sizeof(address));
    memcpy(address, "192.168.1.238/", sizeof("192.168.1.238/") - 1);
    address[sizeof(address) - 8] = '\0';
    assert(solar_os_rtsp_url_normalize(address, uri, sizeof(uri)) == ESP_OK);
    address[sizeof(address) - 8] = 'a'; address[sizeof(address) - 7] = '\0';
    assert(solar_os_rtsp_url_normalize(address, uri, sizeof(uri)) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_rtsp_uri_resolve("", "track", uri, sizeof(uri)) != ESP_OK);
    assert(solar_os_rtsp_uri_resolve("rtsp://cam/media/", "trackID=0", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://cam/media/trackID=0"));
    assert(solar_os_rtsp_uri_resolve("rtsp://cam/media", "/track", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://cam/track"));
    assert(solar_os_rtsp_uri_resolve("rtsp://cam/media", "rtsp://cam/other", uri, sizeof(uri)) == ESP_OK);
    assert(!strcmp(uri, "rtsp://cam/other"));
}

static void test_response(void)
{
    const char *text = "RTSP/1.0 200 OK\r\nCSeq: 7\r\nSession: abc123;timeout=60\r\nContent-Base: rtsp://cam/media/\r\nContent-Length: 3\r\n\r\nabc";
    solar_os_rtsp_response_t r;
    for (size_t n = 0; n < strlen(text); n++)
        assert(solar_os_rtsp_response_parse((const uint8_t *)text, n, &r) == ESP_ERR_TIMEOUT);
    assert(solar_os_rtsp_response_parse((const uint8_t *)text, strlen(text), &r) == ESP_OK);
    assert(r.cseq == 7 && r.status == 200 && r.body_length == 3);
    assert(!strcmp(r.session, "abc123") && !strcmp(r.content_base, "rtsp://cam/media/"));
    text = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nCSeq: 2\r\n\r\n";
    assert(solar_os_rtsp_response_parse((const uint8_t *)text, strlen(text), &r) == ESP_ERR_INVALID_RESPONSE);
    text = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 4294967295\r\n\r\n";
    assert(solar_os_rtsp_response_parse((const uint8_t *)text, strlen(text), &r) == ESP_ERR_INVALID_SIZE);
    text = "RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\n\r\n";
    assert(solar_os_rtsp_response_parse((const uint8_t *)text, strlen(text), &r) == ESP_OK && r.status == 401);
}

static void test_sdp(void)
{
    const char *sdp = "v=0\r\na=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 JPEG/90000\r\na=control:trackID=0\r\nm=audio 0 RTP/AVP 97\r\na=rtpmap:97 L16/16000/2\r\na=control:trackID=1\r\n";
    solar_os_rtsp_description_t d;
    assert(solar_os_rtsp_description_parse((const uint8_t *)sdp, strlen(sdp), "rtsp://cam/media/", &d) == ESP_OK);
    assert(d.video.present && d.audio.present && d.audio.media.format.audio.channels == 2);
    assert(!strcmp(d.video.uri, "rtsp://cam/media/trackID=0"));
    sdp = "v=0\na=control:rtsp://cam/media\nm=audio 0 RTP/AVP 97\na=rtpmap:97 L16/48000\na=control:/audio\n";
    assert(solar_os_rtsp_description_parse((const uint8_t *)sdp, strlen(sdp), "rtsp://cam/media/", &d) == ESP_OK);
    assert(!d.video.present && d.audio.present && d.audio.media.format.audio.channels == 1);
    assert(!strcmp(d.aggregate, "rtsp://cam/media"));
    sdp = "m=video 0 RTP/AVP 96\na=rtpmap:96 H264/90000\na=control:video\nm=audio 0 RTP/AVP 97\na=rtpmap:97 L16/16000/9\na=control:audio\n";
    assert(solar_os_rtsp_description_parse((const uint8_t *)sdp, strlen(sdp), "rtsp://cam/media/", &d) == ESP_ERR_NOT_SUPPORTED);
    sdp = "m=video 0 RTP/AVP 26\na=control:video\n";
    assert(solar_os_rtsp_description_parse((const uint8_t *)sdp, strlen(sdp), "rtsp://cam/media", &d) == ESP_OK && d.video.present);
}

static void test_transport(void)
{
    uint16_t a, b;
    assert(solar_os_rtsp_transport_parse("RTP/AVP/UDP;unicast;client_port=10000-10001;server_port=1234-1235;ssrc=abc", 10000, &a, &b) == ESP_OK);
    assert(a == 1234 && b == 1235);
    assert(solar_os_rtsp_transport_parse("RTP/AVP;unicast;client_port=10000-10001", 10000, &a, &b) == ESP_OK && !a);
    assert(solar_os_rtsp_transport_parse("RTP/AVP/TCP;unicast;interleaved=0-1", 10000, &a, &b) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_rtsp_transport_parse("RTP/AVP;unicast;client_port=1234-1235", 10000, &a, &b) != ESP_OK);
}

static void test_clock(void)
{
    uint8_t rtcp[128]; size_t length;
    assert(solar_os_rtcp_sender_report(123, 100, 0x80000000U, 0xfffffff0U, 7, 1234, "cam", rtcp, sizeof(rtcp), &length) == ESP_OK);
    solar_os_rtsp_sender_clock_t c = {0};
    assert(solar_os_rtsp_sender_clock_feed(&c, rtcp, length) == ESP_OK);
    assert(solar_os_rtsp_sender_time(&c, 0x10, 16000) == 100502000ULL);
    assert(solar_os_rtsp_sender_time(&c, 0xffffffe0U, 16000) == 100499000ULL);
    assert(solar_os_rtsp_sender_clock_feed(&c, rtcp, 20) == ESP_ERR_INVALID_SIZE);
}

static void feed_audio(solar_os_rtsp_audio_jitter_t *j, uint16_t seq, uint32_t timestamp, uint64_t now)
{
    uint8_t packet[332] = {0};
    solar_os_rtp_header_t h = {.payload_type = 97, .sequence = seq, .timestamp = timestamp, .ssrc = 123};
    assert(solar_os_rtp_header_encode(&h, packet, sizeof(packet)) == ESP_OK);
    packet[12] = 0x12; packet[13] = 0x34;
    assert(solar_os_rtsp_audio_jitter_feed(j, packet, sizeof(packet), now) == ESP_OK);
}

static void test_jitter(void)
{
    solar_os_media_track_t track = {.payload_type = 97, .clock_rate = 16000,
        .format.audio = {.sample_rate = 16000, .channels = 1, .bits_per_sample = 16}};
    solar_os_rtsp_audio_jitter_t j;
    solar_os_rtsp_audio_jitter_init(&j, &track);
    solar_os_rtsp_audio_packet_t out;
    feed_audio(&j, 65535, 0xffffff00U, 0);
    feed_audio(&j, 1, 0x40, 1000);
    feed_audio(&j, 0, 0xffffffa0U, 2000);
    const uint64_t start = SOLAR_OS_RTSP_JITTER_US;
    assert(!solar_os_rtsp_audio_jitter_pop(&j, start - 1, &out));
    assert(solar_os_rtsp_audio_jitter_pop(&j, start, &out) && out.sequence == 65535);
    assert(out.payload[0] == 0x12 && out.payload[1] == 0x34);
    assert(solar_os_rtsp_audio_jitter_pop(&j, start + 10000, &out) && out.sequence == 0);
    assert(solar_os_rtsp_audio_jitter_pop(&j, start + 20000, &out) && out.sequence == 1);
    feed_audio(&j, 3, 0x180, 61000); /* Missing sequence 2: bounded silence. */
    assert(solar_os_rtsp_audio_jitter_pop(&j, start + 30000, &out) && out.timestamp == 0xe0);
    assert(j.concealed == 1 && j.concealed_frames == 160 && out.payload[0] == 0);
    assert(solar_os_rtsp_audio_jitter_pop(&j, start + 40000, &out) && out.sequence == 3);
    feed_audio(&j, 4, 0x220, 90000);
    assert(!solar_os_rtsp_audio_jitter_pop(&j, 300000, &out) && j.dropped == 1);
    feed_audio(&j, 5, 0x2c0, 400000); /* Sender resumes with a stalled RTP clock. */
    assert(!solar_os_rtsp_audio_jitter_pop(&j, 400000 + start - 1, &out));
    assert(solar_os_rtsp_audio_jitter_pop(&j, 400000 + start, &out) && out.sequence == 5);
    feed_audio(&j, 6, 0x2c0 + 16000 * 8, 410000); /* Resumed sender bursts time forward. */
    assert(j.rebuffers == 2);
    assert(!solar_os_rtsp_audio_jitter_pop(&j, 410000 + start - 1, &out));
    assert(solar_os_rtsp_audio_jitter_pop(&j, 410000 + start, &out) && out.sequence == 6);
    solar_os_rtsp_audio_jitter_init(&j, &track);
    for (unsigned i = 0; i < SOLAR_OS_RTSP_AUDIO_SLOTS; i++) feed_audio(&j, i, 160 * i, 0);
    uint8_t packet[332] = {0};
    solar_os_rtp_header_t h = {.payload_type = 97, .sequence = 99, .timestamp = 9999, .ssrc = 123};
    assert(solar_os_rtp_header_encode(&h, packet, sizeof(packet)) == ESP_OK);
    assert(solar_os_rtsp_audio_jitter_feed(&j, packet, sizeof(packet), 0) == ESP_ERR_NO_MEM);
    /* Recover even when the old bounded queue is completely full. */
    feed_audio(&j, 100, 160000, 1000);
    assert(j.rebuffers == 1 && j.dropped == SOLAR_OS_RTSP_AUDIO_SLOTS + 1);
    assert(solar_os_rtsp_audio_jitter_pop(&j, 1000 + start, &out) && out.sequence == 100);
}

static void test_relay_audio(void)
{
    solar_os_media_track_t track = {.payload_type = 97, .clock_rate = 44100,
        .format.audio = {.sample_rate = 44100, .channels = 1, .bits_per_sample = 16}};
    solar_os_rtsp_audio_jitter_t j;
    solar_os_rtsp_audio_jitter_init(&j, &track);
    uint8_t packet[1500] = {0}; solar_os_rtsp_audio_packet_t out;
    uint32_t timestamp = 0xffffff00U;
    /* Eight reordered relay blocks arrive as a burst, still bounded by the
     * jitter slots. Both halves of each 1024-sample block must survive. */
    for (unsigned i = 0; i < 16; i++) {
        unsigned index = i ^ 1U;
        size_t length = index % 2 ? 660 : 1388;
        uint32_t offset = (index / 2) * 1024 + (index % 2 ? 694 : 0);
        solar_os_rtp_header_t h = {.payload_type = 97, .sequence = index,
            .timestamp = timestamp + offset, .ssrc = 123};
        assert(solar_os_rtp_header_encode(&h, packet, sizeof(packet)) == ESP_OK);
        memset(packet + 12, 0x12, length);
        /* Establish origin with the earliest packet, then reorder the rest. */
        if (i == 0) {
            h.sequence = 0; h.timestamp = timestamp;
            assert(solar_os_rtp_header_encode(&h, packet, sizeof(packet)) == ESP_OK);
            assert(solar_os_rtsp_audio_jitter_feed(&j, packet, 12 + 1388, 0) == ESP_OK);
            assert(solar_os_rtp_header_encode(&(solar_os_rtp_header_t){.payload_type = 97,
                .sequence = index, .timestamp = timestamp + offset, .ssrc = 123}, packet, sizeof(packet)) == ESP_OK);
        }
        assert(solar_os_rtsp_audio_jitter_feed(&j, packet, 12 + length, 1000) == ESP_OK);
    }
    for (unsigned i = 0; i < 16; i++) {
        uint32_t offset = (i / 2) * 1024 + (i % 2 ? 694 : 0);
        uint64_t due = SOLAR_OS_RTSP_JITTER_US + (uint64_t)offset * 1000000 / 44100;
        assert(!solar_os_rtsp_audio_jitter_pop(&j, due - 1, &out));
        assert(solar_os_rtsp_audio_jitter_pop(&j, due, &out));
        assert(out.sequence == i && out.length == (i % 2 ? 660 : 1388));
    }
    assert(!j.dropped && !j.concealed);
    solar_os_rtp_header_t h = {.payload_type = 97, .timestamp = timestamp + 8192, .ssrc = 123};
    assert(solar_os_rtp_header_encode(&h, packet, sizeof(packet)) == ESP_OK);
    assert(solar_os_rtsp_audio_jitter_feed(&j, packet, 12 + SOLAR_OS_RTSP_AUDIO_PAYLOAD_MAX + 2, 0) == ESP_ERR_INVALID_SIZE);
    assert(j.dropped == 1);
}

int main(void)
{
    test_urls(); test_response(); test_sdp(); test_transport(); test_clock(); test_jitter(); test_relay_audio();
    puts("rtsp_receiver_test: OK"); return 0;
}
