/* SPDX-License-Identifier: MIT */
#include "sdlp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  sdlp_device_t device;
  uint8_t queued[256], pixels[64];
  uint64_t now;
  unsigned writes, presents, delay, drop, corrupt;
  int configure_result, begin_result;
  uint8_t last_request[256];
} fixture_t;
static int configure(void *ctx, unsigned f) {
  (void)f;
  return ((fixture_t *)ctx)->configure_result;
}
static int begin_frame(void *ctx, uint32_t f, bool full) {
  (void)f;
  (void)full;
  return ((fixture_t *)ctx)->begin_result;
}
static int rect(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  (void)ctx;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  return 0;
}
static int write_data(void *ctx, uint32_t off, const uint8_t *p, size_t n) {
  fixture_t *f = ctx;
  assert(off + n <= sizeof(f->pixels));
  memcpy(f->pixels + off, p, n);
  f->writes++;
  return 0;
}
static int present(void *ctx) {
  fixture_t *f = ctx;
  f->presents++;
  return 0;
}
static int poll(void *ctx) {
  fixture_t *f = ctx;
  if (f->delay) {
    f->delay--;
    return SDLP_BUSY;
  }
  return 0;
}
static int abort_frame(void *ctx) {
  (void)ctx;
  return 0;
}
static uint64_t now_us(void *ctx) { return ((fixture_t *)ctx)->now; }
static void sleep_us(void *ctx, uint32_t us) { ((fixture_t *)ctx)->now += us; }
static int exchange(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t hz,
                    uint32_t gap, uint64_t deadline) {
  fixture_t *f = ctx;
  (void)deadline;
  f->now += gap + UINT64_C(2048000000) / hz;
  memcpy(rx, f->queued, 256);
  if (f->corrupt) {
    rx[31] ^= 1;
    f->corrupt--;
  }
  if (f->drop) {
    f->drop--;
    return 0;
  }
  memcpy(f->last_request, tx, 256);
  (void)sdlp_device_receive(&f->device, tx, f->queued, f->now);
  return 0;
}
static void setup(fixture_t *f, sdlp_host_t *host) {
  memset(f, 0, sizeof(*f));
  sdlp_info_t i = {.width = 5,
                   .height = 2,
                   .formats = SDLP_FORMATS,
                   .preferred_format = SDLP_MONO1,
                   .max_clock_hz = 1000000,
                   .min_gap_us = 100,
                   .max_rect_bytes = 64,
                   .command_ms = 100,
                   .present_ms = 100};
  strcpy(i.model, "test");
  strcpy(i.firmware, "1");
  sdlp_backend_t b = {.ctx = f,
                      .configure = configure,
                      .begin_frame = begin_frame,
                      .begin_rect = rect,
                      .write = write_data,
                      .present = present,
                      .poll_present = poll,
                      .abort = abort_frame};
  assert(sdlp_device_init(&f->device, &i, 42, &b));
  sdlp_device_idle(&f->device, f->queued);
  sdlp_transport_t t = {
      .ctx = f, .now_us = now_us, .sleep_us = sleep_us, .exchange = exchange};
  sdlp_host_init(host, &t);
  assert(!sdlp_host_discover(host, 5000000));
  assert(!sdlp_host_configure(host, 123, SDLP_MONO1, 1000000));
}
static void codec(void) {
  uint8_t raw[256];
  sdlp_cell_t c = {0}, q;
  assert(sdlp_crc32("123456789", 9) == 0xcbf43926);
  assert(sdlp_encode(raw, &c));
  assert(sdlp_get_u32(raw + 252) == 0xab525476);
  assert(sdlp_decode(&q, raw));
  c.kind = SDLP_REQUEST;
  c.opcode = SDLP_GET_INFO;
  c.sequence = 17;
  assert(sdlp_encode(raw, &c));
  assert(sdlp_get_u32(raw + 252) == 0x8150d3ce);
  for (size_t n = 0; n < 256; n++) {
    raw[n] ^= 1;
    assert(!sdlp_decode(&q, raw));
    raw[n] ^= 1;
  }
  raw[40] = 1;
  sdlp_put_u32(raw + 252, sdlp_crc32(raw, 252));
  assert(!sdlp_decode(&q, raw));
  assert(sdlp_row_bytes(5, SDLP_MONO1) == 1);
  assert(sdlp_row_bytes(5, SDLP_RGB565_BE) == 10);
}
static void upload_retry(void) {
  fixture_t f;
  sdlp_host_t h;
  setup(&f, &h);
  assert(h.info.width == 5 && h.info.height == 2);
  assert(!sdlp_host_begin_frame(&h, 1, true));
  assert(!sdlp_host_begin_rect(&h, 1, 1, 0, 0, 5, 2));
  uint8_t bad[] = {0x80, 0};
  assert(sdlp_host_write(&h, 1, 1, 0, bad, 2) == SDLP_BAD_ARGUMENT);
  assert(f.writes == 0);
  uint8_t pixels[] = {0x15, 0};
  f.drop = 1;
  assert(!sdlp_host_write(&h, 1, 1, 0, pixels, 2));
  assert(f.writes == 1);
  uint8_t original[256], out[256];
  memcpy(original, f.device.replay.request, 256);
  assert(sdlp_device_receive(&f.device, original, out, f.now));
  assert(f.writes == 1);
  original[44] ^= 1;
  sdlp_put_u32(original + 252, sdlp_crc32(original, 252));
  assert(sdlp_device_receive(&f.device, original, out, f.now));
  sdlp_cell_t a;
  assert(sdlp_decode(&a, out));
  assert(a.result == SDLP_BAD_SEQUENCE);
  f.delay = 3;
  assert(!sdlp_host_present(&h, 1));
  assert(f.presents == 1);
  memcpy(original, f.device.replay.request, 256);
  assert(sdlp_device_receive(&f.device, original, out, f.now));
  assert(f.presents == 1);
  assert(!sdlp_host_wait_frame(&h, 1, f.now + 500000));
  assert(!memcmp(f.pixels, pixels, 2));
  assert(sdlp_host_begin_frame(&h, 2, false) == SDLP_BAD_ARGUMENT);
  assert(!sdlp_host_begin_frame(&h, 2, true));
  assert(sdlp_host_begin_rect(&h, 2, 1, 0, 1, 5, 1) == SDLP_BAD_ARGUMENT);
  assert(sdlp_host_begin_rect(&h, 2, 1, 4, 0, 2, 1) == SDLP_BAD_ARGUMENT);
}
static void reset_timeout(void) {
  fixture_t f;
  sdlp_host_t h;
  setup(&f, &h);
  assert(!sdlp_host_begin_frame(&h, 1, true));
  assert(!sdlp_host_begin_rect(&h, 1, 1, 0, 0, 5, 2));
  uint8_t pixels[2] = {0};
  assert(!sdlp_host_write(&h, 1, 1, 0, pixels, 2));
  f.delay = 100000;
  assert(!sdlp_host_present(&h, 1));
  assert(sdlp_host_wait_frame(&h, 1, f.now + 500000) == SDLP_TIMEOUT);
  assert(f.device.status.state == SDLP_STATE_FAULT);
  f.device.boot = 43;
  sdlp_device_idle(&f.device, f.queued);
  assert(sdlp_host_command(&h, SDLP_GET_STATUS, NULL, 0, NULL, 0) ==
         SDLP_STALE_BOOT);
  assert(!h.configured);
}
static void discovery_replay(void) {
  fixture_t f;
  sdlp_host_t h;
  setup(&f, &h);
  uint8_t original[256], out[256];
  memcpy(original, f.device.replay.request, 256);
  sdlp_cell_t q = {
      .kind = SDLP_REQUEST, .opcode = SDLP_GET_INFO, .sequence = 100};
  uint8_t in[256];
  sdlp_encode(in, &q);
  assert(sdlp_device_receive(&f.device, in, out, f.now));
  assert(sdlp_device_receive(&f.device, original, out, f.now));
  sdlp_cell_t a;
  assert(sdlp_decode(&a, out));
  assert(a.result == 0 && a.opcode == SDLP_CONFIGURE);
  q.opcode = SDLP_BEGIN_FRAME;
  q.boot = 41;
  q.session = 123;
  q.sequence = 2;
  q.length = 8;
  sdlp_put_u32(q.payload, 1);
  sdlp_put_u32(q.payload + 4, 1);
  sdlp_encode(in, &q);
  assert(sdlp_device_receive(&f.device, in, out, f.now));
  assert(sdlp_decode(&a, out));
  assert(a.result == SDLP_STALE_BOOT);
  q.boot = 42;
  q.session = 999;
  sdlp_encode(in, &q);
  assert(sdlp_device_receive(&f.device, in, out, f.now));
  assert(sdlp_decode(&a, out));
  assert(a.result == SDLP_BAD_SESSION);
  f.corrupt = 3;
  assert(!sdlp_host_begin_frame(&h, 1, true));
}
static void backend_errors(void) {
  fixture_t f;
  sdlp_host_t h;
  setup(&f, &h);
  uint8_t value = 50;
  assert(sdlp_host_command(&h, SDLP_SET_BRIGHTNESS, &value, 1, NULL, 0) ==
         SDLP_UNSUPPORTED);
  assert(!sdlp_host_begin_frame(&h, 1, true));
  f.configure_result = SDLP_BUSY;
  assert(sdlp_host_configure(&h, 124, SDLP_MONO1, 1000000) == SDLP_BUSY);
  assert(f.device.session == 123 &&
         f.device.status.state == SDLP_STATE_FRAME_OPEN &&
         f.device.status.active_frame == 1);
  f.configure_result = 0;
  assert(!sdlp_host_configure(&h, 125, SDLP_MONO1, 1000000));
  f.begin_result = SDLP_IO_ERROR;
  assert(sdlp_host_begin_frame(&h, 1, true) == SDLP_IO_ERROR);
  assert(f.device.status.state == SDLP_STATE_FAULT &&
         f.device.status.failed_frame == 1);
  f.begin_result = 0;
  assert(!sdlp_host_configure(&h, 126, SDLP_MONO1, 1000000));
  assert(!sdlp_host_begin_frame(&h, 1, true));
  uint8_t id[4];
  sdlp_put_u32(id, 1);
  assert(!sdlp_host_command(&h, SDLP_ABORT_FRAME, id, 4, NULL, 0));
  assert(sdlp_host_begin_frame(&h, 2, false) == SDLP_BAD_ARGUMENT);
}
int main(void) {
  codec();
  upload_retry();
  reset_timeout();
  discovery_replay();
  backend_errors();
  puts("SDLP codec, upload, replay, reset, and timeout tests passed");
  return 0;
}
