/* SPDX-License-Identifier: MIT */
#include "dlp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 241
#define HEIGHT 19
#define IMAGE_SIZE (WIDTH * HEIGHT * 2)
typedef struct {
  dlp_device_t device;
  dlp_stream_t parser;
  uint8_t image[IMAGE_SIZE], reply[DLP_CELL_SIZE];
  size_t pending, sent, bytes, commands, writes, full, partial, presentations, lz4;
  uint16_t x, y, width;
  uint64_t now;
  unsigned reads, attempts;
  bool drop_present, fail_write, old_peer;
} fixture_t;
static fixture_t f;
static uint64_t now_us(void *ctx) { return ++((fixture_t *)ctx)->now; }
static void sleep_us(void *ctx, uint32_t us) { ((fixture_t *)ctx)->now += us; }
static int configure(void *ctx, unsigned format) {
  (void)format;
  memset(((fixture_t *)ctx)->image, 0, IMAGE_SIZE);
  return DLP_OK;
}
static int begin_frame(void *ctx, uint32_t frame, bool full) {
  fixture_t *p = ctx;
  (void)frame;
  if (full)
    p->full++;
  else
    p->partial++;
  return DLP_OK;
}
static int begin_rect(void *ctx, uint16_t x, uint16_t y, uint16_t w,
                      uint16_t h) {
  fixture_t *p = ctx;
  (void)h;
  p->x = x;
  p->y = y;
  p->width = w;
  return DLP_OK;
}
static int write_pixels(void *ctx, uint32_t offset, const uint8_t *data,
                        size_t n) {
  fixture_t *p = ctx;
  if (p->fail_write)
    return DLP_IO_ERROR;
  static const unsigned bpp[] = {1, 2, 4, 8, 16};
  unsigned bits = bpp[p->device.format];
  uint32_t stride = dlp_row_bytes(WIDTH, p->device.format);
  uint32_t row = dlp_row_bytes(p->width, p->device.format);
  for (size_t k = 0; k < n; k++) {
    uint32_t pos = offset + (uint32_t)k, col = pos % row;
    for (unsigned bit = 0; bit < 8 && col * 8 + bit < p->width * bits; bit++) {
      uint32_t target = p->x * bits + col * 8 + bit;
      uint8_t *out = p->image + (p->y + pos / row) * stride + target / 8;
      unsigned mask = 1u << (target % 8);
      *out = (uint8_t)((*out & ~mask) | (((data[k] >> bit) & 1u) ? mask : 0));
    }
  }
  p->writes++;
  return DLP_OK;
}
static int present(void *ctx) {
  ((fixture_t *)ctx)->presentations++;
  return DLP_OK;
}
static int operation(void *ctx) {
  (void)ctx;
  return DLP_OK;
}
static int write_stream(void *ctx, const uint8_t *data, size_t n) {
  fixture_t *p = ctx;
  if (++p->attempts % 7 == 0)
    return 0; /* Backpressure, including mid-packet. */
  if (n > 7)
    n = 7;
  p->bytes += n;
  for (size_t k = 0; k < n; k++) {
    uint8_t cell[DLP_CELL_SIZE], reply[DLP_CELL_SIZE];
    bool compact;
    if (!dlp_stream_feed(&p->parser, data[k], cell, &compact))
      continue;
    assert(!p->pending);
    p->commands++;
    if (cell[7] == DLP_RECT_LZ4) p->lz4++;
    assert(dlp_device_receive(&p->device, cell, reply, p->now));
    if (p->old_peer && (cell[7] == DLP_GET_EXTENSIONS || cell[7] == DLP_GET_CODECS)) {
      dlp_cell_t c;
      assert(dlp_decode(&c, reply));
      c.result = DLP_UNSUPPORTED;
      c.length = 0;
      assert(dlp_encode(reply, &c));
    }
    if (p->drop_present && cell[7] == DLP_PRESENT) {
      p->drop_present = false;
      continue;
    }
    p->pending = dlp_stream_encode(p->reply, reply, compact);
    p->sent = 0;
  }
  return (int)n;
}
static int read_stream(void *ctx, uint8_t *data, size_t n) {
  fixture_t *p = ctx;
  if (!p->pending || ++p->reads % 11 == 0)
    return 0;
  assert(n);
  data[0] = p->reply[p->sent++];
  if (p->sent == p->pending)
    p->pending = 0;
  return 1;
}
static void setup(unsigned format, dlp_stream_host_t *stream, dlp_host_t *host,
                  bool legacy, bool old_peer) {
  memset(&f, 0, sizeof(f));
  f.old_peer = old_peer;
  dlp_info_t info = {.width = WIDTH,
                     .height = HEIGHT,
                     .formats = DLP_FORMAT_BIT(format),
                     .preferred_format = (uint16_t)format,
                     .capabilities = DLP_PRESERVE_CONTENT | DLP_SYNC | DLP_RLE | DLP_LZ4,
                     /* No SPI clock: a genuine stream-only peripheral. */
                     .max_rect_bytes = dlp_row_bytes(WIDTH, format) * 7,
                     .command_ms = 1000,
                     .present_ms = 1000};
  dlp_backend_t b = {.ctx = &f,
                     .configure = configure,
                     .begin_frame = begin_frame,
                     .begin_rect = begin_rect,
                     .write = write_pixels,
                     .present = present,
                     .poll_present = operation,
                     .abort = operation};
  assert(dlp_device_init(&f.device, &info, 42, &b));
  dlp_stream_io_t io = {.ctx = &f,
                        .now_us = now_us,
                        .sleep_us = sleep_us,
                        .read = read_stream,
                        .write = write_stream};
  assert(dlp_stream_host_init(stream, &io));
  stream->legacy = legacy;
  stream->retry_us = 50000;
  dlp_transport_t t = dlp_stream_transport(stream);
  dlp_host_init(host, &t);
  assert(!dlp_host_discover(host, 3000000));
  assert(!dlp_host_configure(host, 123, format, 0));
  assert(!dlp_host_extensions(host));
  assert((host->info.capabilities & (DLP_RLE | DLP_SYNC)) ==
         (old_peer ? 0 : DLP_RLE | DLP_SYNC));
}
static void uploads(unsigned format, bool legacy, bool old_peer) {
  dlp_stream_host_t stream;
  dlp_host_t host;
  setup(format, &stream, &host, legacy, old_peer);
  uint8_t image[IMAGE_SIZE], retained[IMAGE_SIZE];
  bool valid = false;
  size_t stride = dlp_row_bytes(WIDTH, format), size = stride * HEIGHT;
  for (size_t k = 0; k < size; k++)
    image[k] = (uint8_t)(k * 17 + k / stride);
  static const unsigned bpp[] = {1, 2, 4, 8, 16};
  unsigned used = (WIDTH * bpp[format]) % 8;
  if (used)
    for (unsigned y = 0; y < HEIGHT; y++)
      image[(y + 1) * stride - 1] &= (1u << used) - 1;
  f.drop_present = true; /* Retry must not present twice. */
  size_t before = f.bytes;
  assert(!dlp_host_upload_frame(&host, 1, image, size, retained, &valid));
  size_t full_bytes = f.bytes - before;
  assert(valid && f.presentations == 1 && f.full == 1);
  assert(!memcmp(image, f.image, size) && !memcmp(image, retained, size));
  image[10 * stride + 3] ^= 1;
  before = f.bytes;
  assert(!dlp_host_upload_frame(&host, 2, image, size, retained, &valid));
  assert(valid && f.partial == 1 && f.presentations == 2);
  assert(f.bytes - before < full_bytes && !memcmp(image, f.image, size));
  before = f.presentations;
  assert(!dlp_host_upload_frame(&host, 3, image, size, retained, &valid));
  assert(f.presentations == before + (old_peer ? 1 : 0));
  f.device.need_full = true;
  memset(f.image, 0, size);
  assert(!dlp_host_upload_frame(&host, 4, image, size, retained, &valid));
  assert(!memcmp(image, f.image, size));
  f.device.need_full = true;
  image[3] ^= 2; /* Changed delta must also recover. */
  assert(!dlp_host_upload_frame(&host, 5, image, size, retained, &valid));
  assert(!memcmp(image, f.image, size));
  image[3] ^= 4;
  f.fail_write = true;
  assert(dlp_host_upload_frame(&host, 6, image, size, retained, &valid) ==
         DLP_IO_ERROR);
  assert(!valid && memcmp(image, retained, size));
}
static void codecs(void) {
  uint8_t input[DLP_RLE_MAX_DECODED], encoded[DLP_PAYLOAD_SIZE], decoded[1024];
  uint32_t rng = 1;
  for (unsigned trial = 0; trial < 400; trial++) {
    size_t size = 1 + trial * 23 % sizeof(input);
    for (size_t k = 0; k < size; k++) {
      rng = rng * 1664525u + 1013904223u;
      input[k] = trial % 3 ? (uint8_t)(rng >> 24) : (uint8_t)(k / 130);
    }
    for (size_t offset = 0; offset < size;) {
      size_t used, n = dlp_rle_encode(encoded, 206, input + offset,
                                      size - offset, &used);
      assert(n && n <= 206 && used && used <= 1024);
      assert(dlp_rle_decode(decoded, used, encoded, n));
      assert(!memcmp(decoded, input + offset, used));
      offset += used;
    }
  }
  const uint8_t bad[][3] = {{255, 0, 0}, {127, 0, 0}, {128, 0, 0}};
  for (unsigned k = 0; k < 3; k++)
    assert(!dlp_rle_decode(decoded, 2, bad[k], 3));
  dlp_cell_t q = {.kind = DLP_REQUEST, .opcode = DLP_GET_INFO, .sequence = 1};
  uint8_t cell[256], packet[256], output[256];
  assert(dlp_encode(cell, &q));
  for (unsigned compact = 0; compact < 2; compact++) {
    dlp_stream_t parser = {0};
    bool small;
    size_t size = dlp_stream_encode(packet, cell, compact);
    assert(size == (compact ? 36 : 256));
    /* Corrupt and unrelated traffic followed by a complete packet. */
    packet[size - 1] ^= 1;
    for (size_t k = 0; k < size; k++)
      assert(!dlp_stream_feed(&parser, packet[k], output, &small));
    packet[size - 1] ^= 1;
    for (size_t k = 0; k < size; k++) {
      bool have = dlp_stream_feed(&parser, packet[k], output, &small);
      assert(have == (k + 1 == size));
    }
    assert(small == (bool)compact && !memcmp(output, cell, 256));
    assert(dlp_stream_encode(packet, cell, compact));
  }
}
static int stalled_write(void *ctx, const uint8_t *data, size_t n) {
  (void)ctx;
  (void)data;
  (void)n;
  return 0;
}
static void bounds_and_deadlines(void) {
  dlp_stream_host_t stream;
  dlp_host_t host;
  setup(DLP_MONO1, &stream, &host, false, false);
  assert(!dlp_host_begin_frame(&host, 1, true));
  assert(!dlp_host_begin_rect(&host, 1, 1, 0, 0, WIDTH, 1));
  uint8_t p[16] = {0};
  dlp_put_u32(p, 1);
  dlp_put_u32(p + 4, 1);
  /* 31 bytes of ones have invalid padding for the 241-pixel row. */
  dlp_put_u16(p + 12, 31);
  p[14] = 128 + 31 - 3;
  p[15] = 255;
  assert(dlp_host_command(&host, DLP_RECT_RLE, p, 16, NULL, 0) ==
         DLP_BAD_ARGUMENT);
  assert(!f.writes && !f.device.status.offset);
  dlp_put_u16(p + 12, 32);
  p[14] = 128 + 32 - 3;
  assert(dlp_host_command(&host, DLP_RECT_RLE, p, 16, NULL, 0) ==
         DLP_BAD_ARGUMENT);
  assert(!f.writes && !f.device.status.offset);
  dlp_put_u16(p + 12, 1025);
  assert(dlp_host_command(&host, DLP_RECT_RLE, p, 16, NULL, 0) ==
         DLP_BAD_ARGUMENT);
  assert(!f.writes && !f.device.status.offset);
  dlp_put_u16(p + 12, 31);
  p[14] = 128 + 31 - 3;
  p[15] = 0;
  assert(!dlp_host_command(&host, DLP_RECT_RLE, p, 16, NULL, 0));
  assert(f.writes == 1);
  stream.io.write = stalled_write;
  uint64_t start = f.now;
  assert(dlp_host_command(&host, DLP_GET_STATUS, NULL, 0, NULL, start + 5000) ==
         DLP_LINK_ERROR);
  assert(f.now - start < 6000 && !host.configured);
}
static void compressed_uploads(void) {
  for (unsigned legacy = 0; legacy < 2; legacy++)
    for (unsigned peer = 0; peer < 3; peer++)
      for (unsigned enabled = 0; enabled < 2; enabled++) {
        dlp_stream_host_t stream;
        dlp_host_t host;
        setup(DLP_RGB565_BE, &stream, &host, legacy != 0, peer == 1);
        if (peer == 2) f.device.info.capabilities &= ~(uint32_t)DLP_LZ4;
        host.compressed = enabled != 0;
        assert(!dlp_host_extensions(&host));
        assert(!!(host.info.capabilities & DLP_LZ4) == (enabled && peer == 0));
        uint8_t image[IMAGE_SIZE], retained[IMAGE_SIZE]; bool valid = false;
        for (size_t k = 0; k < sizeof(image); k++) image[k] = k % 2 ? 0 : 248;
        f.drop_present = true;
        assert(!dlp_host_upload_frame(&host, 1, image, sizeof(image), retained, &valid));
        assert(valid && !memcmp(image, f.image, sizeof(image)) && f.presentations == 1);
        assert(!!f.lz4 == (enabled && peer == 0));
        image[100] ^= 1;
        assert(!dlp_host_upload_frame(&host, 2, image, sizeof(image), retained, &valid));
        assert(valid && !memcmp(image, f.image, sizeof(image)) && f.presentations == 2);
      }
}
static void malformed_lz4(void) {
  dlp_stream_host_t stream; dlp_host_t host;
  setup(DLP_RGB565_BE, &stream, &host, false, false);
  assert(!dlp_host_begin_frame(&host, 1, true));
  assert(!dlp_host_begin_rect(&host, 1, 1, 0, 0, WIDTH, 1));
  uint8_t payload[18] = {0};
  dlp_put_u32(payload, 1); dlp_put_u32(payload + 4, 1);
  const uint8_t bad[][4] = {{0, 0, 0, 0}, {0, 1, 0, 0}, {255, 255, 255, 255}};
  for (unsigned k = 0; k < 3; k++) {
    dlp_put_u16(payload + 12, 100); memcpy(payload + 14, bad[k], 4);
    assert(dlp_host_command(&host, DLP_RECT_LZ4, payload, sizeof(payload), NULL, 0) == DLP_BAD_ARGUMENT);
    assert(!f.writes && !f.device.status.offset);
  }
  dlp_put_u16(payload + 12, 1025);
  assert(dlp_host_command(&host, DLP_RECT_LZ4, payload, sizeof(payload), NULL, 0) == DLP_BAD_ARGUMENT);
  assert(!f.writes && !f.device.status.offset);
}
int main(void) {
  compressed_uploads(); malformed_lz4();
  codecs();
  bounds_and_deadlines();
  for (unsigned format = 0; format < 5; format++)
    uploads(format, false, false);
  uploads(DLP_MONO1, true, true);
  puts("DLP compact/legacy streams, partial updates in all formats, RLE, "
       "replay and cache recovery passed");
  return 0;
}
