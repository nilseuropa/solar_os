#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_media.h"
#include "solar_os_rtp.h"
#include "solar_os_rtp_jpeg.h"
#include "solar_os_rtsp.h"
#ifdef TEST_NATIVE_MEDIA_PROTOCOL
#include <stdlib.h>
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_media_protocol.h"
#endif

typedef struct {
    uint8_t packets[16][256];
    size_t lengths[16];
    size_t count;
} packet_capture_t;

static void count_release(void *token)
{
    (*(unsigned *)token)++;
}

static esp_err_t capture_packet(const uint8_t *packet,
                                size_t packet_len,
                                void *user)
{
    packet_capture_t *capture = user;
    assert(capture->count < 16U);
    assert(packet_len <= sizeof(capture->packets[0]));
    memcpy(capture->packets[capture->count], packet, packet_len);
    capture->lengths[capture->count] = packet_len;
    capture->count++;
    return ESP_OK;
}

static void test_release_and_clock(void)
{
    unsigned releases = 0U;
    const uint8_t data[] = {1U};
    solar_os_media_unit_t unit = {
        .track = {
            .kind = SOLAR_OS_MEDIA_TRACK_VIDEO,
            .codec = SOLAR_OS_MEDIA_CODEC_JPEG,
            .payload_type = 96U,
            .clock_rate = 90000U,
            .format.video = {320U, 240U, 5U},
        },
        .data = data,
        .length = sizeof(data),
        .release = count_release,
        .release_token = &releases,
    };
    assert(solar_os_media_unit_valid(&unit));
    solar_os_media_unit_release(&unit);
    solar_os_media_unit_release(&unit);
    assert(releases == 1U);

    solar_os_media_clock_t clock;
    assert(solar_os_media_clock_init(&clock, 1000000U, 0xfffffff0U, 90000U) ==
           ESP_OK);
    uint32_t timestamp = 0U;
    assert(solar_os_media_clock_map(&clock, 1500000U, &timestamp) == ESP_OK);
    assert(timestamp == (uint32_t)(0xfffffff0U + 45000U));
    assert(solar_os_media_clock_map(&clock, 999999U, &timestamp) ==
           ESP_ERR_INVALID_ARG);
}

static void test_rtp_header(void)
{
    uint8_t packet[12];
    const solar_os_rtp_header_t source = {
        .marker = true,
        .payload_type = 96U,
        .sequence = 0x1234U,
        .timestamp = 0x01020304U,
        .ssrc = 0xa0b0c0d0U,
    };
    assert(solar_os_rtp_header_encode(&source, packet, sizeof(packet)) == ESP_OK);
    solar_os_rtp_header_t decoded;
    const uint8_t *payload = NULL;
    size_t payload_len = 1U;
    assert(solar_os_rtp_header_decode(packet,
                                      sizeof(packet),
                                      &decoded,
                                      &payload,
                                      &payload_len) == ESP_OK);
    assert(decoded.marker && decoded.payload_type == source.payload_type);
    assert(decoded.sequence == source.sequence);
    assert(decoded.timestamp == source.timestamp);
    assert(decoded.ssrc == source.ssrc);
    assert(payload_len == 0U && payload == packet + sizeof(packet));
    packet[0] |= 0x10U;
    assert(solar_os_rtp_header_decode(packet,
                                      sizeof(packet),
                                      &decoded,
                                      &payload,
                                      &payload_len) == ESP_ERR_NOT_SUPPORTED);
}

static void test_l16_packetization(void)
{
    const int16_t samples[] = {
        (int16_t)0x1234, (int16_t)0xabcd,
        (int16_t)0x0102, (int16_t)0x0304,
        (int16_t)0x0506, (int16_t)0x0708,
        (int16_t)0x090a, (int16_t)0x0b0c,
        (int16_t)0x0d0e, (int16_t)0x0f10,
    };
    solar_os_rtp_sender_t sender = {
        .payload_type = 97U,
        .sequence = 10U,
        .timestamp = 100U,
        .ssrc = 7U,
        .max_packet_bytes = 20U,
    };
    uint8_t packet[20];
    packet_capture_t capture = {0};
    assert(solar_os_rtp_l16_packetize(&sender,
                                      samples,
                                      5U,
                                      2U,
                                      packet,
                                      sizeof(packet),
                                      capture_packet,
                                      &capture) == ESP_OK);
    assert(capture.count == 3U);
    assert(sender.sequence == 13U && sender.timestamp == 105U);
    solar_os_rtp_header_t header;
    const uint8_t *payload = NULL;
    size_t payload_len = 0U;
    assert(solar_os_rtp_header_decode(capture.packets[0],
                                      capture.lengths[0],
                                      &header,
                                      &payload,
                                      &payload_len) == ESP_OK);
    assert(header.sequence == 10U && header.timestamp == 100U);
    assert(payload_len == 8U);
    assert(payload[0] == 0x12U && payload[1] == 0x34U);
    assert(payload[2] == 0xabU && payload[3] == 0xcdU);
    assert(solar_os_rtp_header_decode(capture.packets[2],
                                      capture.lengths[2],
                                      &header,
                                      &payload,
                                      &payload_len) == ESP_OK);
    assert(header.sequence == 12U && header.timestamp == 104U);
    assert(payload_len == 4U);
}

