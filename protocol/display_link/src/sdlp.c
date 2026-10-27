/* SPDX-License-Identifier: MIT */
#include "sdlp.h"
#include <limits.h>
#include <string.h>
uint16_t sdlp_get_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
uint32_t sdlp_get_u32(const uint8_t *p) {
  return (uint32_t)sdlp_get_u16(p) | (uint32_t)sdlp_get_u16(p + 2) << 16;
}
void sdlp_put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
void sdlp_put_u32(uint8_t *p, uint32_t v) {
  sdlp_put_u16(p, (uint16_t)v);
  sdlp_put_u16(p + 2, (uint16_t)(v >> 16));
}
static uint64_t get64(const uint8_t *p) {
  return sdlp_get_u32(p) | (uint64_t)sdlp_get_u32(p + 4) << 32;
}
static void put64(uint8_t *p, uint64_t v) {
  sdlp_put_u32(p, (uint32_t)v);
  sdlp_put_u32(p + 4, (uint32_t)(v >> 32));
}
static bool zero(const uint8_t *p, size_t n) {
  while (n--)
    if (*p++)
      return false;
  return true;
}
uint32_t sdlp_crc32(const void *data, size_t n) {
  const uint8_t *p = data;
  uint32_t crc = UINT32_MAX;
  while (n--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      crc = (crc >> 1) ^ ((0u - (crc & 1u)) & UINT32_C(0xedb88320));
  }
  return ~crc;
}
bool sdlp_encode(uint8_t out[256], const sdlp_cell_t *c) {
  if (!out || !c || c->length > 220 || c->kind > SDLP_PUSH)
    return false;
  memset(out, 0, 256);
  memcpy(out, "SDLP", 4);
  out[4] = 1;
  out[6] = c->kind;
  out[7] = c->opcode;
  sdlp_put_u32(out + 8, c->session);
  sdlp_put_u32(out + 12, c->sequence);
  sdlp_put_u16(out + 16, c->result);
  sdlp_put_u16(out + 18, c->length);
  put64(out + 20, c->boot);
  memcpy(out + 32, c->payload, c->length);
  sdlp_put_u32(out + 252, sdlp_crc32(out, 252));
  return true;
}
bool sdlp_decode(sdlp_cell_t *c, const uint8_t in[256]) {
  if (!c || !in || memcmp(in, "SDLP", 4) || in[4] != 1 || in[5] != 0 ||
      in[6] > SDLP_PUSH || !zero(in + 28, 4) ||
      sdlp_get_u32(in + 252) != sdlp_crc32(in, 252))
    return false;
  uint16_t n = sdlp_get_u16(in + 18);
  if (n > 220 || !zero(in + 32 + n, 220 - n))
    return false;
  if (in[6] == SDLP_IDLE &&
      (in[7] || sdlp_get_u32(in + 12) || sdlp_get_u16(in + 16) || n))
    return false;
  if (in[6] != SDLP_IDLE && !sdlp_get_u32(in + 12))
    return false;
  if ((in[6] == SDLP_REQUEST || in[6] == SDLP_PUSH) && sdlp_get_u16(in + 16))
    return false;
  memset(c, 0, sizeof(*c));
  c->kind = in[6];
  c->opcode = in[7];
  c->session = sdlp_get_u32(in + 8);
  c->sequence = sdlp_get_u32(in + 12);
  c->result = sdlp_get_u16(in + 16);
  c->length = n;
  c->boot = get64(in + 20);
  memcpy(c->payload, in + 32, n);
  return true;
}
uint32_t sdlp_row_bytes(uint16_t w, unsigned f) {
  static const unsigned bits[] = {1, 2, 4, 8, 16};
  return f < 5 ? ((uint32_t)w * bits[f] + 7) / 8 : 0;
}
bool sdlp_info_valid(const sdlp_info_t *i) {
  if (!i || !i->width || !i->height || !i->formats ||
      (i->formats & ~SDLP_FORMATS) || i->preferred_format >= 5 ||
      !(i->formats & SDLP_FORMAT_BIT(i->preferred_format)) ||
      (i->max_clock_hz && i->max_clock_hz < 100000) || i->min_gap_us > 10000 || !i->command_ms ||
      !i->present_ms || (i->capabilities & ~UINT32_C(255)) ||
      !memchr(i->model, 0, 32) || !memchr(i->firmware, 0, 32))
    return false;
  for (unsigned f = 0; f < 5; f++)
    if ((i->formats & SDLP_FORMAT_BIT(f)) &&
        i->max_rect_bytes < sdlp_row_bytes(i->width, f))
      return false;
  return true;
}
void sdlp_info_encode(uint8_t p[128], const sdlp_info_t *i) {
  memset(p, 0, 128);
  sdlp_put_u16(p, 128);
  sdlp_put_u16(p + 4, i->width);
  sdlp_put_u16(p + 6, i->height);
  sdlp_put_u32(p + 8, i->formats);
  sdlp_put_u16(p + 12, i->preferred_format);
  /* Keep GET_INFO compatible with v1 clients that reject unknown bits.
   * New capabilities are queried after CONFIGURE via GET_EXTENSIONS. */
  sdlp_put_u32(p + 16, i->capabilities & UINT32_C(31));
  sdlp_put_u32(p + 20, i->max_clock_hz);
  sdlp_put_u32(p + 24, i->min_gap_us);
  sdlp_put_u32(p + 28, i->max_rect_bytes);
  sdlp_put_u32(p + 32, i->command_ms);
  sdlp_put_u32(p + 36, i->present_ms);
  sdlp_put_u32(p + 40, i->nominal_refresh_ms);
  memcpy(p + 44, i->identity, 16);
  memcpy(p + 60, i->model, 32);
  memcpy(p + 92, i->firmware, 32);
}
bool sdlp_info_decode(sdlp_info_t *i, const uint8_t *p, size_t n) {
  if (!i || !p || n != 128 || sdlp_get_u16(p) != 128 || p[2] || p[3] ||
      !zero(p + 14, 2) || !zero(p + 124, 4))
    return false;
  memset(i, 0, sizeof(*i));
  i->width = sdlp_get_u16(p + 4);
  i->height = sdlp_get_u16(p + 6);
  i->formats = sdlp_get_u32(p + 8);
  i->preferred_format = sdlp_get_u16(p + 12);
  i->capabilities = sdlp_get_u32(p + 16);
  i->max_clock_hz = sdlp_get_u32(p + 20);
  i->min_gap_us = sdlp_get_u32(p + 24);
  i->max_rect_bytes = sdlp_get_u32(p + 28);
  i->command_ms = sdlp_get_u32(p + 32);
  i->present_ms = sdlp_get_u32(p + 36);
  i->nominal_refresh_ms = sdlp_get_u32(p + 40);
  memcpy(i->identity, p + 44, 16);
  memcpy(i->model, p + 60, 32);
  memcpy(i->firmware, p + 92, 32);
  if (!sdlp_info_valid(i))
    return false;
  const char *a = memchr(i->model, 0, 32), *b = memchr(i->firmware, 0, 32);
  return zero((const uint8_t *)a, (size_t)(i->model + 32 - a)) &&
         zero((const uint8_t *)b, (size_t)(i->firmware + 32 - b));
}
static void status_encode(uint8_t *p, const sdlp_status_t *s) {
  p[0] = s->state;
  p[1] = s->brightness;
  sdlp_put_u16(p + 2, s->error);
  sdlp_put_u32(p + 4, s->active_frame);
  sdlp_put_u32(p + 8, s->completed_frame);
  sdlp_put_u32(p + 12, s->failed_frame);
  sdlp_put_u32(p + 16, s->rectangle);
  sdlp_put_u32(p + 20, s->offset);
}
bool sdlp_status_decode(sdlp_status_t *s, const uint8_t *p, size_t n) {
  if (!s || !p || n != 24 || p[0] > SDLP_STATE_FAULT ||
      (p[1] > 100 && p[1] != 255))
    return false;
  s->state = p[0];
  s->brightness = p[1];
  s->error = sdlp_get_u16(p + 2);
  s->active_frame = sdlp_get_u32(p + 4);
  s->completed_frame = sdlp_get_u32(p + 8);
  s->failed_frame = sdlp_get_u32(p + 12);
  s->rectangle = sdlp_get_u32(p + 16);
  s->offset = sdlp_get_u32(p + 20);
  return true;
}
bool sdlp_device_init(sdlp_device_t *d, const sdlp_info_t *i, uint64_t boot,
                      const sdlp_backend_t *b) {
  if (!d || !sdlp_info_valid(i) || !boot || !b || !b->configure ||
      !b->begin_frame || !b->begin_rect || !b->write || !b->present ||
      !b->poll_present || !b->abort ||
      ((i->capabilities & SDLP_BRIGHTNESS) && !b->brightness) ||
      ((i->capabilities & SDLP_POWER_SAVE) && !b->power))
    return false;
  memset(d, 0, sizeof(*d));
  d->info = *i;
  d->boot = boot;
  d->backend = *b;
  d->need_full = true;
  d->status.brightness = (i->capabilities & SDLP_BRIGHTNESS) ? 100 : 255;
  return true;
}
void sdlp_device_idle(const sdlp_device_t *d, uint8_t out[256]) {
  sdlp_cell_t c = {0};
  c.boot = d->boot;
  c.session = d->session;
  (void)sdlp_encode(out, &c);
}
static void fault(sdlp_device_t *d, int error) {
  d->status.failed_frame = d->status.active_frame;
  d->status.active_frame = 0;
  d->status.rectangle = 0;
  d->status.offset = 0;
  d->status.state = SDLP_STATE_FAULT;
  d->status.error = (uint16_t)error;
  d->need_full = true;
}
bool sdlp_device_enable_window(sdlp_device_t *d, sdlp_device_window_t *w,
                               unsigned maximum) {
  if (!d || !w || d->session || !maximum || maximum > SDLP_MAX_WINDOW)
    return false;
  memset(w, 0, sizeof(*w));
  w->maximum = (uint16_t)maximum;
  d->window = w;
  return true;
}
static void window_status(sdlp_device_t *d, sdlp_cell_t *a) {
  sdlp_device_window_t *w = d->window;
  w->grant = w->applied > UINT32_MAX - w->size ? UINT32_MAX : w->applied + w->size;
  sdlp_put_u16(a->payload, SDLP_WINDOW_VERSION);
  sdlp_put_u16(a->payload + 2, w->size);
  sdlp_put_u32(a->payload + 4, w->applied);
  sdlp_put_u32(a->payload + 8, w->grant);
  sdlp_put_u32(a->payload + 12, w->failed);
  sdlp_put_u16(a->payload + 16, w->error);
  sdlp_put_u16(a->payload + 18, d->status.error);
  sdlp_put_u32(a->payload + 20, d->status.completed_frame);
  sdlp_put_u32(a->payload + 24, d->status.failed_frame);
  sdlp_put_u32(a->payload + 28, d->need_full);
  a->length = 32;
}
void sdlp_device_tick(sdlp_device_t *d, uint64_t now) {
  if (d->status.state == SDLP_STATE_PRESENTING) {
    int r = d->backend.poll_present(d->backend.ctx);
    if (r == SDLP_OK) {
      d->status.completed_frame = d->status.active_frame;
      d->status.active_frame = 0;
      d->status.state = SDLP_STATE_IDLE;
      d->need_full = false;
    } else if (r != SDLP_BUSY)
      fault(d, r == SDLP_TIMEOUT ? SDLP_TIMEOUT : SDLP_IO_ERROR);
    else if (now - d->present_started_us >= (uint64_t)d->info.present_ms * 1000)
      fault(d, SDLP_TIMEOUT);
  }
  sdlp_device_window_t *w = d->window;
  if (w && w->waiting && d->status.state != SDLP_STATE_PRESENTING) {
    sdlp_cell_t q, a = {0};
    (void)sdlp_decode(&q, w->pending.request);
    a.kind = SDLP_RESPONSE; a.opcode = q.opcode;
    a.session = q.session; a.sequence = q.sequence; a.boot = d->boot;
    window_status(d, &a);
    (void)sdlp_encode(w->pending.response, &a);
    w->pending.valid = true;
    d->replay = w->pending;
    w->waiting = false;
    w->ready = true;
  }
}
bool sdlp_device_take_reply(sdlp_device_t *d, uint8_t out[256]) {
  if (!d || !out || !d->window || !d->window->ready) return false;
  memcpy(out, d->window->pending.response, 256);
  d->window->ready = false;
  return true;
}
static bool replay(sdlp_replay_t *r, const uint8_t *in, uint8_t *out) {
  if (!r->valid || memcmp(r->request, in, 256))
    return false;
  memcpy(out, r->response, 256);
  return true;
}
#define WINDOW_DEFER (-100)
static int execute(sdlp_device_t *d, const sdlp_cell_t *q, sdlp_cell_t *a,
                   uint64_t now) {
  const uint8_t *p = q->payload;
  sdlp_status_t *s = &d->status;
  void *ctx = d->backend.ctx;
  if (q->opcode == SDLP_GET_INFO) {
    if (q->session || q->boot || q->length)
      return SDLP_BAD_ARGUMENT;
    sdlp_info_encode(a->payload, &d->info);
    a->length = 128;
    return SDLP_OK;
  }
  if (q->opcode == SDLP_CONFIGURE) {
    if (q->length != 4 || sdlp_get_u16(p + 2) || !q->session ||
        q->session == d->session)
      return SDLP_BAD_ARGUMENT;
    unsigned f = sdlp_get_u16(p);
    if (f >= 5 || !(d->info.formats & SDLP_FORMAT_BIT(f)))
      return SDLP_UNSUPPORTED;
    if (s->state == SDLP_STATE_PRESENTING)
      return SDLP_BUSY;
    if (s->state == SDLP_STATE_FAULT &&
        d->backend.poll_present(ctx) == SDLP_BUSY)
      return SDLP_BUSY;
    int r = d->backend.configure(ctx, f);
    if (r) {
      if (d->session && (r == SDLP_IO_ERROR || r == SDLP_TIMEOUT))
        fault(d, r);
      return r;
    }
    d->session = q->session;
    d->format = (uint16_t)f;
    d->need_full = true;
    d->frame_high = 0;
    d->rect_high = 0;
    uint8_t brightness = s->brightness;
    memset(s, 0, sizeof(*s));
    s->brightness = brightness;
    if (d->window) {
      uint16_t maximum = d->window->maximum;
      memset(d->window, 0, sizeof(*d->window));
      d->window->maximum = maximum;
    }
    return SDLP_OK;
  }
  if (q->opcode == SDLP_GET_STATUS) {
    if (q->length)
      return SDLP_BAD_ARGUMENT;
    status_encode(a->payload, s);
    a->length = 24;
    return SDLP_OK;
  }
  if (q->opcode == SDLP_GET_CODECS) {
    if (q->length) return SDLP_BAD_ARGUMENT;
    sdlp_put_u32(a->payload, d->info.capabilities & SDLP_LZ4);
    a->length = 4;
    return SDLP_OK;
  }
  if (q->opcode == SDLP_GET_EXTENSIONS || q->opcode == SDLP_GET_SYNC) {
    if (q->length) return SDLP_BAD_ARGUMENT;
    if (q->opcode == SDLP_GET_SYNC && !(d->info.capabilities & SDLP_SYNC))
      return SDLP_UNSUPPORTED;
    sdlp_put_u32(a->payload, q->opcode == SDLP_GET_EXTENSIONS ?
        d->info.capabilities & (SDLP_RLE | SDLP_SYNC) : (uint32_t)d->need_full);
    a->length = 4;
    return SDLP_OK;
  }
  if (q->opcode == SDLP_NEGOTIATE_WINDOW) {
    sdlp_device_window_t *w = d->window;
    if (!w) return SDLP_UNSUPPORTED;
    if (q->length != 4 || sdlp_get_u16(p) != SDLP_WINDOW_VERSION ||
        !sdlp_get_u16(p + 2) || sdlp_get_u16(p + 2) > SDLP_MAX_WINDOW)
      return SDLP_BAD_ARGUMENT;
    if (w->active || s->state != SDLP_STATE_IDLE) return SDLP_BAD_STATE;
    w->size = sdlp_get_u16(p + 2);
    if (w->size > w->maximum) w->size = w->maximum;
    w->grant = w->size;
    w->active = true;
    sdlp_put_u16(a->payload, SDLP_WINDOW_VERSION);
    sdlp_put_u16(a->payload + 2, w->size);
    a->length = 4;
    return SDLP_OK;
  }
  if (q->opcode == SDLP_WINDOW_SYNC) {
    sdlp_device_window_t *w = d->window;
    if (!w || !w->active) return SDLP_UNSUPPORTED;
    if (q->length != 8 || sdlp_get_u32(p) > w->grant ||
        sdlp_get_u32(p) < w->applied) return SDLP_BAD_ARGUMENT;
    uint32_t frame = sdlp_get_u32(p + 4);
    if (!w->error && w->applied == sdlp_get_u32(p) && frame) {
      if (frame == s->active_frame && s->state == SDLP_STATE_PRESENTING)
        return WINDOW_DEFER;
      if (frame != s->completed_frame && frame != s->failed_frame)
        return SDLP_BAD_ARGUMENT;
    }
    window_status(d, a);
    return SDLP_OK;
  }
  if (d->window && d->window->active && d->window->error)
    return SDLP_BAD_STATE;
  if (q->opcode != SDLP_BEGIN_FRAME && q->opcode != SDLP_BEGIN_RECT &&
      q->opcode != SDLP_RECT_DATA && q->opcode != SDLP_RECT_RLE &&
      q->opcode != SDLP_RECT_LZ4 && q->opcode != SDLP_PRESENT &&
      q->opcode != SDLP_ABORT_FRAME && q->opcode != SDLP_SET_BRIGHTNESS &&
      q->opcode != SDLP_SET_POWER)
    return SDLP_UNSUPPORTED;
  if (s->state == SDLP_STATE_FAULT)
    return SDLP_BAD_STATE;
  if (s->state == SDLP_STATE_PRESENTING)
    return SDLP_BUSY;
  if (q->opcode == SDLP_SET_POWER || q->opcode == SDLP_SET_BRIGHTNESS) {
    bool power = q->opcode == SDLP_SET_POWER;
    uint32_t cap = power ? SDLP_POWER_SAVE : SDLP_BRIGHTNESS;
    if (!(d->info.capabilities & cap))
      return SDLP_UNSUPPORTED;
    if (q->length != 1 || p[0] > (power ? 1 : 100))
      return SDLP_BAD_ARGUMENT;
    if (s->state == SDLP_STATE_FRAME_OPEN ||
        (!power && s->state == SDLP_STATE_POWER_SAVE))
      return SDLP_BAD_STATE;
    int r = SDLP_OK;
    if (power) {
      if ((s->state == SDLP_STATE_POWER_SAVE) != (p[0] != 0))
        r = d->backend.power(ctx, p[0] != 0);
      if (!r) {
        if (s->state == SDLP_STATE_POWER_SAVE && !p[0])
          d->need_full = true;
        s->state = p[0] ? SDLP_STATE_POWER_SAVE : SDLP_STATE_IDLE;
      }
    } else {
      r = d->backend.brightness(ctx, p[0]);
      if (!r)
        s->brightness = p[0];
    }
    if (r == SDLP_IO_ERROR || r == SDLP_TIMEOUT)
      fault(d, r);
    return r;
  }
  if (s->state == SDLP_STATE_POWER_SAVE)
    return SDLP_BAD_STATE;
  if (q->opcode == SDLP_BEGIN_FRAME) {
    if (q->length != 8)
      return SDLP_BAD_ARGUMENT;
    uint32_t f = sdlp_get_u32(p), flags = sdlp_get_u32(p + 4);
    if (!f || f <= d->frame_high || flags > 1 ||
        (!(flags & 1) &&
         (d->need_full || !(d->info.capabilities & SDLP_PRESERVE_CONTENT))))
      return SDLP_BAD_ARGUMENT;
    if (s->state != SDLP_STATE_IDLE)
      return SDLP_BAD_STATE;
    int r = d->backend.begin_frame(ctx, f, (flags & 1) != 0);
    if (r) {
      if (r == SDLP_IO_ERROR || r == SDLP_TIMEOUT) {
        s->active_frame = f;
        fault(d, r);
      }
      return r;
    }
    s->state = SDLP_STATE_FRAME_OPEN;
    s->active_frame = f;
    d->frame_high = f;
    d->frame_flags = flags;
    d->rect_high = 0;
    d->next_y = 0;
    d->have_rect = false;
    return SDLP_OK;
  }
  if (s->state != SDLP_STATE_FRAME_OPEN)
    return SDLP_BAD_STATE;
  if (q->length < 4 || sdlp_get_u32(p) != s->active_frame)
    return SDLP_BAD_ARGUMENT;
  int r = SDLP_OK;
  if (q->opcode == SDLP_BEGIN_RECT) {
    if (q->length != 16)
      return SDLP_BAD_ARGUMENT;
    if (s->rectangle)
      return SDLP_BAD_STATE;
    uint32_t id = sdlp_get_u32(p + 4);
    uint16_t x = sdlp_get_u16(p + 8), y = sdlp_get_u16(p + 10),
             w = sdlp_get_u16(p + 12), h = sdlp_get_u16(p + 14);
    uint32_t row = sdlp_row_bytes(w, d->format);
    uint64_t total = (uint64_t)row * h;
    if (!id || id <= d->rect_high || !w || !h ||
        (uint32_t)x + w > d->info.width || (uint32_t)y + h > d->info.height ||
        total > d->info.max_rect_bytes || total > UINT32_MAX ||
        ((d->frame_flags & 1) && (x || w != d->info.width || y != d->next_y)))
      return SDLP_BAD_ARGUMENT;
    r = d->backend.begin_rect(ctx, x, y, w, h);
    if (!r) {
      s->rectangle = id;
      s->offset = 0;
      d->rect_high = id;
      d->row_bytes = row;
      d->rect_total = (uint32_t)total;
      d->rect_height = h;
      /* Store the unused-bit mask for validating the last byte of each row. */
      static const unsigned bits[] = {1, 2, 4, 8, 16};
      d->row_used_bits = (uint16_t)((w * bits[d->format]) % 8);
    }
  } else if (q->opcode == SDLP_RECT_DATA || q->opcode == SDLP_RECT_RLE ||
             q->opcode == SDLP_RECT_LZ4) {
    if (q->length < 13 || !s->rectangle ||
        sdlp_get_u32(p + 4) != s->rectangle || sdlp_get_u32(p + 8) != s->offset)
      return SDLP_BAD_ARGUMENT;
    uint32_t n = q->length - 12;
    const uint8_t *bytes = p + 12;
    if (q->opcode != SDLP_RECT_DATA) {
      bool lz4 = q->opcode == SDLP_RECT_LZ4;
      if (!(d->info.capabilities & (lz4 ? SDLP_LZ4 : SDLP_RLE))) return SDLP_UNSUPPORTED;
      if (q->length < 16) return SDLP_BAD_ARGUMENT;
      n = sdlp_get_u16(p + 12);
      if (!n || n > sizeof(d->decoded) ||
          !(lz4 ? sdlp_lz4_decode(d->decoded, n, p + 14, q->length - 14) :
                   sdlp_rle_decode(d->decoded, n, p + 14, q->length - 14)))
        return SDLP_BAD_ARGUMENT;
      bytes = d->decoded;
    }
    if (n > d->rect_total - s->offset)
      return SDLP_BAD_ARGUMENT;
    if (d->row_used_bits)
      for (uint32_t k = 0; k < n; k++)
        if ((s->offset + k + 1) % d->row_bytes == 0 &&
            (bytes[k] & (uint8_t)(0xffu << d->row_used_bits)))
          return SDLP_BAD_ARGUMENT;
    r = d->backend.write(ctx, s->offset, bytes, n);
    if (!r) {
      s->offset += n;
      if (s->offset == d->rect_total) {
        s->rectangle = 0;
        s->offset = 0;
        d->have_rect = true;
        if (d->frame_flags & 1)
          d->next_y = (uint16_t)(d->next_y + d->rect_height);
      }
    }
  } else if (q->opcode == SDLP_PRESENT) {
    if (q->length != 4)
      return SDLP_BAD_ARGUMENT;
    if (s->rectangle || !d->have_rect ||
        ((d->frame_flags & 1) && d->next_y != d->info.height))
      return SDLP_BAD_STATE;
    r = d->backend.present(ctx);
    if (!r) {
      s->state = SDLP_STATE_PRESENTING;
      d->present_started_us = now;
      sdlp_device_tick(d, now);
    }
  } else if (q->opcode == SDLP_ABORT_FRAME) {
    if (q->length != 4)
      return SDLP_BAD_ARGUMENT;
    r = d->backend.abort(ctx);
    if (!r) {
      s->state = SDLP_STATE_IDLE;
      s->active_frame = 0;
      s->rectangle = 0;
      s->offset = 0;
      d->need_full = true;
    }
  }
  if (r == SDLP_IO_ERROR || r == SDLP_TIMEOUT)
    fault(d, r);
  return r;
}
bool sdlp_device_receive(sdlp_device_t *d, const uint8_t in[256],
                         uint8_t out[256], uint64_t now) {
  sdlp_cell_t q, a = {0};
  if (!d || !in || !out || !sdlp_decode(&q, in) ||
      (q.kind != SDLP_REQUEST && q.kind != SDLP_PUSH))
    return false;
  sdlp_device_tick(d, now);
  sdlp_device_window_t *w = d->window;
  if (q.kind == SDLP_PUSH) {
    if (!w || !w->active || q.boot != d->boot || q.session != d->session || w->error)
      return false;
    int error = SDLP_OK;
    uint32_t crc = sdlp_get_u32(in + 252);
    if (q.sequence <= w->applied) {
      unsigned slot = (q.sequence - 1) % w->size;
      if (w->seen[slot].sequence != q.sequence || w->seen[slot].crc == crc)
        return false;
      error = SDLP_BAD_SEQUENCE;
    } else if (q.sequence > w->grant) error = SDLP_BAD_SEQUENCE;
    else if (q.sequence != w->applied + 1) return false;
    else if (q.opcode != SDLP_BEGIN_FRAME && q.opcode != SDLP_BEGIN_RECT &&
             q.opcode != SDLP_RECT_DATA && q.opcode != SDLP_RECT_RLE &&
             q.opcode != SDLP_RECT_LZ4 &&
             q.opcode != SDLP_PRESENT) error = SDLP_UNSUPPORTED;
    else error = execute(d, &q, &a, now);
    if (error) {
      w->failed = q.sequence; w->error = (uint16_t)error;
      fault(d, error);
    } else {
      unsigned slot = (q.sequence - 1) % w->size;
      w->seen[slot].sequence = q.sequence; w->seen[slot].crc = crc;
      w->applied = q.sequence;
    }
    return false;
  }
  if (replay(&d->replay, in, out)) {
    if (w && w->ready && !memcmp(w->pending.request, in, 256)) w->ready = false;
    return true;
  }
  if (replay(&d->auxiliary, in, out)) return true;
  if (w && w->waiting && !memcmp(w->pending.request, in, 256)) return false;
  a.kind = SDLP_RESPONSE;
  a.opcode = q.opcode;
  a.session = q.session;
  a.sequence = q.sequence;
  a.boot = d->boot;
  bool auxiliary = q.opcode == SDLP_GET_INFO || q.opcode == SDLP_CONFIGURE;
  bool cache = true;
  if (q.opcode != SDLP_GET_INFO && q.boot != d->boot) {
    a.result = SDLP_STALE_BOOT;
    cache = false;
  } else if (!auxiliary && (!d->session || q.session != d->session)) {
    a.result = SDLP_BAD_SESSION;
    cache = false;
  } else if (q.opcode != SDLP_GET_INFO && q.session == d->session &&
             q.sequence <= d->sequence) {
    a.result = SDLP_BAD_SEQUENCE;
    cache = false;
  } else if (auxiliary && d->auxiliary.valid &&
             q.session == sdlp_get_u32(d->auxiliary.request + 8) &&
             q.sequence == sdlp_get_u32(d->auxiliary.request + 12)) {
    a.result = SDLP_BAD_SEQUENCE;
    cache = false;
  } else if (w && w->waiting) {
    a.result = SDLP_BUSY;
    cache = false;
  } else {
    int r = execute(d, &q, &a, now);
    if (r == WINDOW_DEFER) {
      memcpy(w->pending.request, in, 256);
      w->pending.valid = false; w->waiting = true; w->ready = false;
      d->sequence = q.sequence;
      return false;
    }
    a.result = (uint16_t)r;
  }
  (void)sdlp_encode(out, &a);
  if (cache) {
    sdlp_replay_t *r = &d->replay;
    if (auxiliary && !(q.opcode == SDLP_CONFIGURE && a.result == SDLP_OK))
      r = &d->auxiliary;
    else
      d->sequence = q.sequence;
    memcpy(r->request, in, 256);
    memcpy(r->response, out, 256);
    r->valid = true;
  }
  return true;
}

