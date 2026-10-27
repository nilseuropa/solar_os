#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_link_stream.c"

struct test_queue { size_t size, used, head; uint8_t bytes[]; };
static unsigned queue_count;
static uint32_t now_ms, random_value = 100;
static size_t frame_mtu = 250;
static solar_os_port_driver_t ports[2];
static bool online[2];
static bool transport_backpressure;
static uint8_t window_limit = LINK_STREAM_WINDOW_SIZE;
static bool scheduling_test;
static uint32_t scheduling_bytes[2];
static unsigned lose_open, lose_ack, lose_data;
static bool duplicate_data;
static bool reopen_during_send;
typedef struct { unsigned destination; solar_os_link_message_t message; } packet_t;
static packet_t packets[64];
static size_t packet_head, packet_count;

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) { size_t c = n < size ? n : size - 1; memcpy(dst, src, c); dst[c] = 0; }
    return n;
}
uint32_t esp_random(void) { return ++random_value; }
TickType_t xTaskGetTickCount(void) { return now_ms; }
void vTaskDelay(TickType_t ticks) { now_ms += ticks; }
QueueHandle_t solar_os_queue_create(UBaseType_t length, UBaseType_t size) {
    assert(size == 1);
    QueueHandle_t q = calloc(1, sizeof(*q) + length);
    assert(q); q->size = length; queue_count++; return q;
}
void solar_os_queue_delete(QueueHandle_t q) { if (q) { assert(queue_count); queue_count--; free(q); } }
void xQueueReset(QueueHandle_t q) { q->head = q->used = 0; }
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->used; }
unsigned uxQueueSpacesAvailable(QueueHandle_t q) { return q->size - q->used; }
int xQueueSend(QueueHandle_t q, const void *p, unsigned ticks) {
    if (q->used == q->size) { now_ms += ticks; return pdFALSE; }
    q->bytes[(q->head + q->used++) % q->size] = *(const uint8_t *)p; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *p, unsigned ticks) {
    if (!q->used) { now_ms += ticks; return pdFALSE; }
    *(uint8_t *)p = q->bytes[q->head++ % q->size]; q->used--; return pdTRUE;
}
esp_err_t solar_os_port_register(const solar_os_port_driver_t *driver) {
    unsigned i = driver->name[0] == 'a' ? 0 : 1;
    assert(!ports[i].name); ports[i] = *driver; return ESP_OK;
}
esp_err_t solar_os_port_unregister(const char *name) {
    unsigned i = name[0] == 'a' ? 0 : 1;
    if (((link_stream_t *)ports[i].user)->port_open) return ESP_ERR_INVALID_STATE;
    memset(&ports[i], 0, sizeof(ports[i])); return ESP_OK;
}
esp_err_t solar_os_link_get_status(const char *name, solar_os_link_status_t *status) {
    *status = (solar_os_link_status_t){.local_id = name[0] == 'a' ? 1 : 2, .frame_mtu = frame_mtu};
    status->tx_queued = transport_backpressure ? 1 : 0;
    return ESP_OK;
}
esp_err_t solar_os_link_send(const char *name, solar_os_link_message_type_t type,
    uint32_t destination, const void *data, size_t len, uint16_t *sequence) {
    (void)sequence;
    unsigned source = name[0] == 'a' ? 0 : 1, target = 1 - source;
    if (!online[source]) return ESP_ERR_INVALID_STATE;
    const uint8_t *p = data;
    if (scheduling_test) {
        assert(source == 0 && destination >= 2 && destination <= 3);
        if (p[3] == LINK_STREAM_OPCODE_DATA) {
            scheduling_bytes[destination-2] += (uint32_t)(len-LINK_STREAM_HEADER_SIZE);
            solar_os_link_message_t ack = {.type=type, .source=destination,
                .destination=1, .payload_len=LINK_STREAM_HEADER_SIZE};
            ack.payload[0]=LINK_STREAM_MAGIC_0; ack.payload[1]=LINK_STREAM_MAGIC_1;
            ack.payload[2]=LINK_STREAM_VERSION; ack.payload[3]=LINK_STREAM_OPCODE_ACK;
            stream_write_u32(ack.payload+4, 1000+destination);
            memcpy(ack.payload+10, p+4, 4);
            memcpy(ack.payload+14, p+8, 2);
            stream_write_u32(ack.payload+16, scheduling_bytes[destination-2]+2048);
            assert(solar_os_link_stream_ingest("a", &ack, now_ms) == ESP_OK);
        }
        return ESP_OK;
    }
    assert(type == SOLAR_OS_LINK_MESSAGE_STREAM && destination == target + 1);
    assert(len >= 20 && p[0] == 0x53 && p[1] == 0x56 && p[2] == 3);
    if (reopen_during_send && source == 0 && p[3] == LINK_STREAM_OPCODE_OPEN) {
        reopen_during_send = false;
        assert(ports[0].close(ports[0].user) == ESP_OK);
        assert(ports[0].open(ports[0].user) == ESP_OK);
    }
    if (!online[target]) return ESP_OK;
    if (p[3] == LINK_STREAM_OPCODE_OPEN && lose_open) { lose_open--; return ESP_OK; }
    if (p[3] == LINK_STREAM_OPCODE_ACK && lose_ack) { lose_ack--; return ESP_OK; }
    if (p[3] == LINK_STREAM_OPCODE_DATA && lose_data) { lose_data--; return ESP_OK; }
    unsigned copies = p[3] == LINK_STREAM_OPCODE_DATA && duplicate_data ? 2 : 1;
    for (unsigned i = 0; i < copies; i++) {
        assert(packet_count < 64);
        packet_t *packet = &packets[(packet_head + packet_count++) % 64];
        *packet = (packet_t){.destination = target, .message = {
            .source = source + 1, .destination = target + 1, .type = type, .payload_len = len}};
        memcpy(packet->message.payload, p, len);
    }
    return ESP_OK;
}
static const char *links[2] = {"a", "b"};
static const char *names[2] = {"a0", "b0"};
static void pump(unsigned duration) {
    uint32_t until = now_ms + duration;
    while ((int32_t)(until - now_ms) > 0) {
        for (unsigned i = 0; i < 2; i++) if (online[i])
            solar_os_link_stream_process_with_window(links[i], now_ms, window_limit);
        while (packet_count) {
            packet_t packet = packets[packet_head++ % 64]; packet_count--;
            if (!online[packet.destination]) continue;
            (void)solar_os_link_stream_ingest(links[packet.destination], &packet.message, now_ms);

        }
        now_ms++;
    }
}
static void write_bytes(unsigned i, const uint8_t *data, size_t len) {
    size_t n;
    assert(ports[i].write(ports[i].user, data, len, &n) == ESP_OK && n == len);
}
static void read_bytes(unsigned i, const uint8_t *expected, size_t len) {
    uint8_t data[2049]; size_t n;
    assert(len < sizeof(data));
    assert(ports[i].read(ports[i].user, data, sizeof(data), 0, &n) == ESP_OK);
    assert(n == len && !memcmp(data, expected, len));
    assert(ports[i].read(ports[i].user, data, sizeof(data), 0, &n) == ESP_ERR_TIMEOUT && !n);
}
static void exchange(void) {
    uint8_t a[391], b[271];
    for (size_t i = 0; i < sizeof(a); i++) a[i] = (uint8_t)(i * 17 + now_ms);
    for (size_t i = 0; i < sizeof(b); i++) b[i] = (uint8_t)(i * 29 + now_ms);
    write_bytes(0, a, sizeof(a)); write_bytes(1, b, sizeof(b)); pump(4000);
    read_bytes(1, a, sizeof(a)); read_bytes(0, b, sizeof(b));
    assert(streams[0].tx_window_count == 0 && streams[1].tx_window_count == 0);
    uint32_t a_epoch = streams[0].local_session, b_epoch = streams[1].local_session;
    pump(16000); /* Include periodic OPENs, not just the first successful byte. */
    assert(a_epoch == streams[0].local_session && b_epoch == streams[1].local_session);
}
static void credit_window_test(void) {
    uint8_t full[2048], next[777];
    for (size_t i = 0; i < sizeof(full); i++) full[i] = (uint8_t)(i * 13);
    memset(next, 0x95, sizeof(next));
    /* A reader may stop indefinitely. More than one window must fit exactly,
     * with no overflow, retransmission storm or prematurely acknowledged data. */
    pump(100);
    uint32_t dropped = streams[1].counters.dropped;
    write_bytes(0, full, sizeof(full)); pump(3000);
    assert(uxQueueMessagesWaiting(streams[1].rx_queue) == sizeof(full));
    assert(!streams[0].tx_window_count);
    write_bytes(0, next, sizeof(next)); pump(3000);
    assert(uxQueueMessagesWaiting(streams[0].tx_queue) == sizeof(next));
    assert(streams[1].counters.dropped == dropped);
    /* Drop the only standalone window-opening ACK. The sender must probe and
     * recover without a port reset or the periodic 15-second announcement. */
    read_bytes(1, full, sizeof(full)); lose_ack = 1; pump(3000);
    assert(!lose_ack);
    read_bytes(1, next, sizeof(next)); pump(100);
    assert(!streams[0].tx_window_count);
    /* Replaying an old receive edge cannot shrink or inflate current credit. */
    uint32_t edge = streams[0].remote_receive_edge;
    solar_os_link_message_t stale = {.source=2, .destination=1,
        .type=SOLAR_OS_LINK_MESSAGE_STREAM, .payload_len=LINK_STREAM_HEADER_SIZE};
    stale.payload[0]=LINK_STREAM_MAGIC_0; stale.payload[1]=LINK_STREAM_MAGIC_1;
    stale.payload[2]=LINK_STREAM_VERSION; stale.payload[3]=LINK_STREAM_OPCODE_ACK;
    stream_write_u32(stale.payload+4, streams[1].local_session);
    stream_write_u32(stale.payload+10, streams[0].local_session);
    stream_write_u32(stale.payload+16, 2048);
    assert(solar_os_link_stream_ingest("a", &stale, now_ms) == ESP_OK);
    assert(streams[0].remote_receive_edge == edge);
}
static void burst_and_wrap_test(void) {
    uint8_t bytes[1800]; memset(bytes, 0x41, sizeof(bytes));
    pump(100);
    /* Force both sequence and cumulative byte-credit rollover within a burst. */
    streams[0].next_tx_sequence = UINT16_MAX - 2;
    streams[1].expected_rx_sequence = UINT16_MAX - 2;
    streams[0].tx_assigned_bytes = UINT32_MAX - 500;
    streams[0].remote_receive_edge = streams[0].tx_assigned_bytes + 2048U;
    streams[1].rx_received_bytes = streams[0].tx_assigned_bytes;
    streams[1].advertised_receive_edge = streams[0].remote_receive_edge;
    uint32_t before = streams[0].counters.bytes_sent;
    write_bytes(0, bytes, sizeof(bytes));
    pump(100); /* Old stop-and-wait + 40ms ACK cannot finish this in 100ms. */
    read_bytes(1, bytes, sizeof(bytes));
    assert(streams[0].counters.bytes_sent - before == sizeof(bytes));
    assert(!streams[0].tx_window_count);
    pump(100);
    /* Drop the leading packet of an eight-frame burst. Later frames arrive
     * out of order, then go-back-N restores the exact byte sequence once. */
    lose_data = 1; write_bytes(0, bytes, sizeof(bytes)); pump(3000);
    read_bytes(1, bytes, sizeof(bytes)); assert(!lose_data);
    pump(100);
}
static void transport_backpressure_test(void) {
    uint8_t bytes[1000]; memset(bytes, 0xb7, sizeof(bytes));
    pump(100);
    uint32_t sent = streams[0].counters.frames_sent;
    transport_backpressure = true;
    write_bytes(0, bytes, sizeof(bytes)); pump(2000);
    assert(streams[0].counters.frames_sent == sent);
    assert(uxQueueMessagesWaiting(streams[0].tx_queue) == sizeof(bytes));
    assert(!packet_count);
    transport_backpressure = false;
    pump(2000); read_bytes(1, bytes, sizeof(bytes)); pump(100);
    assert(!streams[0].tx_window_count);
    /* A raw half-duplex radio must get a receive turn between sends, even
     * when its consumer has enough bytes to fill the configured bulk window. */
    window_limit = 1;
    write_bytes(0, bytes, sizeof(bytes)); pump(1);
    assert(streams[0].tx_window_count == 1 && streams[0].window_frames == 1);
    pump(200); read_bytes(1, bytes, sizeof(bytes));
    window_limit = LINK_STREAM_WINDOW_SIZE; pump(100);
}
static void stop(unsigned i) {
    online[i] = false; solar_os_link_stream_transport_stopped(links[i]);
    solar_os_link_stream_status_t status;
    assert(solar_os_link_stream_get_status(names[i], &status) == ESP_OK && !status.connected);
}
static void run_pair(void) {
    online[0] = online[1] = true;
    for (unsigned i = 0; i < 2; i++) {
        assert(solar_os_link_stream_create(links[i], names[i], 2 - i) == ESP_OK);
        assert(ports[i].open(ports[i].user) == ESP_OK);
    }
    lose_open = 3; pump(4000); assert(!lose_open);
    exchange();
    credit_window_test();
    burst_and_wrap_test();
    transport_backpressure_test();
    lose_ack = 1; duplicate_data = true; exchange(); duplicate_data = false;
    assert(!lose_ack);
    lose_data = 2; exchange(); assert(!lose_data);
    reopen_during_send = true;
    streams[0].open_sent = false;
    solar_os_link_stream_process(links[0], now_ms);
    assert(!reopen_during_send && !streams[0].open_sent);
    pump(4000); exchange();
    for (unsigned cycle = 0; cycle < 12; cycle++) {
        unsigned i = cycle % 2;
        assert(ports[i].close(ports[i].user) == ESP_OK);
        if (cycle % 3) pump(100); /* Also cover a lost CLOSE / immediate reopen. */
        assert(ports[i].open(ports[i].user) == ESP_OK);
        pump(2500); exchange();
        const uint8_t pending[] = "queued across a transport interruption";
        stop(i); write_bytes(1 - i, pending, sizeof(pending)); pump(2000);
        online[i] = true; pump(4000); read_bytes(i, pending, sizeof(pending)); exchange();
    }
    stop(0); stop(1); pump(50000);
    online[0] = online[1] = true; pump(4000); exchange();
    /* A full queue applies backpressure and closing drops its old bytes. */
    stop(1);
    uint8_t full[2049] = {0}; size_t written = 0;
    assert(ports[0].write(ports[0].user, full, sizeof(full), &written) == ESP_ERR_TIMEOUT);
    assert(written == 2048);
    assert(solar_os_link_stream_remove(names[0]) == ESP_ERR_INVALID_STATE);
    for (unsigned i = 0; i < 2; i++) {
        assert(ports[i].close(ports[i].user) == ESP_OK);
        assert(solar_os_link_stream_remove(names[i]) == ESP_OK);
    }
    packet_count = packet_head = 0;
    assert(!queue_count && !solar_os_link_stream_count());
}
static void meshcore_profile_test(void) {
    frame_mtu = 112; /* Match the MeshCore encrypted stream envelope capacity. */
    solar_os_link_stream_config_t config;
    solar_os_link_stream_config_default(&config);
    config.window_frames = 2;
    config.acknowledgement_delay_ms = 250;
    config.retry_ms = 12000;
    config.retry_jitter_ms = 4000;
    config.open_interval_ms = 30000;
    config.peer_timeout_ms = 180000;
    online[0] = online[1] = true;
    for (unsigned i = 0; i < 2; i++) {
        assert(solar_os_link_stream_create_configured(links[i], names[i], 2-i, &config) == ESP_OK);
        assert(streams[i].data_mtu == 78);
        assert(ports[i].open(ports[i].user) == ESP_OK);
    }
    lose_open = 1; pump(20000);
    const uint8_t key[] = "k", echo[] = "k!";
    write_bytes(0, key, sizeof(key)); pump(20);
    assert(streams[0].tx_window_count == 1); /* Give sparse echo time to piggyback. */
    write_bytes(1, echo, sizeof(echo)); pump(20);
    assert(!streams[0].tx_window_count);
    read_bytes(1, key, sizeof(key)); read_bytes(0, echo, sizeof(echo)); pump(300);
    uint8_t bytes[2048]; memset(bytes, 0xa3, sizeof(bytes));
    write_bytes(0, bytes, sizeof(bytes));
    lose_data = 1; pump(30000);
    read_bytes(1, bytes, sizeof(bytes));
    lose_ack = 1;
    write_bytes(0, bytes, sizeof(bytes)); pump(30000);
    read_bytes(1, bytes, sizeof(bytes));
    assert(!lose_data && !lose_ack);
    for (unsigned i = 0; i < 2; i++) {
        assert(ports[i].close(ports[i].user) == ESP_OK);
        assert(solar_os_link_stream_remove(names[i]) == ESP_OK);
    }
    packet_count = packet_head = 0;
    assert(!queue_count);
    frame_mtu = 250;
}
static void stream_fairness_test(void) {
    scheduling_test = true; online[0] = true;
    uint8_t bytes[2048]; memset(bytes, 0x72, sizeof(bytes));
    for (unsigned i = 0; i < 2; i++) {
        assert(solar_os_link_stream_create("a", names[i], i+2) == ESP_OK);
        assert(ports[i].open(ports[i].user) == ESP_OK);
        streams[i].remote_session = 1002+i;
        streams[i].remote_last_seen_ms = now_ms;
        streams[i].open_sent = true;
        streams[i].next_open_ms = now_ms+15000;
        streams[i].remote_receive_edge = 2048;
        write_bytes(i, bytes, sizeof(bytes));
    }
    /* An immediately ACKed bulk stream stays writable on every turn. It must
     * not starve the other endpoint sharing the same physical link. */
    for (unsigned i = 0; i < 4; i++) solar_os_link_stream_process("a", now_ms++);
    assert(scheduling_bytes[0] && scheduling_bytes[1]);
    for (unsigned i = 0; i < 2; i++) {
        assert(ports[i].close(ports[i].user) == ESP_OK);
        assert(solar_os_link_stream_remove(names[i]) == ESP_OK);
    }
    scheduling_test = false;
    assert(!queue_count);
}
int main(void) {
    run_pair();
    stream_fairness_test();
    meshcore_profile_test();
    now_ms = UINT32_MAX - 50000;
    run_pair();
    puts("Link v3: lost OPEN/DATA/ACK, duplicate data, reopen during send, transport restart, timer/sequence/credit wrap, lost credit, bulk window, backpressure and queue cleanup PASS");
}