static void test_rtcp_packets(void)
{
    uint8_t packet[96];
    size_t packet_len = 0U;
    assert(solar_os_rtcp_sender_report(0x11223344U,
                                       1U,
                                       2U,
                                       3U,
                                       4U,
                                       5U,
                                       "solaros@test",
                                       packet,
                                       sizeof(packet),
                                       &packet_len) == ESP_OK);
    assert(packet_len % 4U == 0U && packet_len > 28U);
    assert(packet[0] == 0x80U && packet[1] == 200U);
    assert(packet[28] == 0x81U && packet[29] == 202U);
    assert(packet[36] == 1U && packet[37] == 12U);
    assert(memcmp(&packet[38], "solaros@test", 12U) == 0);
    assert(solar_os_rtcp_bye(0x11223344U,
                             packet,
                             sizeof(packet),
                             &packet_len) == ESP_OK);
    assert(packet_len == 8U && packet[0] == 0x81U && packet[1] == 203U);
}

static void make_jpeg_view(solar_os_rtp_jpeg_view_t *view,
                           uint8_t *scan,
                           size_t scan_len)
{
    memset(view, 0, sizeof(*view));
    view->scan = scan;
    view->scan_len = scan_len;
    view->width = 320U;
    view->height = 240U;
    view->type = 1U;
    for (size_t i = 0U; i < sizeof(view->quant_tables); i++) {
        view->quant_tables[i] = (uint8_t)(1U + i % 127U);
    }
}

static void packetize_jpeg(packet_capture_t *capture,
                           uint16_t sequence,
                           uint32_t timestamp)
{
    static uint8_t scan[400];
    for (size_t i = 0U; i < sizeof(scan); i++) {
        scan[i] = (uint8_t)(i % 251U);
    }
    solar_os_rtp_jpeg_view_t view;
    make_jpeg_view(&view, scan, sizeof(scan));
    solar_os_rtp_sender_t sender = {
        .payload_type = 96U,
        .sequence = sequence,
        .timestamp = timestamp,
        .ssrc = 0x11223344U,
        .max_packet_bytes = 180U,
    };
    uint8_t packet[180];
    assert(solar_os_rtp_jpeg_packetize(&sender,
                                       &view,
                                       packet,
                                       sizeof(packet),
                                       capture_packet,
                                       capture) == ESP_OK);
    assert(capture->count > 1U);
}

static void test_jpeg_round_trip(void)
{
    packet_capture_t capture = {0};
    packetize_jpeg(&capture, 100U, 9000U);
    uint8_t buffer[8192];
    solar_os_rtp_jpeg_receiver_t receiver;
    assert(solar_os_rtp_jpeg_receiver_init(
               &receiver, 96U, buffer, sizeof(buffer)) == ESP_OK);
    solar_os_rtp_jpeg_frame_t frame;
    for (size_t i = 0U; i < capture.count; i++) {
        assert(solar_os_rtp_jpeg_receiver_feed(&receiver,
                                               capture.packets[i],
                                               capture.lengths[i],
                                               &frame) == ESP_OK);
        assert((frame.data != NULL) == (i + 1U == capture.count));
    }
    assert(frame.width == 320U && frame.height == 240U);
    assert(frame.timestamp == 9000U && frame.ssrc == 0x11223344U);
    assert(receiver.frames == 1U && receiver.dropped_frames == 0U);

    solar_os_rtp_jpeg_view_t parsed;
    assert(solar_os_rtp_jpeg_parse(frame.data, frame.length, &parsed) == ESP_OK);
    assert(parsed.width == 320U && parsed.height == 240U && parsed.type == 1U);
    assert(parsed.scan_len == 400U);
    for (size_t i = 0U; i < parsed.scan_len; i++) {
        assert(parsed.scan[i] == (uint8_t)(i % 251U));
    }
}