void sdlp_host_init(sdlp_host_t *h, const sdlp_transport_t *t) {
  memset(h, 0, sizeof(*h));
  h->transport = *t;
  h->clock_hz = 100000;
  h->gap_us = 10000;
  h->command_limit_ms = 60000;
}
static uint32_t host_timeout_ms(const sdlp_host_t *h, uint32_t ms) {
  if (!ms || ms > h->command_limit_ms) ms = h->command_limit_ms;
  if (h->transport.minimum_timeout_ms > ms)
    ms = h->transport.minimum_timeout_ms < h->command_limit_ms ?
        h->transport.minimum_timeout_ms : h->command_limit_ms;
  return ms;
}
/* SPI needs idle polls between command retries. Byte streams never use these
 * cells, so do not reserve them in every command's stack frame. Keep this
 * boundary even with size optimization, which otherwise inlines the scratch. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int host_exchange(sdlp_host_t *h, const uint8_t request[256],
                         uint8_t rx[256], unsigned count, uint64_t deadline) {
  if (count % 4 == 0)
    return h->transport.exchange(h->transport.ctx, request, rx, h->clock_hz, h->gap_us, deadline);
  sdlp_cell_t blank = {0};
  uint8_t idle[256];
  (void)sdlp_encode(idle, &blank);
  return h->transport.exchange(h->transport.ctx, idle, rx, h->clock_hz, h->gap_us, deadline);
}
int sdlp_host_command(sdlp_host_t *h, unsigned op, const void *p, size_t n,
                      sdlp_cell_t *answer, uint64_t deadline) {
  if (!h || (!h->transport.exchange && !h->transport.request) || !h->transport.now_us ||
      !h->transport.sleep_us || n > 220 || (n && !p) || op > 255)
    return SDLP_BAD_ARGUMENT;
  bool discovery = op == SDLP_GET_INFO;
  if (!discovery && op != SDLP_CONFIGURE && !h->configured)
    return SDLP_BAD_SESSION;
  uint32_t *seq = discovery ? &h->discovery_sequence : &h->sequence;
  if (*seq == UINT32_MAX)
    return SDLP_BAD_SEQUENCE;
  sdlp_cell_t cell = {0};
  uint32_t sequence = ++*seq, session = discovery ? 0 : h->session;
  cell.kind = SDLP_REQUEST;
  cell.opcode = (uint8_t)op;
  cell.sequence = sequence;
  cell.session = session;
  cell.boot = discovery ? 0 : h->boot;
  cell.length = (uint16_t)n;
  if (n)
    memcpy(cell.payload, p, n);
  uint8_t request[256], rx[256];
  (void)sdlp_encode(request, &cell);
  uint64_t now = h->transport.now_us(h->transport.ctx);
  uint32_t ms = discovery ? 1000 : h->info.command_ms;
  if (op == SDLP_WINDOW_SYNC && n == 8 && sdlp_get_u32((const uint8_t *)p + 4))
    ms = h->info.present_ms;
  ms = host_timeout_ms(h, ms);
  uint64_t own = now + (uint64_t)ms * 1000 + 100000;
  if (!h->transport.request)
    own += 2 * (UINT64_C(2048000000) / h->clock_hz + h->gap_us);
  if (!deadline || deadline > own)
    deadline = own;
  for (unsigned count = 0; h->transport.now_us(h->transport.ctx) < deadline;
       count++) {
    if (h->transport.cancelled && h->transport.cancelled(h->transport.ctx)) {
      h->configured = false;
      return SDLP_LINK_ERROR;
    }
    int exchanged = h->transport.request ?
        h->transport.request(h->transport.ctx, request, rx, deadline) :
        host_exchange(h, request, rx, count, deadline);
    if (exchanged) {
      if (!discovery)
        h->configured = false;
      return SDLP_LINK_ERROR;
    }
    if (!sdlp_decode(&cell, rx) ||
        (cell.kind != SDLP_IDLE && cell.kind != SDLP_RESPONSE) || !cell.boot)
      continue;
    if (!discovery && cell.boot != h->boot) {
      h->configured = false;
      return SDLP_STALE_BOOT;
    }
    if (cell.kind != SDLP_RESPONSE || cell.opcode != op || cell.sequence != sequence ||
        cell.session != session)
      continue;
    if (discovery && cell.result == SDLP_OK)
      h->boot = cell.boot;
    if (answer)
      *answer = cell;
    return cell.result;
  }
  if (!discovery)
    h->configured = false;
  return SDLP_DEADLINE;
}
int sdlp_host_discover(sdlp_host_t *h, uint64_t deadline) {
  h->configured = false;
  if (h->window) memset(h->window, 0, sizeof(*h->window));
  h->clock_hz = 100000;
  h->gap_us = 10000;
  sdlp_cell_t a;
  int r;
  do {
    if (h->transport.cancelled && h->transport.cancelled(h->transport.ctx))
      return SDLP_LINK_ERROR;
    r = sdlp_host_command(h, SDLP_GET_INFO, NULL, 0, &a, deadline);
    if (!r) {
      sdlp_info_t info;
      if (!sdlp_info_decode(&info, a.payload, a.length))
        return SDLP_INVALID_CELL;
      h->info = info;
      if (!h->transport.request && !info.max_clock_hz) return SDLP_UNSUPPORTED;
      return SDLP_OK;
    }
    if (r != SDLP_DEADLINE && r != SDLP_LINK_ERROR)
      return r;
    h->transport.sleep_us(h->transport.ctx, 10000);
  } while (h->transport.now_us(h->transport.ctx) < deadline);
  return r;
}
int sdlp_host_configure(sdlp_host_t *h, uint32_t session, unsigned f,
                        uint32_t max_hz) {
  if (!session || !h->boot || f >= 5 ||
      !(h->info.formats & SDLP_FORMAT_BIT(f)) ||
      (!h->transport.request && max_hz < 100000) ||
      h->info.command_ms > h->command_limit_ms)
    return SDLP_BAD_ARGUMENT;
  h->session = session;
  h->sequence = 0;
  h->configured = false;
  if (h->window) memset(h->window, 0, sizeof(*h->window));
  h->clock_hz = 100000;
  h->gap_us = 10000;
  uint8_t p[4] = {0};
  sdlp_put_u16(p, (uint16_t)f);
  int r = sdlp_host_command(h, SDLP_CONFIGURE, p, 4, NULL, 0);
  if (!r) {
    h->configured = true;
    h->format = f;
    h->clock_hz = h->transport.request ? 100000 :
        (max_hz < h->info.max_clock_hz ? max_hz : h->info.max_clock_hz);
    h->gap_us = h->info.min_gap_us;
  }
  return r;
}
int sdlp_host_extensions(sdlp_host_t *h) {
  if (!h || !h->configured) return SDLP_BAD_SESSION;
  h->info.capabilities &= UINT32_C(31);
  sdlp_cell_t extensions;
  int r = sdlp_host_command(h, SDLP_GET_EXTENSIONS, NULL, 0, &extensions, 0);
  if (r != SDLP_UNSUPPORTED) {
    if (r) return r;
    if (extensions.length != 4 ||
        (sdlp_get_u32(extensions.payload) & ~(uint32_t)(SDLP_RLE | SDLP_SYNC)))
      return SDLP_INVALID_CELL;
    h->info.capabilities |= sdlp_get_u32(extensions.payload);
  }
  if (h->compressed) {
    r = sdlp_host_command(h, SDLP_GET_CODECS, NULL, 0, &extensions, 0);
    if (r == SDLP_UNSUPPORTED) return SDLP_OK;
    if (r) return r;
    if (extensions.length != 4 || (sdlp_get_u32(extensions.payload) & ~(uint32_t)SDLP_LZ4))
      return SDLP_INVALID_CELL;
    h->info.capabilities |= sdlp_get_u32(extensions.payload);
  }
  return SDLP_OK;
}
int sdlp_host_negotiate_window(sdlp_host_t *h, sdlp_host_window_t *w, unsigned size) {
  if (!h || !w || !size || size > SDLP_MAX_WINDOW ||
      (!h->transport.push && !h->transport.exchange)) return SDLP_BAD_ARGUMENT;
  if (!h->configured) return SDLP_BAD_SESSION;
  if (h->window && h->window->active) return SDLP_BAD_STATE;
  memset(w, 0, sizeof(*w));
  h->window = w;
  uint8_t p[4];
  sdlp_put_u16(p, SDLP_WINDOW_VERSION); sdlp_put_u16(p + 2, (uint16_t)size);
  sdlp_cell_t a;
  int r = sdlp_host_command(h, SDLP_NEGOTIATE_WINDOW, p, sizeof(p), &a, 0);
  if (r == SDLP_UNSUPPORTED) return SDLP_OK;
  if (r) return r;
  if (a.length != 4 || sdlp_get_u16(a.payload) != SDLP_WINDOW_VERSION ||
      !sdlp_get_u16(a.payload + 2) || sdlp_get_u16(a.payload + 2) > size) {
    h->configured = false;
    return SDLP_INVALID_CELL;
  }
  w->size = sdlp_get_u16(a.payload + 2); w->grant = w->size; w->active = true;
  return SDLP_OK;
}
static int push_packet(sdlp_host_t *h, const uint8_t packet[256], uint64_t deadline) {
  int r;
  if (h->transport.push) r = h->transport.push(h->transport.ctx, packet, deadline);
  else {
    uint8_t rx[256];
    r = h->transport.exchange(h->transport.ctx, packet, rx, h->clock_hz, h->gap_us, deadline);
    sdlp_cell_t a;
    if (!r && sdlp_decode(&a, rx) && a.boot && a.boot != h->boot) {
      h->configured = false;
      return SDLP_STALE_BOOT;
    }
  }
  if (r) h->configured = false;
  return r ? SDLP_LINK_ERROR : SDLP_OK;
}
static int window_flush(sdlp_host_t *h, uint32_t frame) {
  sdlp_host_window_t *w = h->window;
  uint32_t ms = frame ? h->info.present_ms : h->info.command_ms;
  ms = host_timeout_ms(h, ms);
  uint64_t deadline = h->transport.now_us(h->transport.ctx) + (uint64_t)ms * 1000 + 100000;
  if (!h->transport.request)
    deadline += (w->size + 2) * (UINT64_C(2048000000) / h->clock_hz + h->gap_us);
  int r = SDLP_DEADLINE;
  while (h->transport.now_us(h->transport.ctx) < deadline) {
    uint8_t p[8];
    sdlp_put_u32(p, w->sent); sdlp_put_u32(p + 4, frame);
    sdlp_cell_t a;
    r = sdlp_host_command(h, SDLP_WINDOW_SYNC, p, sizeof(p), &a, deadline);
    if (r) break;
    uint32_t ack = sdlp_get_u32(a.payload + 4), grant = sdlp_get_u32(a.payload + 8);
    uint32_t expected = ack > UINT32_MAX - w->size ? UINT32_MAX : ack + w->size;
    if (a.length != 32 || sdlp_get_u16(a.payload) != SDLP_WINDOW_VERSION ||
        sdlp_get_u16(a.payload + 2) != w->size || ack < w->acknowledged ||
        ack > w->sent || grant != expected || sdlp_get_u32(a.payload + 28) > 1 ||
        (!!sdlp_get_u32(a.payload + 12) != !!sdlp_get_u16(a.payload + 16)) ||
        sdlp_get_u32(a.payload + 12) > w->sent) {
      r = SDLP_INVALID_CELL; break;
    }
    if (sdlp_get_u16(a.payload + 16)) {
      r = sdlp_get_u16(a.payload + 16); break;
    }
    if (frame && (sdlp_get_u32(a.payload + 24) == frame || sdlp_get_u16(a.payload + 18))) {
      r = sdlp_get_u16(a.payload + 18);
      if (!r) r = SDLP_IO_ERROR;
      break;
    }
    unsigned removed = ack - w->acknowledged;
    if (removed > w->count) { r = SDLP_INVALID_CELL; break; }
    w->count = (uint16_t)(w->count - removed);
    memmove(w->packets, w->packets + removed, w->count * SDLP_CELL_SIZE);
    w->acknowledged = ack; w->grant = grant;
    w->completed = sdlp_get_u32(a.payload + 20);
    if (!w->count) {
      if (!frame || (w->completed == frame && !sdlp_get_u32(a.payload + 28)))
        return SDLP_OK;
      r = SDLP_INVALID_CELL; break;
    }
    /* Gap: resend the retained suffix with original sequence and bytes. The
     * next control request has a fresh sequence and observes a fresh prefix. */
    for (unsigned i = 0; i < w->count; i++) {
      r = push_packet(h, w->packets[i], deadline);
      if (r) break;
    }
    if (r) break;
    r = SDLP_DEADLINE;
  }
  h->configured = false;
  return r;
}
static int upload_command(sdlp_host_t *h, unsigned op, const void *p, size_t n) {
  if (!h) return SDLP_BAD_ARGUMENT;
  sdlp_host_window_t *w = h->window;
  if (!w || !w->active || !w->collecting)
    return sdlp_host_command(h, op, p, n, NULL, 0);
  if (!h->configured) return SDLP_BAD_SESSION;
  if (w->count == w->size) {
    int r = window_flush(h, 0);
    if (r) return r;
  }
  if (w->sent == UINT32_MAX || w->sent >= w->grant) {
    h->configured = false;
    return SDLP_BAD_SEQUENCE;
  }
  sdlp_cell_t q = {0};
  q.kind = SDLP_PUSH; q.opcode = (uint8_t)op;
  q.session = h->session; q.boot = h->boot; q.sequence = ++w->sent;
  q.length = (uint16_t)n; memcpy(q.payload, p, n);
  (void)sdlp_encode(w->packets[w->count], &q);
  uint64_t deadline = h->transport.now_us(h->transport.ctx) +
      (uint64_t)host_timeout_ms(h, h->info.command_ms) * 1000 + 100000;
  if (!h->transport.request)
    deadline += UINT64_C(2048000000) / h->clock_hz + h->gap_us;
  int r = push_packet(h, w->packets[w->count++], deadline);
  if (!r && op == SDLP_PRESENT) r = window_flush(h, sdlp_get_u32(p));
  return r;
}
int sdlp_host_begin_frame(sdlp_host_t *h, uint32_t frame, bool full) {
  uint8_t p[8];
  sdlp_put_u32(p, frame);
  sdlp_put_u32(p + 4, full ? 1 : 0);
  return upload_command(h, SDLP_BEGIN_FRAME, p, 8);
}
int sdlp_host_begin_rect(sdlp_host_t *h, uint32_t frame, uint32_t rect,
                         uint16_t x, uint16_t y, uint16_t w, uint16_t height) {
  uint8_t p[16];
  sdlp_put_u32(p, frame);
  sdlp_put_u32(p + 4, rect);
  sdlp_put_u16(p + 8, x);
  sdlp_put_u16(p + 10, y);
  sdlp_put_u16(p + 12, w);
  sdlp_put_u16(p + 14, height);
  return upload_command(h, SDLP_BEGIN_RECT, p, 16);
}
int sdlp_host_write(sdlp_host_t *h, uint32_t frame, uint32_t rect,
                    uint32_t offset, const uint8_t *data, size_t size) {
  if (!data || !size || size > 208)
    return SDLP_BAD_ARGUMENT;
  uint8_t p[220];
  sdlp_put_u32(p, frame);
  sdlp_put_u32(p + 4, rect);
  sdlp_put_u32(p + 8, offset);
  memcpy(p + 12, data, size);
  return upload_command(h, SDLP_RECT_DATA, p, size + 12);
}
int sdlp_host_present(sdlp_host_t *h, uint32_t frame) {
  uint8_t p[4];
  sdlp_put_u32(p, frame);
  return upload_command(h, SDLP_PRESENT, p, 4);
}
int sdlp_host_wait_frame(sdlp_host_t *h, uint32_t frame, uint64_t deadline) {
  if (h->configured && h->window && h->window->active && h->window->completed == frame)
    return SDLP_OK;
  while (h->transport.now_us(h->transport.ctx) < deadline) {
    sdlp_cell_t a;
    sdlp_status_t s;
    int r = sdlp_host_command(h, SDLP_GET_STATUS, NULL, 0, &a, deadline);
    if (r)
      return r;
    if (!sdlp_status_decode(&s, a.payload, a.length))
      return SDLP_INVALID_CELL;
    if (s.failed_frame == frame || s.state == SDLP_STATE_FAULT)
      return s.error ? s.error : SDLP_IO_ERROR;
    if (s.completed_frame == frame)
      return SDLP_OK;
    h->transport.sleep_us(h->transport.ctx, 1000);
  }
  return SDLP_DEADLINE;
}

