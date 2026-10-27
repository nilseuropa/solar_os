#include "solar_os_display_link_transport.h"
#include "solar_os_stream.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

typedef struct {
  solar_os_stream_handle_t handle;
  char name[SOLAR_OS_STREAM_ID_MAX], owner[SOLAR_OS_STREAM_OWNER_MAX];
  solar_os_display_link_options_t options;
  sdlp_stream_t parser;
  /* Transport callbacks are serialized by the link owner. Keep protocol
   * scratch with that owner rather than nesting it on the display shell stack. */
  uint8_t rx[SDLP_CELL_SIZE], packet[SDLP_CELL_SIZE];
  sdlp_cell_t cell;
  size_t rx_at, rx_size;
  bool compact, failed;
} stream_transport_t;

static uint64_t now_us(void *ctx) { (void)ctx; return esp_timer_get_time(); }
static void sleep_us(void *ctx, uint32_t us) {
  (void)ctx;
  TickType_t ticks = pdMS_TO_TICKS(us / 1000);
  vTaskDelay(ticks ? ticks : 1);
}
static bool owner_cancelled(stream_transport_t *s) {
  return s->options.cancelled && s->options.cancelled(s->options.cancel_user);
}
static bool cancelled(void *ctx) {
  stream_transport_t *s = ctx;
  return s->failed || owner_cancelled(s);
}
static esp_err_t prepare(void *ctx, bool reset) {
  (void)reset;
  stream_transport_t *s = ctx;
  if (solar_os_stream_handle_valid(&s->handle)) {
    esp_err_t error = solar_os_stream_close_ex(&s->handle);
    if (error != ESP_OK) return error;
  }
  memset(&s->parser, 0, sizeof(s->parser));
  s->rx_at = s->rx_size = 0;
  s->failed = false;
  s->compact = true;
  solar_os_stream_open_options_t options = {.direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX};
  return solar_os_stream_open_ex(s->name, s->owner, &options, &s->handle);
}
static void destroy(void *ctx) {
  stream_transport_t *s = ctx;
  if (!s) return;
  if (solar_os_stream_handle_valid(&s->handle)) (void)solar_os_stream_close_ex(&s->handle);
  solar_os_memory_free(s);
}
static int send_packet(stream_transport_t *s, const uint8_t *cell, uint64_t deadline) {
  size_t size = sdlp_stream_encode(s->packet, cell, s->compact), at = 0;
  while (at < size && now_us(s) < deadline && !cancelled(s)) {
    size_t n = 0;
    esp_err_t error = solar_os_stream_write(&s->handle, s->packet + at, size - at, 0, &n);
    if ((error != ESP_OK && error != ESP_ERR_TIMEOUT) || n > size - at) {
      s->failed = true; return SDLP_LINK_ERROR;
    }
    at += n;
    if (!n) sleep_us(s, 1000);
  }
  return at == size ? SDLP_OK : SDLP_LINK_ERROR;
}
static int push(void *ctx, const uint8_t cell[SDLP_CELL_SIZE], uint64_t deadline) {
  return send_packet(ctx, cell, deadline);
}
static int request(void *ctx, const uint8_t tx[SDLP_CELL_SIZE],
    uint8_t rx[SDLP_CELL_SIZE], uint64_t deadline) {
  stream_transport_t *s = ctx;
  if (!sdlp_decode(&s->cell, tx)) return SDLP_LINK_ERROR;
  uint8_t opcode = s->cell.opcode;
  uint32_t sequence = s->cell.sequence, session = s->cell.session;
  uint64_t start = now_us(s), retry_at = 0;
  bool discovery = opcode == SDLP_GET_INFO, fallback = false;
  while (now_us(s) < deadline && !owner_cancelled(s)) {
    if (s->failed) {
      /* A listener may still be fencing its previous peer. Discovery can
       * retry with a fresh stream within the original connection deadline.
       * Configured commands must fail instead of crossing peer sessions. */
      if (!discovery) return SDLP_LINK_ERROR;
      sleep_us(s, 50000);
      if (now_us(s) >= deadline || owner_cancelled(s)) break;
      if (prepare(s, false) != ESP_OK) { s->failed = true; continue; }
      s->compact = !fallback;
      retry_at = 0;
    }
    uint64_t now = now_us(s);
    if (discovery && !fallback && now >= start + (deadline - start) / 2) {
      s->compact = false; fallback = true; retry_at = 0;
    }
    if (now >= retry_at) {
      if (send_packet(s, tx, deadline)) {
        if (discovery && s->failed) continue;
        return SDLP_LINK_ERROR;
      }
      retry_at = now_us(s) + 1000000;
    }
    bool compact = false;
    bool ready = sdlp_stream_next(&s->parser, rx, &compact);
    if (!ready && s->rx_at == s->rx_size) {
      size_t n = 0;
      esp_err_t error = solar_os_stream_read(&s->handle, s->rx, sizeof(s->rx), 0, &n);
      if ((error != ESP_OK && error != ESP_ERR_TIMEOUT) || n > sizeof(s->rx)) {
        s->failed = true;
        if (discovery) continue;
        return SDLP_LINK_ERROR;
      }
      s->rx_at = 0; s->rx_size = n;
      if (!n) { sleep_us(s, 1000); continue; }
    }
    while (!ready && s->rx_at < s->rx_size)
      ready = sdlp_stream_feed(&s->parser, s->rx[s->rx_at++], rx, &compact);
    if (ready) {
      if (sdlp_decode(&s->cell, rx) && s->cell.kind == SDLP_RESPONSE &&
          s->cell.opcode == opcode && s->cell.sequence == sequence &&
          s->cell.session == session) {
        if (discovery) s->compact = compact;
        return SDLP_OK;
      }
    }
  }
  return SDLP_LINK_ERROR;
}

esp_err_t solar_os_display_link_open_stream(const char *name, const char *owner,
    const solar_os_display_link_options_t *options, solar_os_display_link_t **out) {
  if (!out) return ESP_ERR_INVALID_ARG;
  *out = NULL;
  if (!name || !*name || strlen(name) >= SOLAR_OS_STREAM_ID_MAX ||
      !owner || !*owner || strlen(owner) >= SOLAR_OS_STREAM_OWNER_MAX ||
      (options && (options->timeout_ms > 60000 || options->format < -1 || options->format > 4)))
    return ESP_ERR_INVALID_ARG;
  solar_os_stream_info_t info;
  esp_err_t error = solar_os_stream_get_info(name, &info);
  if (error != ESP_OK) return error;
  if (info.type != SOLAR_OS_STREAM_TYPE_BYTES || info.direction != SOLAR_OS_STREAM_DIRECTION_DUPLEX ||
      info.sharing != SOLAR_OS_STREAM_SHARING_EXCLUSIVE) return ESP_ERR_NOT_SUPPORTED;
  stream_transport_t *s = solar_os_memory_calloc(1, sizeof(*s),
      SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "dlp.stream");
  if (!s) return ESP_ERR_NO_MEM;
  s->handle = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
  strlcpy(s->name, name, sizeof(s->name)); strlcpy(s->owner, owner, sizeof(s->owner));
  s->options = options ? *options : (solar_os_display_link_options_t){.format = -1};
  if (!s->options.timeout_ms) s->options.timeout_ms = 5000;
  solar_os_display_link_transport_t io = {
    .protocol = {.ctx = s, .now_us = now_us, .sleep_us = sleep_us, .request = request,
      .push = push, .cancelled = cancelled, .minimum_timeout_ms = s->options.timeout_ms},
    .prepare = prepare, .destroy = destroy};
  return solar_os_display_link_open_transport(&io, &s->options, out);
}