static void test_jpeg_loss_drops_frame_and_recovers(void)
{
    packet_capture_t first = {0};
    packetize_jpeg(&first, 200U, 12000U);
    uint8_t buffer[8192];
    solar_os_rtp_jpeg_receiver_t receiver;
    assert(solar_os_rtp_jpeg_receiver_init(
               &receiver, 96U, buffer, sizeof(buffer)) == ESP_OK);
    solar_os_rtp_jpeg_frame_t frame;
    assert(solar_os_rtp_jpeg_receiver_feed(&receiver,
                                           first.packets[0],
                                           first.lengths[0],
                                           &frame) == ESP_OK);
    assert(solar_os_rtp_jpeg_receiver_feed(&receiver,
                                           first.packets[2],
                                           first.lengths[2],
                                           &frame) == ESP_ERR_INVALID_RESPONSE);
    assert(receiver.dropped_frames == 1U);

    packet_capture_t second = {0};
    packetize_jpeg(&second, 300U, 15000U);
    for (size_t i = 0U; i < second.count; i++) {
        assert(solar_os_rtp_jpeg_receiver_feed(&receiver,
                                               second.packets[i],
                                               second.lengths[i],
                                               &frame) == ESP_OK);
    }
    assert(frame.data != NULL);
    assert(receiver.frames == 1U && receiver.dropped_frames == 1U);
}

static void test_rtsp_request_parser(void)
{
    static const char setup[] =
        "SETUP rtsp://camera/media/trackID=0 RTSP/1.0\r\n"
        "CSeq: 4\r\n"
        "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n"
        "Session: abc123;timeout=60\r\n\r\n";
    assert(solar_os_rtsp_header_length(
               (const uint8_t *)setup, sizeof(setup) - 1U) ==
           sizeof(setup) - 1U);
    solar_os_rtsp_request_t request;
    assert(solar_os_rtsp_parse_request((const uint8_t *)setup,
                                       sizeof(setup) - 1U,
                                       &request) == ESP_OK);
    assert(request.method == SOLAR_OS_RTSP_METHOD_SETUP);
    assert(request.cseq == 4U);
    assert(request.client_rtp_port == 5000U);
    assert(request.client_rtcp_port == 5001U);
    assert(strcmp(request.session, "abc123") == 0);

    /* FFmpeg sends the explicit lower transport and may put CSeq last. */
    static const char explicit_udp[] =
        "SETUP rtsp://camera/media/trackID=0 RTSP/1.0\r\n"
        "Transport: RTP/AVP/UDP;unicast;client_port=5000-5001\r\n"
        "CSeq: 6\r\n\r\n";
    assert(solar_os_rtsp_parse_request((const uint8_t *)explicit_udp,
                                       sizeof(explicit_udp) - 1U,
                                       &request) == ESP_OK);
    assert(request.cseq == 6U);
    assert(request.client_rtp_port == 5000U);
    assert(request.client_rtcp_port == 5001U);

    static const char unsupported[] =
        "SETUP rtsp://camera/media/trackID=0 RTSP/1.0\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "CSeq: 5\r\n\r\n";
    assert(solar_os_rtsp_parse_request((const uint8_t *)unsupported,
                                       sizeof(unsupported) - 1U,
                                       &request) == ESP_ERR_NOT_SUPPORTED);
    assert(request.cseq == 5U);
}

#ifdef TEST_NATIVE_MEDIA_PROTOCOL
static const solar_os_native_rtp_api_v1_t *rtp_api;
static size_t receiver_allocations;

const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "rtp.jpeg.receiver"));
    void *state = malloc(bytes);
    if (state) receiver_allocations++;
    return state;
}

void solar_os_memory_free(void *state)
{
    if (state) receiver_allocations--;
    free(state);
}

static int abort_packet(const uint8_t *packet, size_t length, void *data)
{
    unsigned *visits = data;
    assert(packet && length >= SOLAR_OS_RTP_HEADER_BYTES);
    (*visits)++;
    return SOLAR_OS_NATIVE_ERROR_CANCELLED;
}

