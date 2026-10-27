/* SPDX-License-Identifier: MIT */
#include "dlp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 256
#define HEIGHT 128
#define SIZE (WIDTH * HEIGHT / 8)
typedef struct {
  dlp_device_t d;
  dlp_device_window_t window;
  dlp_stream_t parser;
  uint8_t image[SIZE], reply[256], held[256];
  size_t reply_size, reply_at, bytes;
  uint64_t now, available, complete_at, reply_delay, write_available;
  uint32_t row, y, x;
  unsigned presents, writes, requests, pushes, attempts, format;
  bool spi, compact, drop, duplicate, reorder, corrupt, drop_ack, reset;
  bool forever_busy, bad_ack, drop_completion;
  int write_error, present_error;
} model_t;
static model_t m;
static uint64_t now_us(void *ctx) { return ++((model_t *)ctx)->now; }
static void sleep_us(void *ctx, uint32_t us) { ((model_t *)ctx)->now += us; }
static int configure(void *ctx, unsigned format) {
  ((model_t *)ctx)->format = format;
  return DLP_OK;
}
static int begin_frame(void *ctx, uint32_t frame, bool full) {
  (void)ctx; (void)frame; (void)full; return DLP_OK;
}
static int begin_rect(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  model_t *p = ctx; (void)h;
  p->row = dlp_row_bytes(w, p->format); p->x = x; p->y = y;
  return DLP_OK;
}
static int write_data(void *ctx, uint32_t offset, const uint8_t *data, size_t n) {
  model_t *p = ctx;
  if (p->write_error) return p->write_error;
  for (size_t k = 0; k < n; k++) {
    unsigned pos = offset + (unsigned)k;
    p->image[(p->y + pos / p->row) * WIDTH / 8 + p->x / 8 + pos % p->row] = data[k];
  }
  p->writes++; return DLP_OK;
}
static int present(void *ctx) {
  model_t *p = ctx; p->presents++; p->complete_at = p->now + 30000;
  return DLP_OK;
}
static int poll_present(void *ctx) {
  model_t *p = ctx;
  if (p->forever_busy || p->now < p->complete_at) return DLP_BUSY;
  return p->present_error;
}
static int abort_frame(void *ctx) { (void)ctx; return DLP_OK; }
static void queue(model_t *p, const uint8_t *cell, bool compact) {
  dlp_cell_t a;
  assert(dlp_decode(&a, cell));
  if (p->drop_ack && a.opcode == DLP_WINDOW_SYNC) { p->drop_ack = false; return; }
  if (p->drop_completion && a.opcode == DLP_WINDOW_SYNC &&
      dlp_get_u32(a.payload + 20)) { p->drop_completion = false; return; }
  uint8_t changed[256];
  if (p->bad_ack && a.opcode == DLP_WINDOW_SYNC) {
    dlp_put_u32(a.payload + 4, UINT32_MAX);
    assert(dlp_encode(changed, &a)); cell = changed;
  }
  /* Simulated one-cell TX slot: a duplicate reply may meet an undrained
   * completion. Keep the first reply; exact retry can recover either loss. */
  if (p->reply_size) return;
  if (p->spi) { memcpy(p->reply, cell, 256); p->reply_size = 256; }
  else p->reply_size = dlp_stream_encode(p->reply, cell, compact);
  p->reply_at = 0; p->available = p->now + (p->spi ? 0 : p->reply_delay ? p->reply_delay : 10000);
}
static void tick(model_t *p) {
  dlp_device_tick(&p->d, p->now);
  uint8_t reply[256];
  if (!p->reply_size && dlp_device_take_reply(&p->d, reply)) queue(p, reply, p->compact);
}
static void receive(model_t *p, const uint8_t *cell, bool compact) {
  uint8_t out[256], changed[256];
  if (cell[6] == DLP_PUSH) {
    p->pushes++;
    if (p->reset) { p->d.boot++; p->reset = false; }
    uint32_t seq = dlp_get_u32(cell + 12);
    if (seq == 3 && p->drop) { p->drop = false; return; }
    if (seq == 3 && p->corrupt) {
      memcpy(changed, cell, 256); changed[50] ^= 1;
      cell = changed; p->corrupt = false;
    }
    if (seq == 2 && p->reorder) { memcpy(p->held, cell, 256); return; }
    assert(!dlp_device_receive(&p->d, cell, out, p->now));
    if (p->duplicate) assert(!dlp_device_receive(&p->d, cell, out, p->now));
    if (seq == 3 && p->reorder) {
      p->reorder = false;
      assert(!dlp_device_receive(&p->d, p->held, out, p->now));
    }
    return;
  }
  if (cell[6] == DLP_REQUEST) p->requests++;
  if (dlp_device_receive(&p->d, cell, out, p->now)) queue(p, out, compact);
}
static int write_stream(void *ctx, const uint8_t *data, size_t n) {
  model_t *p = ctx;
  if (p->now < p->write_available) return 0;
  if (++p->attempts % 7 == 0) return 0;
  if (n > 11) n = 11;
  p->bytes += n;
  for (size_t k = 0; k < n; k++) {
    uint8_t cell[256]; bool compact;
    if (dlp_stream_feed(&p->parser, data[k], cell, &compact)) receive(p, cell, compact);
  }
  return (int)n;
}
static int read_stream(void *ctx, uint8_t *data, size_t n) {
  model_t *p = ctx; tick(p);
  if (!p->reply_size || p->now < p->available) return 0;
  assert(n);
  data[0] = p->reply[p->reply_at++];
  if (p->reply_at == p->reply_size) p->reply_size = 0;
  return 1;
}
static int exchange(void *ctx, const uint8_t tx[256], uint8_t rx[256],
                    uint32_t hz, uint32_t gap, uint64_t deadline) {
  model_t *p = ctx; (void)hz; (void)gap;
  if (p->now + 2000 >= deadline) return -1;
  p->now += 2000; p->bytes += 256; tick(p);
  if (p->reply_size) { memcpy(rx, p->reply, 256); p->reply_size = 0; }
  else dlp_device_idle(&p->d, rx);
  receive(p, tx, false);
  return 0;
}
static void setup(dlp_host_t *h, dlp_stream_host_t *stream,
                  dlp_host_window_t *window, unsigned size, bool spi,
                  bool compact, bool enabled) {
  memset(&m, 0, sizeof(m)); m.spi = spi; m.compact = compact;
  dlp_info_t info = {.width = WIDTH, .height = HEIGHT, .formats = 1,
    .max_clock_hz = 2000000, .max_rect_bytes = SIZE, .command_ms = 1000,
    .present_ms = 100, .capabilities = DLP_PRESERVE_CONTENT | DLP_SYNC | DLP_RLE | DLP_LZ4};
  dlp_backend_t b = {.ctx = &m, .configure = configure, .begin_frame = begin_frame,
    .begin_rect = begin_rect, .write = write_data, .present = present,
    .poll_present = poll_present, .abort = abort_frame};
  assert(dlp_device_init(&m.d, &info, 42, &b));
  if (enabled) assert(dlp_device_enable_window(&m.d, &m.window, size));
  dlp_stream_io_t io = {.ctx = &m, .now_us = now_us, .sleep_us = sleep_us,
    .read = read_stream, .write = write_stream};
  assert(dlp_stream_host_init(stream, &io));
  stream->legacy = !compact; stream->retry_us = 50000;
  dlp_transport_t t = spi ? (dlp_transport_t){.ctx = &m, .now_us = now_us,
    .sleep_us = sleep_us, .exchange = exchange} : dlp_stream_transport(stream);
  dlp_host_init(h, &t);
  assert(!dlp_host_discover(h, 3000000));
  assert(!dlp_host_configure(h, 123, DLP_MONO1, 2000000));
  assert(!dlp_host_extensions(h));
  assert(!dlp_host_negotiate_window(h, window, DLP_MAX_WINDOW));
  assert(window->active == enabled);
  if (enabled) assert(window->size == size);
}
static void exercise(unsigned window, bool spi, bool compact, unsigned fault, bool compressed) {
  dlp_host_t host; dlp_stream_host_t stream; dlp_host_window_t storage;
  setup(&host, &stream, &storage, window, spi, compact, true);
  host.compressed = compressed;
  assert(!dlp_host_extensions(&host));
  m.drop = fault == 1; m.duplicate = fault == 2; m.reorder = fault == 3;
  m.corrupt = fault == 4; m.drop_ack = fault == 5;
  m.drop_completion = fault == 6;
  uint8_t image[SIZE], retained[SIZE]; bool valid = false;
  for (size_t k = 0; k < SIZE; k++) image[k] = compressed ? (uint8_t)(k % 16) : (uint8_t)(k * 47 + k / 17);
  assert(!dlp_host_upload_frame(&host, 1, image, SIZE, retained, &valid));
  assert(valid && m.presents == 1 && !memcmp(image, m.image, SIZE));
  /* Raw uploads have 20 backend writes, even after loss/duplication/reordering. */
  assert(m.writes == (compressed ? SIZE / DLP_RLE_MAX_DECODED :
                      (SIZE + DLP_DATA_SIZE - 1) / DLP_DATA_SIZE));
  /* An evicted duplicate cannot repeat a frame or become a new operation. */
  if (window == 16 && !compressed) {
    dlp_cell_t old = {.kind = DLP_PUSH, .opcode = DLP_PRESENT,
      .boot = host.boot, .session = host.session, .sequence = 1, .length = 4};
    uint8_t cell[256], out[256];
    assert(dlp_encode(cell, &old));
    assert(!dlp_device_receive(&m.d, cell, out, m.now));
    assert(!m.window.error && m.presents == 1);
  }
  image[3] ^= 1;
  assert(!dlp_host_upload_frame(&host, 2, image, SIZE, retained, &valid));
  assert(valid && m.presents == 2 && !memcmp(image, m.image, SIZE));
  assert(!dlp_host_upload_frame(&host, 3, image, SIZE, retained, &valid));
  assert(m.presents == 2);
  /* Exact final checkpoint retries must replay a completed frame, no present. */
  uint8_t response[256];
  assert(dlp_device_receive(&m.d, m.d.replay.request, response, m.now));
  assert(m.presents == 2);
}
static void failures(void) {
  for (unsigned kind = 0; kind < 5; kind++) {
    dlp_host_t host; dlp_stream_host_t stream; dlp_host_window_t storage;
    setup(&host, &stream, &storage, 16, false, true, true);
    m.write_error = kind == 0 ? DLP_IO_ERROR : 0;
    m.present_error = kind == 1 ? DLP_IO_ERROR : 0;
    m.forever_busy = kind == 2; m.bad_ack = kind == 3; m.reset = kind == 4;
    uint8_t image[SIZE] = {0}, retained[SIZE]; bool valid = false;
    int r = dlp_host_upload_frame(&host, 1, image, SIZE, retained, &valid);
    assert(r == (kind < 2 ? DLP_IO_ERROR : kind == 2 ? DLP_TIMEOUT :
                 kind == 3 ? DLP_INVALID_CELL : DLP_STALE_BOOT));
    assert(!valid && !host.configured && !storage.collecting);
    if (kind < 3) assert(!m.d.status.completed_frame && m.d.need_full);
    if (!kind) assert(!m.presents && m.window.error == DLP_IO_ERROR);
  }
}
static void rules(void) {
  assert(dlp_host_begin_frame(NULL, 1, true) == DLP_BAD_ARGUMENT);
  assert(dlp_host_present(NULL, 1) == DLP_BAD_ARGUMENT);
  dlp_host_t h; dlp_stream_host_t stream; dlp_host_window_t w;
  setup(&h, &stream, &w, 16, false, true, false);
  uint8_t image[SIZE] = {0}, retained[SIZE]; bool valid = false;
  assert(!dlp_host_upload_frame(&h, 1, image, SIZE, retained, &valid));
  assert(valid && m.presents == 1); /* Genuine old receiver: UNSUPPORTED. */
  for (unsigned kind = 0; kind < 4; kind++) {
    setup(&h, &stream, &w, 4, false, true, true);
    dlp_cell_t q = {.kind = DLP_PUSH, .opcode = DLP_BEGIN_FRAME,
      .boot = h.boot, .session = h.session, .sequence = 1, .length = 8};
    dlp_put_u32(q.payload, 1); dlp_put_u32(q.payload + 4, 1);
    uint8_t cell[256], out[256];
    if (kind == 0) q.sequence = 5; /* Credit violation. */
    if (kind == 1) q.opcode = DLP_ABORT_FRAME;
    if (kind == 2) q.session++; /* Fenced: no state effects. */
    assert(dlp_encode(cell, &q));
    assert(!dlp_device_receive(&m.d, cell, out, m.now));
    if (kind == 3) {
      assert(m.window.applied == 1);
      q.payload[0] = 2; assert(dlp_encode(cell, &q));
      assert(!dlp_device_receive(&m.d, cell, out, m.now));
    }
    assert(m.window.error == (kind == 1 ? DLP_UNSUPPORTED :
           kind == 2 ? DLP_OK : DLP_BAD_SEQUENCE));
    assert(!dlp_host_configure(&h, 124, DLP_MONO1, 2000000));
    assert(!m.window.active && !m.window.error && !w.active && m.d.need_full);
  }
  setup(&h, &stream, &w, 1, false, true, true);
  w.sent = w.acknowledged = m.window.applied = UINT32_MAX;
  w.grant = m.window.grant = UINT32_MAX;
  valid = false;
  assert(dlp_host_upload_frame(&h, 1, image, SIZE, retained, &valid) == DLP_BAD_SEQUENCE);
  assert(!valid && !h.configured && !m.presents);
}
static void round_trips(void) {
  unsigned legacy = 0;
  for (unsigned enabled = 0; enabled < 2; enabled++) {
    dlp_host_t h; dlp_stream_host_t stream; dlp_host_window_t w;
    setup(&h, &stream, &w, 16, false, true, enabled != 0);
    uint8_t image[SIZE] = {0}, retained[SIZE]; bool valid = false;
    unsigned before = m.requests; uint64_t start = m.now;
    assert(!dlp_host_upload_frame(&h, 1, image, SIZE, retained, &valid));
    unsigned count = m.requests - before;
    if (!enabled) legacy = count;
    else { assert(count == 1 && count < legacy);
      printf("DLP 10ms link model: legacy %u round trips, window %u (%llu us incl. 30ms presentation)\n",
        legacy, count, (unsigned long long)(m.now - start)); }
  }
}
static void slow_transport_budget(void) {
  for (unsigned slow_write = 0; slow_write < 2; slow_write++) {
    dlp_host_t h; dlp_stream_host_t stream; dlp_host_window_t w;
    setup(&h, &stream, &w, 8, false, true, true);
    h.transport.minimum_timeout_ms = 5000;
    if (slow_write) m.write_available = m.now + 1500000;
    else { m.reply_delay = 1500000; stream.retry_us = 2000000; }
    uint8_t image[SIZE], retained[SIZE]; bool valid = false;
    for (size_t i = 0; i < SIZE; i++) image[i] = (uint8_t)(i * 47 + i / 17);
    uint64_t started = m.now;
    assert(!dlp_host_upload_frame(&h, 1, image, SIZE, retained, &valid));
    assert(valid && m.presents == 1 && !memcmp(image, m.image, SIZE));
    assert(m.now - started >= 1500000);
  }
}
int main(void) {
  for (unsigned spi = 0; spi < 2; spi++)
    for (unsigned compact = 0; compact < 2; compact++)
      for (unsigned fault = 0; fault < 7; fault++)
        for (unsigned compressed = 0; compressed < 2; compressed++)
          exercise(16, spi != 0, compact != 0, fault, compressed != 0);
  exercise(1, false, true, 1, false);
  exercise(1, false, true, 1, true);
  failures(); rules(); round_trips(); slow_transport_budget();
  puts("DLP windows: SPI/streams, backpressure, loss, corruption, duplicates, reordering, replay, completion and fencing PASS");
  return 0;
}