#include "lz4.inc"

size_t sdlp_rle_encode(uint8_t *out, size_t cap, const uint8_t *in, size_t size,
                       size_t *consumed) {
  size_t at = 0, used = 0;
  if (!consumed)
    return 0;
  *consumed = 0;
  if (!out || !in)
    return 0;
  while (at < size && used + 2 <= cap) {
    size_t run = 1;
    while (at + run < size && run < 130 && in[at + run] == in[at])
      run++;
    if (run >= 3) {
      out[used++] = (uint8_t)(128 + run - 3);
      out[used++] = in[at];
      at += run;
    } else {
      size_t start = at;
      do {
        at++;
        if (at == size || at - start == 128 || at - start == cap - used - 1)
          break;
      } while (
          !(at + 2 < size && in[at] == in[at + 1] && in[at] == in[at + 2]));
      size_t n = at - start;
      out[used++] = (uint8_t)(n - 1);
      memcpy(out + used, in + start, n);
      used += n;
    }
  }
  *consumed = at;
  return used;
}
bool sdlp_rle_decode(uint8_t *out, size_t expected, const uint8_t *in,
                     size_t size) {
  if (!out || !in || !expected || expected > SDLP_RLE_MAX_DECODED)
    return false;
  size_t at = 0, used = 0;
  while (at < size) {
    unsigned tag = in[at++];
    size_t n = tag < 128 ? tag + 1 : (tag & 127) + 3;
    if (n > expected - used)
      return false;
    if (tag < 128) {
      if (n > size - at)
        return false;
      memcpy(out + used, in + at, n);
      at += n;
    } else {
      if (at == size)
        return false;
      memset(out + used, in[at++], n);
    }
    used += n;
  }
  return used == expected;
}
bool sdlp_delta_init(sdlp_delta_t *d, const uint8_t *before,
                     const uint8_t *after, uint16_t w, uint16_t h,
                     unsigned format) {
  if (!d || !before || !after || !w || !h || format >= 5)
    return false;
  static const unsigned bits[] = {1, 2, 4, 8, 16};
  *d = (sdlp_delta_t){.before = before,
                      .after = after,
                      .width = w,
                      .height = h,
                      .stride = sdlp_row_bytes(w, format),
                      .bits = bits[format]};
  return true;
}
bool sdlp_delta_next(sdlp_delta_t *d, sdlp_rect_t *r) {
  if (!d || !r)
    return false;
  while (d->next_y < d->height) {
    unsigned y = d->next_y, end = y + 8;
    if (end > d->height)
      end = d->height;
    uint32_t left = d->stride, right = 0;
    for (unsigned row = y; row < end; row++)
      for (uint32_t x = 0; x < d->stride; x++)
        if (d->before[(size_t)row * d->stride + x] !=
            d->after[(size_t)row * d->stride + x]) {
          if (x < left)
            left = x;
          if (x + 1 > right)
            right = x + 1;
        }
    d->next_y = (uint16_t)end;
    if (left == d->stride)
      continue;
    unsigned x = left * 8 / d->bits;
    unsigned last = (right * 8 + d->bits - 1) / d->bits;
    if (last > d->width)
      last = d->width;
    *r = (sdlp_rect_t){(uint16_t)x, (uint16_t)y, (uint16_t)(last - x),
                       (uint16_t)(end - y)};
    return true;
  }
  return false;
}
int sdlp_host_write_some(sdlp_host_t *h, uint32_t frame, uint32_t rect,
                         uint32_t offset, const uint8_t *data, size_t size,
                         size_t *consumed) {
  if (!h || !data || !size || !consumed) return SDLP_BAD_ARGUMENT;
  *consumed = 0;
  size_t raw = size < SDLP_DATA_SIZE ? size : SDLP_DATA_SIZE;
  size_t used = raw, encoded = raw;
  unsigned opcode = SDLP_RECT_DATA;
  uint8_t p[SDLP_PAYLOAD_SIZE], candidate[SDLP_PAYLOAD_SIZE - 14];
  memcpy(p + 12, data, raw);
  if (h->info.capabilities & SDLP_RLE) {
    size_t decoded, limit = size < SDLP_RLE_MAX_DECODED ? size : SDLP_RLE_MAX_DECODED;
    size_t n = sdlp_rle_encode(candidate, sizeof(candidate), data, limit, &decoded);
    if (decoded > used || (decoded == used && n + 2 < encoded)) {
      opcode = SDLP_RECT_RLE; used = decoded; encoded = n + 2;
      memcpy(p + 14, candidate, n);
    }
  }
  if (h->compressed && (h->info.capabilities & SDLP_LZ4)) {
    size_t limit = size < SDLP_RLE_MAX_DECODED ? size : SDLP_RLE_MAX_DECODED;
    for (;;) {
      size_t n = sdlp_lz4_encode(candidate, sizeof(candidate), data, limit);
      if (n && (limit > used || (limit == used && n + 2 < encoded))) {
        opcode = SDLP_RECT_LZ4; used = limit; encoded = n + 2;
        memcpy(p + 14, candidate, n);
      }
      if (limit <= used) break;
      limit /= 2;
      if (limit < used) limit = used;
    }
  }
  sdlp_put_u32(p, frame); sdlp_put_u32(p + 4, rect); sdlp_put_u32(p + 8, offset);
  if (opcode != SDLP_RECT_DATA) sdlp_put_u16(p + 12, (uint16_t)used);
  int r = upload_command(h, opcode, p, encoded + 12);
  if (!r) *consumed = used;
  return r;
}
static uint64_t rect_cost(const sdlp_host_t *h, const sdlp_rect_t *r) {
  uint32_t row = sdlp_row_bytes(r->width, h->format);
  uint32_t rows = h->info.max_rect_bytes / row;
  if (!rows)
    return UINT64_MAX;
  uint64_t cost = 0;
  for (unsigned y = 0; y < r->height;) {
    unsigned n = r->height - y;
    if (n > rows)
      n = rows;
    cost += 1 + ((uint64_t)row * n + SDLP_DATA_SIZE - 1) / SDLP_DATA_SIZE;
    y += n;
  }
  return cost;
}
static int upload_rect(sdlp_host_t *h, uint32_t frame, uint32_t *id,
                       const uint8_t *image, uint32_t stride,
                       const sdlp_rect_t *rect) {
  static const unsigned bits[] = {1, 2, 4, 8, 16};
  uint32_t row = sdlp_row_bytes(rect->width, h->format);
  uint32_t max_rows = h->info.max_rect_bytes / row;
  if (!max_rows)
    return SDLP_BAD_ARGUMENT;
  uint8_t data[SDLP_RLE_MAX_DECODED];
  for (unsigned y = 0; y < rect->height;) {
    unsigned rows = rect->height - y;
    if (rows > max_rows)
      rows = max_rows;
    int r =
        sdlp_host_begin_rect(h, frame, ++*id, rect->x, (uint16_t)(rect->y + y),
                             rect->width, (uint16_t)rows);
    if (r)
      return r;
    uint32_t offset = 0, total = row * rows;
    while (offset < total) {
      size_t n = total - offset;
      if (n > sizeof(data))
        n = sizeof(data);
      for (size_t k = 0; k < n; k++) {
        uint32_t pos = offset + (uint32_t)k;
        data[k] = image[(size_t)(rect->y + y + pos / row) * stride +
                        rect->x * bits[h->format] / 8 + pos % row];
      }
      size_t used;
      r = sdlp_host_write_some(h, frame, *id, offset, data, n, &used);
      if (r)
        return r;
      offset += (uint32_t)used;
    }
    y += rows;
  }
  return SDLP_OK;
}
int sdlp_host_upload_frame(sdlp_host_t *h, uint32_t frame, const uint8_t *image,
                           size_t size, uint8_t *retained, bool *valid) {
  if (!h || !h->configured || !image || h->format >= 5)
    return SDLP_BAD_SESSION;
  uint32_t stride = sdlp_row_bytes(h->info.width, h->format);
  if (!frame || retained == image || (uint64_t)stride * h->info.height != size)
    return SDLP_BAD_ARGUMENT;
  bool full = !retained || !valid || !*valid ||
              !(h->info.capabilities & SDLP_PRESERVE_CONTENT);
  if (valid)
    *valid = false;
  sdlp_delta_t delta;
  sdlp_rect_t all = {0, 0, h->info.width, h->info.height}, rect;
  if (!full) {
    (void)sdlp_delta_init(&delta, retained, image, all.width, all.height,
                          h->format);
    uint64_t cost = 0;
    while (sdlp_delta_next(&delta, &rect))
      cost += rect_cost(h, &rect);
    if (!cost) {
      if (h->info.capabilities & SDLP_SYNC) {
        sdlp_cell_t reply;
        int r = sdlp_host_command(h, SDLP_GET_SYNC, NULL, 0, &reply, 0);
        if (r)
          return r;
        if (reply.length != 4 || sdlp_get_u32(reply.payload) > 1)
          return SDLP_INVALID_CELL;
        if (!sdlp_get_u32(reply.payload)) {
          *valid = true;
          return SDLP_OK;
        }
      }
      full = true;
    } else if (cost >= rect_cost(h, &all))
      full = true;
  }
  if (h->window && h->window->active) h->window->collecting = true;
  int r = sdlp_host_begin_frame(h, frame, full);
  if (!full && (r == SDLP_BAD_ARGUMENT || r == SDLP_BAD_STATE)) {
    full = true;
    r = sdlp_host_begin_frame(h, frame, true);
  }
  uint32_t id = 0;
  if (!r && full)
    r = upload_rect(h, frame, &id, image, stride, &all);
  if (!r && !full) {
    (void)sdlp_delta_init(&delta, retained, image, all.width, all.height,
                          h->format);
    while (!r && sdlp_delta_next(&delta, &rect))
      r = upload_rect(h, frame, &id, image, stride, &rect);
  }
  if (!r)
    r = sdlp_host_present(h, frame);
  if (h->window) h->window->collecting = false;
  if (!r)
    r = sdlp_host_wait_frame(h, frame,
                             h->transport.now_us(h->transport.ctx) +
                                 (uint64_t)h->info.present_ms * 1000 + 1000000);
  if (!r && retained && valid) {
    memcpy(retained, image, size);
    *valid = true;
  }
  return r;
}