static int native_frame(const solar_os_rtp_jpeg_frame_t *frame, void *data)
{
    solar_os_native_handle_t id = *(solar_os_native_handle_t *)data;
    assert(frame->width == 320 && frame->height == 240 && frame->timestamp == 9000);
    assert(rtp_api->jpeg_receiver_close(id) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(rtp_api->jpeg_receiver_reset(id) == SOLAR_OS_NATIVE_ERROR_BUSY);
    solar_os_rtp_jpeg_view_t parsed;
    assert(!rtp_api->jpeg_parse(frame->data, frame->length, &parsed) && parsed.scan_len == 400);
    for (size_t i = 0; i < parsed.scan_len; i++) assert(parsed.scan[i] == (uint8_t)(i % 251U));
    return SOLAR_OS_NATIVE_ERROR_CANCELLED;
}

static void test_native_protocol(void)
{
    rtp_api = solar_os_native_media_protocol_get_service("media.rtp", 1, sizeof(*rtp_api));
    const solar_os_native_rtsp_protocol_api_v1_t *rtsp = solar_os_native_media_protocol_get_service(
        "media.rtsp.protocol", 1, sizeof(*rtsp));
    assert(rtp_api && rtsp && !solar_os_native_media_protocol_get_service("media.rtp", 2, 8));
    const uint8_t request[] = "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    solar_os_rtsp_request_t parsed;
    assert(rtsp->header_length(request, sizeof(request) - 2) == 0);
    assert(!rtsp->parse_request(request, sizeof(request) - 1, &parsed));
    assert(parsed.cseq == 1 && !strcmp(rtsp->method_name(parsed.method), "OPTIONS"));

    solar_os_rtp_sender_t sender = {.payload_type = 96, .sequence = 100, .timestamp = 9000,
        .ssrc = 0x11223344, .max_packet_bytes = 180};
    uint8_t packet[180];
    const int16_t stereo[] = {0, 0};
    unsigned visits = 0;
    assert(rtp_api->l16_packetize(&sender, stereo, SIZE_MAX, 2, packet, sizeof(packet), abort_packet, &visits)
        == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !visits);
    assert(rtp_api->l16_packetize(&sender, stereo, 1, 2, packet, sizeof(packet), abort_packet, &visits)
        == SOLAR_OS_NATIVE_ERROR_CANCELLED && visits == 1);
    assert(sender.sequence == 101 && sender.timestamp == 9001);

    solar_os_rtp_jpeg_view_t view;
    uint8_t scan[400];
    for (size_t i = 0; i < sizeof(scan); i++) scan[i] = (uint8_t)(i % 251U);
    make_jpeg_view(&view, scan, SIZE_MAX);
    assert(rtp_api->jpeg_packetize(&sender, &view, packet, sizeof(packet), abort_packet, &visits)
        == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && visits == 1);
    view.scan_len = sizeof(scan);
    sender.timestamp = 9000;
    packet_capture_t capture = {0};
    assert(!rtp_api->jpeg_packetize(&sender, &view, packet, sizeof(packet), capture_packet, &capture));
    assert(capture.count > 1);
    solar_os_native_handle_t id;
    uint8_t buffer[8192];
    assert(rtp_api->jpeg_receiver_open(96, buffer, 16, &id) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!id && !receiver_allocations);
    assert(!rtp_api->jpeg_receiver_open(96, buffer, sizeof(buffer), &id));
    for (size_t i = 0; i < capture.count; i++) {
        bool complete;
        const bool last = i + 1 == capture.count;
        const int result = rtp_api->jpeg_receiver_feed(id, capture.packets[i], capture.lengths[i],
            native_frame, &id, &complete);
        assert(complete == last && result == (last ? SOLAR_OS_NATIVE_ERROR_CANCELLED : 0));
    }
    solar_os_native_rtp_jpeg_stats_v1_t stats;
    assert(!rtp_api->jpeg_receiver_status(id, &stats) && stats.frames == 1 && !stats.dropped_frames && !stats.collecting);
    assert(!rtp_api->jpeg_receiver_reset(id) && !rtp_api->jpeg_receiver_close(id) && !receiver_allocations);
    assert(rtp_api->jpeg_receiver_reset(id) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}
#endif

int main(void)
{
    test_release_and_clock();
    test_rtp_header();
    test_l16_packetization();
    test_rtcp_packets();
    test_jpeg_round_trip();
    test_jpeg_loss_drops_frame_and_recovers();
    test_rtsp_request_parser();
#ifdef TEST_NATIVE_MEDIA_PROTOCOL
    test_native_protocol();
#endif
    puts("Media service tests: ok");
    return 0;
}
