/* Actual SolarOS receiver, exposed to the Python host interoperability test. */
#include "solar_os_dlp_receiver.h"
#include <assert.h>
#include <string.h>

#define CAPACITY (64 * 32 / 8)
static solar_os_dlp_receiver_t receiver;
static uint8_t storage[2 * CAPACITY], input[8192], output[2048], shown[CAPACITY];
static size_t input_at, input_size, output_size;
static uint64_t now, busy_until;
static unsigned presentations, writes;
static int present_error;
static int read_bytes(void *ctx, uint8_t *data, size_t size) {
  (void)ctx;
  size_t n = input_size - input_at;
  if (n > size) n = size;
  if (n > 7) n = 7;
  memcpy(data, input + input_at, n); input_at += n;
  return (int)n;
}
static int write_bytes(void *ctx, const uint8_t *data, size_t size) {
  (void)ctx;
  if (++writes % 3 == 0) return 0;
  if (size > 3) size = 3;
  assert(output_size + size <= sizeof(output));
  memcpy(output + output_size, data, size); output_size += size;
  return (int)size;
}
static int present(void *ctx, unsigned format, const uint8_t *image, size_t size,
                   uint16_t width, uint16_t height) {
  (void)ctx;
  assert(format == DLP_MONO1 && size == CAPACITY && width == 64 && height == 32);
  if (!busy_until) busy_until = now + 20000;
  if (now < busy_until) return DLP_BUSY;
  if (present_error) return present_error;
  busy_until = 0; memcpy(shown, image, size); presentations++;
  return DLP_OK;
}
void bridge_init(unsigned window) {
  input_at = input_size = output_size = 0;
  now = busy_until = 0; presentations = writes = 0; present_error = 0;
  memset(shown, 0xff, sizeof(shown));
  dlp_info_t info = {.width = 64, .height = 32, .formats = 1,
    .capabilities = DLP_STAGED_FRAME | DLP_PRESERVE_CONTENT | DLP_RLE | DLP_SYNC | DLP_LZ4,
    .max_rect_bytes = CAPACITY, .command_ms = 1000, .present_ms = 100};
  solar_os_dlp_receiver_io_t io = {.read = read_bytes, .write = write_bytes, .present = present};
  assert(solar_os_dlp_receiver_init(&receiver, &info, 42, storage, CAPACITY, &io));
  if (!window) receiver.device.window = NULL;
  else receiver.window.maximum = (uint16_t)window;
}
void bridge_feed(const uint8_t *data, size_t size) {
  if (input_at == input_size) input_at = input_size = 0;
  assert(input_size + size <= sizeof(input));
  memcpy(input + input_size, data, size); input_size += size;
}
void bridge_tick(uint64_t time_us) {
  now = time_us;
  assert(!solar_os_dlp_receiver_pump(&receiver, now));
}
size_t bridge_read(uint8_t *out, size_t size) {
  if (size > output_size) size = output_size;
  memcpy(out, output, size); output_size -= size;
  memmove(output, output + size, output_size);
  return size;
}
unsigned bridge_presentations(void) { return presentations; }
void bridge_image(uint8_t *out) { memcpy(out, shown, CAPACITY); }
void bridge_fault(unsigned kind) {
  if (kind == 1) receiver.device.need_full = true;
  if (kind == 2) present_error = DLP_IO_ERROR;
  if (kind == 3) receiver.device.boot++;
}