size_t sdlp_stream_encode(uint8_t out[SDLP_CELL_SIZE],
                          const uint8_t cell[SDLP_CELL_SIZE], bool compact) {
  sdlp_cell_t c;
  if (!out || !sdlp_decode(&c, cell))
    return 0;
  if (!compact) {
    memmove(out, cell, SDLP_CELL_SIZE);
    return SDLP_CELL_SIZE;
  }
  size_t n = 32 + c.length;
  memmove(out, cell, n);
  memcpy(out, "DLP1", 4);
  sdlp_put_u32(out + n, sdlp_crc32(out, n));
  return n + 4;
}
bool sdlp_stream_next(sdlp_stream_t *s, uint8_t cell[SDLP_CELL_SIZE],
                      bool *compact) {
  if (!s || !cell || !compact)
    return false;
  while (s->used >= 4) {
    bool small = !memcmp(s->bytes, "DLP1", 4);
    if (!small && memcmp(s->bytes, "SDLP", 4))
      goto discard;
    if (s->used < 32)
      return false;
    unsigned payload = sdlp_get_u16(s->bytes + 18);
    if (payload > SDLP_PAYLOAD_SIZE || s->bytes[4] != 1 || s->bytes[5])
      goto discard;
    size_t n = small ? 36 + payload : SDLP_CELL_SIZE;
    if (s->used < n)
      return false;
    if (small) {
      if (sdlp_get_u32(s->bytes + n - 4) != sdlp_crc32(s->bytes, n - 4))
        goto discard;
      memset(cell, 0, SDLP_CELL_SIZE);
      memcpy(cell, s->bytes, n - 4);
      memcpy(cell, "SDLP", 4);
      sdlp_put_u32(cell + 252, sdlp_crc32(cell, 252));
    } else
      memcpy(cell, s->bytes, SDLP_CELL_SIZE);
    sdlp_cell_t decoded;
    if (!sdlp_decode(&decoded, cell))
      goto discard;
    s->used -= n;
    memmove(s->bytes, s->bytes + n, s->used);
    *compact = small;
    return true;
  discard:
    memmove(s->bytes, s->bytes + 1, --s->used);
  }
  return false;
}
bool sdlp_stream_feed(sdlp_stream_t *s, uint8_t byte,
                      uint8_t cell[SDLP_CELL_SIZE], bool *compact) {
  if (!s || s->used >= sizeof(s->bytes))
    return false;
  s->bytes[s->used++] = byte;
  return sdlp_stream_next(s, cell, compact);
}
static uint64_t stream_now(void *ctx) {
  sdlp_stream_host_t *s = ctx;
  return s->io.now_us(s->io.ctx);
}
static void stream_sleep(void *ctx, uint32_t us) {
  sdlp_stream_host_t *s = ctx;
  s->io.sleep_us(s->io.ctx, us);
}
static int stream_request(void *ctx, const uint8_t tx[SDLP_CELL_SIZE],
                          uint8_t rx[SDLP_CELL_SIZE], uint64_t deadline) {
  sdlp_stream_host_t *s = ctx;
  uint8_t packet[SDLP_CELL_SIZE];
  size_t n = sdlp_stream_encode(packet, tx, !s->legacy), sent = 0;
  if (!n)
    return -1;
  uint64_t retry = stream_now(s);
  while (stream_now(s) < deadline) {
    uint64_t now = stream_now(s);
    bool progress = false;
    if (sent == n && now >= retry) {
      sent = 0;
      s->parser.used =
          0; /* Discard an incomplete response before exact retry. */
    }
    if (sent < n) {
      int written = s->io.write(s->io.ctx, packet + sent, n - sent);
      if (written < 0 || (size_t)written > n - sent)
        return -1;
      sent += (size_t)written;
      progress = written > 0;
      if (sent == n)
        retry = now + (s->retry_us ? s->retry_us : UINT32_MAX);
    }
    bool compact;
    bool have = sdlp_stream_next(&s->parser, rx, &compact);
    if (!have) {
      uint8_t byte;
      int got = s->io.read(s->io.ctx, &byte, 1);
      if (got < 0 || got > 1)
        return -1;
      if (got)
        have = sdlp_stream_feed(&s->parser, byte, rx, &compact);
      progress = progress || got > 0;
    }
    if (have && rx[6] == SDLP_RESPONSE && rx[7] == tx[7] &&
        !memcmp(rx + 8, tx + 8, 8))
      return 0;
    if (!have && !progress)
      stream_sleep(s, 100);
  }
  return -1;
}
static int stream_push(void *ctx, const uint8_t tx[SDLP_CELL_SIZE], uint64_t deadline) {
  sdlp_stream_host_t *s = ctx;
  uint8_t packet[SDLP_CELL_SIZE];
  size_t n = sdlp_stream_encode(packet, tx, !s->legacy), sent = 0;
  if (!n) return -1;
  while (sent < n && stream_now(s) < deadline) {
    int written = s->io.write(s->io.ctx, packet + sent, n - sent);
    if (written < 0 || (size_t)written > n - sent) return -1;
    sent += (size_t)written;
    if (!written) stream_sleep(s, 100);
  }
  return sent == n ? 0 : -1;
}
bool sdlp_stream_host_init(sdlp_stream_host_t *s, const sdlp_stream_io_t *io) {
  if (!s || !io || !io->now_us || !io->sleep_us || !io->read || !io->write)
    return false;
  memset(s, 0, sizeof(*s));
  s->io = *io;
  s->retry_us = 250000;
  return true;
}
sdlp_transport_t sdlp_stream_transport(sdlp_stream_host_t *s) {
  return (sdlp_transport_t){.ctx = s,
                            .now_us = stream_now,
                            .sleep_us = stream_sleep,
                            .request = stream_request, .push = stream_push};
}
