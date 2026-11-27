#include "solar_os_native_media_protocol.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_MEDIA
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_rtp_jpeg.h"
#include "solar_os_rtsp.h"

static int clock_init(solar_os_media_clock_t *clock, uint64_t origin, uint32_t timestamp, uint32_t rate)
{ return solar_os_native_result(solar_os_media_clock_init(clock, origin, timestamp, rate)); }

static int clock_map(const solar_os_media_clock_t *clock, uint64_t timestamp_us, uint32_t *timestamp)
{ return solar_os_native_result(solar_os_media_clock_map(clock, timestamp_us, timestamp)); }

static int header_encode(const solar_os_rtp_header_t *header, uint8_t *packet, size_t capacity)
{ return solar_os_native_result(solar_os_rtp_header_encode(header, packet, capacity)); }

static int header_decode(const uint8_t *packet, size_t length, solar_os_rtp_header_t *header,
    const uint8_t **payload, size_t *payload_length)
{ return solar_os_native_result(solar_os_rtp_header_decode(packet, length, header, payload, payload_length)); }

typedef struct {
    solar_os_native_rtp_packet_fn visit;
    void *user;
    int result;
} packet_visitor_t;

static esp_err_t packet_visit(const uint8_t *packet, size_t bytes, void *opaque)
{
    packet_visitor_t *visitor = opaque;
    visitor->result = visitor->visit(packet, bytes, visitor->user);
    return visitor->result ? ESP_FAIL : ESP_OK;
}

static int l16_packetize(solar_os_rtp_sender_t *sender, const int16_t *samples, size_t frames,
    uint8_t channels, uint8_t *packet, size_t capacity, solar_os_native_rtp_packet_fn visit, void *user)
{
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    packet_visitor_t visitor = {.visit = visit, .user = user};
    const esp_err_t error = solar_os_rtp_l16_packetize(sender, samples, frames, channels, packet,
        capacity, packet_visit, &visitor);
    return visitor.result ? visitor.result : solar_os_native_result(error);
}

static int sender_report(uint32_t ssrc, uint32_t ntp_seconds, uint32_t ntp_fraction,
    uint32_t timestamp, uint32_t packets, uint32_t octets, const char *cname, uint8_t *packet,
    size_t capacity, size_t *length)
{ return solar_os_native_result(solar_os_rtcp_sender_report(ssrc, ntp_seconds, ntp_fraction,
    timestamp, packets, octets, cname, packet, capacity, length)); }

static int bye(uint32_t ssrc, uint8_t *packet, size_t capacity, size_t *length)
{ return solar_os_native_result(solar_os_rtcp_bye(ssrc, packet, capacity, length)); }

static int jpeg_parse(const uint8_t *jpeg, size_t length, solar_os_rtp_jpeg_view_t *view)
{ return solar_os_native_result(solar_os_rtp_jpeg_parse(jpeg, length, view)); }

static int jpeg_packetize(solar_os_rtp_sender_t *sender, const solar_os_rtp_jpeg_view_t *view,
    uint8_t *packet, size_t capacity, solar_os_native_rtp_packet_fn visit, void *user)
{
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    packet_visitor_t visitor = {.visit = visit, .user = user};
    const esp_err_t error = solar_os_rtp_jpeg_packetize(sender, view, packet, capacity, packet_visit, &visitor);
    return visitor.result ? visitor.result : solar_os_native_result(error);
}

#define RECEIVER_KIND SOLAR_OS_NATIVE_HANDLE_RTP_JPEG_RECEIVER

static int receiver_destroy(void *receiver)
{
    solar_os_memory_free(receiver);
    return SOLAR_OS_NATIVE_OK;
}

static int receiver_open(uint8_t payload_type, uint8_t *buffer, size_t capacity, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_rtp_jpeg_receiver_t *receiver = solar_os_memory_alloc(sizeof(*receiver),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtp.jpeg.receiver");
    if (!receiver) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = solar_os_native_result(solar_os_rtp_jpeg_receiver_init(receiver, payload_type, buffer, capacity));
    if (!result) {
        const solar_os_app_t *owner = solar_os_application_descriptor();
        result = owner ? solar_os_native_handle_create_owned(RECEIVER_KIND, receiver, receiver_destroy, owner, out) :
            solar_os_native_handle_create(RECEIVER_KIND, receiver, receiver_destroy, out);
    }
    if (result) solar_os_memory_free(receiver);
    return result;
}

static int receiver_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, RECEIVER_KIND); }

static int receiver_reset(solar_os_native_handle_t id)
{
    void *receiver;
    const int result = solar_os_native_handle_acquire_exclusive(id, RECEIVER_KIND, &receiver);
    if (result) return result;
    solar_os_rtp_jpeg_receiver_reset(receiver);
    solar_os_native_handle_release_exclusive(id);
    return SOLAR_OS_NATIVE_OK;
}

static int receiver_feed(solar_os_native_handle_t id, const uint8_t *packet, size_t length,
    solar_os_native_rtp_jpeg_frame_fn visit, void *user, bool *complete)
{
    if (!complete) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *complete = false;
    void *receiver;
    int result = solar_os_native_handle_acquire_exclusive(id, RECEIVER_KIND, &receiver);
    if (result) return result;
    solar_os_rtp_jpeg_frame_t frame;
    result = solar_os_native_result(solar_os_rtp_jpeg_receiver_feed(receiver, packet, length, &frame));
    if (!result && frame.data) {
        *complete = true;
        if (visit) result = visit(&frame, user);
    }
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int receiver_status(solar_os_native_handle_t id, solar_os_native_rtp_jpeg_stats_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_rtp_jpeg_stats_v1_t){0};
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, RECEIVER_KIND, &state);
    if (result) return result;
    const solar_os_rtp_jpeg_receiver_t *receiver = state;
    *out = (solar_os_native_rtp_jpeg_stats_v1_t){.frames = receiver->frames,
        .dropped_frames = receiver->dropped_frames, .collecting = receiver->collecting};
    solar_os_native_handle_release_exclusive(id);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_rtp_api_v1_t rtp_api = {
    .abi_version = SOLAR_OS_NATIVE_RTP_ABI, .struct_size = sizeof(rtp_api),
    .track_valid = solar_os_media_track_valid, .clock_init = clock_init, .clock_map = clock_map,
    .header_encode = header_encode, .header_decode = header_decode, .sender_valid = solar_os_rtp_sender_valid,
    .l16_packetize = l16_packetize, .sender_report = sender_report, .bye = bye,
    .jpeg_parse = jpeg_parse, .jpeg_packetize = jpeg_packetize, .jpeg_receiver_open = receiver_open,
    .jpeg_receiver_close = receiver_close, .jpeg_receiver_reset = receiver_reset,
    .jpeg_receiver_feed = receiver_feed, .jpeg_receiver_status = receiver_status,
};

static int rtsp_parse_request(const uint8_t *data, size_t length, solar_os_rtsp_request_t *request)
{ return solar_os_native_result(solar_os_rtsp_parse_request(data, length, request)); }

static const solar_os_native_rtsp_protocol_api_v1_t rtsp_protocol_api = {
    .abi_version = SOLAR_OS_NATIVE_RTSP_PROTOCOL_ABI, .struct_size = sizeof(rtsp_protocol_api),
    .header_length = solar_os_rtsp_header_length, .parse_request = rtsp_parse_request,
    .method_name = solar_os_rtsp_method_name,
};
#endif

const void *solar_os_native_media_protocol_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_media_protocol_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
