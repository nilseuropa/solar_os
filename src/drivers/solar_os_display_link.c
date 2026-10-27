#include "solar_os_display_link_transport.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_memory.h"
#include <stddef.h>
#include <string.h>

struct solar_os_display_link {
  u8g2_t u8g2;
  u8x8_display_info_t display_info;
  sdlp_host_t host;
  sdlp_host_window_t *window;
  solar_os_display_link_transport_t io;
  solar_os_display_link_options_t config;
  SemaphoreHandle_t mutex;
  uint8_t *mono, *row, *wire, *retained;
  size_t wire_size;
  bool retained_valid;
  uint16_t *pixels;
  uint32_t frame_id;
  esp_err_t last_error;
};
static const char *TAG = "dlp-host";
static esp_err_t result(int r) {
  if (!r)
    return ESP_OK;
  if (r == SDLP_TIMEOUT || r == SDLP_DEADLINE)
    return ESP_ERR_TIMEOUT;
  if (r == SDLP_NO_MEMORY)
    return ESP_ERR_NO_MEM;
  if (r == SDLP_UNSUPPORTED)
    return ESP_ERR_NOT_SUPPORTED;
  if (r == SDLP_BAD_ARGUMENT)
    return ESP_ERR_INVALID_ARG;
  return ESP_FAIL;
}
static uint64_t now_us(void *ctx) {
  (void)ctx;
  return (uint64_t)esp_timer_get_time();
}
static esp_err_t connect_endpoint(solar_os_display_link_t *d, bool reset) {
  d->retained_valid = false;
  if (d->io.prepare) {
    esp_err_t error = d->io.prepare(d->io.protocol.ctx, reset);
    if (error != ESP_OK) return error;
  }
  sdlp_info_t old_info = d->host.info;
  int r = sdlp_host_discover(&d->host,
                             now_us(NULL) + (uint64_t)d->config.timeout_ms * 1000);
  if (r) {
    ESP_LOGW(TAG, "discovery failed: DLP %d", r);
    return result(r);
  }
  if (d->host.info.width > 2040 || d->host.info.height > 2040 ||
      (size_t)d->host.info.width * d->host.info.height > 1024 * 1024 ||
      d->host.info.present_ms > 120000 ||
      (old_info.width && (old_info.width != d->host.info.width ||
                          old_info.height != d->host.info.height ||
                          old_info.formats != d->host.info.formats ||
                          old_info.preferred_format != d->host.info.preferred_format))) {
    d->host.info = old_info;
    return ESP_ERR_INVALID_SIZE;
  }
  unsigned format = d->config.format < 0 ? d->host.info.preferred_format : (unsigned)d->config.format;
  uint32_t session;
  do {
    session = esp_random();
  } while (!session || session == d->host.session);
  const char *step = "configure";
  r = sdlp_host_configure(&d->host, session, format, d->io.clock_hz);
  if (!r) { step = "extensions"; r = sdlp_host_extensions(&d->host); }
  if (!r && d->window) {
    step = "window negotiation";
    r = sdlp_host_negotiate_window(&d->host, d->window, SDLP_MAX_WINDOW);
    if (!r && !d->window->active) {
      d->host.window = NULL;
      solar_os_memory_free(d->window);
      d->window = NULL;
    }
  }
  if (!r && d->io.configured)
    return d->io.configured(d->io.protocol.ctx, &d->host.info);
  if (r) ESP_LOGW(TAG, "%s failed: DLP %d", step, r);
  return result(r);
}
static void pack_row(solar_os_display_link_t *d, uint16_t y) {
  const unsigned format = d->host.format;
  uint16_t w = d->host.info.width;
  memset(d->row, 0, sdlp_row_bytes(w, format));
  for (uint16_t x = 0; x < w; x++) {
    uint16_t c = d->pixels[(size_t)y * w + x];
    if (format == SDLP_RGB565_BE) {
      d->row[2 * x] = (uint8_t)(c >> 8);
      d->row[2 * x + 1] = (uint8_t)c;
      continue;
    }
    unsigned red = (c >> 11) * 255 / 31, green = ((c >> 5) & 63) * 255 / 63,
             blue = (c & 31) * 255 / 31;
    unsigned gray = (red * 77 + green * 150 + blue * 29 + 128) >> 8;
    if (format == SDLP_GRAY8) d->row[x] = (uint8_t)gray;
    else if (format == SDLP_MONO1)
      d->row[x / 8] |= (uint8_t)((gray < 128 ? 1 : 0) << (x % 8));
    else {
      unsigned bits = format == SDLP_GRAY2 ? 2 : 4, max = (1u << bits) - 1;
      d->row[x * bits / 8] |=
          (uint8_t)(((gray * max + 127) / 255) << ((x * bits) % 8));
    }
  }
}
static esp_err_t upload(solar_os_display_link_t *d) {
  if (!d->host.configured) {
    esp_err_t e = connect_endpoint(d, false);
    if (e != ESP_OK && d->io.reset_available)
      e = connect_endpoint(d, true);
    if (e != ESP_OK)
      return e;
    d->frame_id = 0;
  }
  if (d->frame_id == UINT32_MAX) {
    d->host.configured = false;
    return ESP_ERR_INVALID_STATE;
  }
  uint32_t frame = ++d->frame_id;
  const unsigned format = d->host.format;
  uint16_t w = d->host.info.width, h = d->host.info.height;
  uint32_t row_bytes = sdlp_row_bytes(w, format);
  if (d->wire && d->wire_size >= (size_t)row_bytes * h) {
    for (uint16_t y = 0; y < h; y++) {
      pack_row(d, y);
      memcpy(d->wire + (size_t)y * row_bytes, d->row, row_bytes);
      if ((uint32_t)w * h > 65536 && (y & 31) == 31) vTaskDelay(1);
    }
    int r = sdlp_host_upload_frame(&d->host, frame, d->wire,
                                    (size_t)row_bytes * h, d->retained,
                                    &d->retained_valid);
    if (r) {
      d->retained_valid = false;
      d->host.configured = false;
      ESP_LOGE(TAG, "presentation failed: DLP %d", r);
    }
    return result(r);
  }
  d->retained_valid = false;
  int r = sdlp_host_begin_frame(&d->host, frame, true);
  uint32_t rows_per_rect = d->host.info.max_rect_bytes / row_bytes;
  if (rows_per_rect > h)
    rows_per_rect = h;
  uint32_t rectangle = 0, offset = 0, band_end = 0;
  uint8_t chunk[SDLP_RLE_MAX_DECODED];
  size_t capacity = d->host.compressed && (d->host.info.capabilities & SDLP_LZ4)
      ? sizeof(chunk) : SDLP_DATA_SIZE;
  size_t used = 0;
  for (uint16_t y = 0; !r && y < h; y++) {
    if (y == band_end) {
      uint32_t rows = h - y;
      if (rows > rows_per_rect)
        rows = rows_per_rect;
      band_end = y + rows;
      rectangle++;
      offset = 0;
      used = 0;
      r = sdlp_host_begin_rect(&d->host, frame, rectangle, 0, y, w,
                               (uint16_t)rows);
      if (r)
        break;
    }
    pack_row(d, y);
    for (uint32_t pos = 0; !r && pos < row_bytes;) {
      size_t n = row_bytes - pos;
      if (n > capacity - used)
        n = capacity - used;
      memcpy(chunk + used, d->row + pos, n);
      used += n;
      pos += (uint32_t)n;
      if (used == capacity) {
        for (size_t at = 0; !r && at < used;) {
          size_t consumed;
          r = sdlp_host_write_some(&d->host, frame, rectangle, offset,
                                   chunk + at, used - at, &consumed);
          if (!r) { at += consumed; offset += (uint32_t)consumed; }
        }
        used = 0;
      }
    }
    if (!r && (uint32_t)y + 1 == band_end && used) {
      for (size_t at = 0; !r && at < used;) {
        size_t consumed;
        r = sdlp_host_write_some(&d->host, frame, rectangle, offset,
                                 chunk + at, used - at, &consumed);
        if (!r) { at += consumed; offset += (uint32_t)consumed; }
      }
      used = 0;
    }
    if ((uint32_t)w * h > 65536 && (y & 31) == 31)
      vTaskDelay(1);
  }
  if (!r)
    r = sdlp_host_present(&d->host, frame);
  if (!r)
    r = sdlp_host_wait_frame(
        &d->host, frame,
        now_us(NULL) + (uint64_t)d->host.info.present_ms * 1000 + 1000000);
  if (r) {
    d->host.configured = false;
    ESP_LOGE(TAG, "presentation failed: SDLP %d", r);
  }
  return result(r);
}
static solar_os_display_link_t *from_u8x8(u8x8_t *u) {
  return (solar_os_display_link_t *)((uint8_t *)u -
                                     offsetof(solar_os_display_link_t, u8g2) -
                                     offsetof(u8g2_t, u8x8));
}
static uint8_t display_cb(u8x8_t *u, uint8_t msg, uint8_t arg, void *ptr) {
  (void)ptr;
  solar_os_display_link_t *d = from_u8x8(u);
  if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
    u8x8_d_helper_display_setup_memory(u, &d->display_info);
    return 1;
  }
  if (msg == U8X8_MSG_DISPLAY_DRAW_TILE)
    return 1;
  if (msg == U8X8_MSG_DISPLAY_REFRESH) {
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    uint16_t w = d->host.info.width, h = d->host.info.height;
    size_t stride = (size_t)d->display_info.tile_width * 8;
    for (uint16_t y = 0; y < h; y++)
      for (uint16_t x = 0; x < w; x++)
        d->pixels[(size_t)y * w + x] =
            (d->mono[(size_t)(y / 8) * stride + x] & (1u << (y % 8))) ? 0
                                                                      : 0xffff;
    d->last_error = upload(d);
    xSemaphoreGive(d->mutex);
    return d->last_error == ESP_OK;
  }
  if (msg == U8X8_MSG_DISPLAY_SET_POWER_SAVE &&
      (d->host.info.capabilities & SDLP_POWER_SAVE)) {
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    uint8_t p = arg ? 1 : 0;
    d->last_error =
        result(sdlp_host_command(&d->host, SDLP_SET_POWER, &p, 1, NULL, 0));
    xSemaphoreGive(d->mutex);
    return d->last_error == ESP_OK;
  }
  return 1;
}
esp_err_t solar_os_display_link_open_transport(
    const solar_os_display_link_transport_t *io,
    const solar_os_display_link_options_t *options, solar_os_display_link_t **out) {
  if (!io || !out) return ESP_ERR_INVALID_ARG;
  *out = NULL;
  solar_os_display_link_t *d = solar_os_memory_calloc(
      1, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.link");
  if (!d) {
    if (io->destroy) io->destroy(io->protocol.ctx);
    return ESP_ERR_NO_MEM;
  }
  d->io = *io;
  d->config = options ? *options : (solar_os_display_link_options_t){.format = -1};
  if (!d->config.timeout_ms) d->config.timeout_ms = 5000;
  d->mutex = xSemaphoreCreateMutex();
  esp_err_t e = ESP_ERR_NO_MEM;
  if (!d->mutex) goto fail;
  sdlp_host_init(&d->host, &io->protocol);
  d->host.compressed = d->config.compressed;
  d->window = solar_os_memory_calloc(1, sizeof(*d->window),
      SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.link.window");
  d->host.discovery_sequence = esp_random() & 0x7fffffff;
  e = connect_endpoint(d, false);
  if (e != ESP_OK && d->io.reset_available) e = connect_endpoint(d, true);
  if (e != ESP_OK) goto fail;
  uint16_t w = d->host.info.width, h = d->host.info.height;
  /* Delta caches are optional and bounded. Large displays or memory pressure
   * retain the streaming full-frame path rather than failing attachment. */
  d->wire_size = (size_t)sdlp_row_bytes(w, d->host.format) * h;
  if (d->wire_size <= 256u * 1024u) {
    d->wire = solar_os_memory_alloc(d->wire_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                     "display.link.wire");
    d->retained = solar_os_memory_alloc(d->wire_size, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "display.link.retained");
    if (!d->wire || !d->retained) {
      solar_os_memory_free(d->wire); solar_os_memory_free(d->retained);
      d->wire = d->retained = NULL;
    }
  }
  d->row = solar_os_memory_alloc((size_t)w * 2,
      SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.link.row");
  if (!d->row) { e = ESP_ERR_NO_MEM; goto fail; }
  *out = d;
  return ESP_OK;
fail:
  solar_os_display_link_destroy(d);
  return e;
}
void solar_os_display_link_destroy(solar_os_display_link_t *d) {
  if (!d)
    return;
  if (d->io.destroy) d->io.destroy(d->io.protocol.ctx);
  if (d->mutex)
    vSemaphoreDelete(d->mutex);
  solar_os_memory_free(d->mono);
  solar_os_memory_free(d->pixels);
  solar_os_memory_free(d->row);
  solar_os_memory_free(d->wire);
  solar_os_memory_free(d->retained);
  solar_os_memory_free(d->window);
  solar_os_memory_free(d);
}
static bool ensure_graphics(solar_os_display_link_t *d) {
  if (d->mono && d->pixels) return true;
  uint16_t w = d->host.info.width, h = d->host.info.height;
  d->display_info = (u8x8_display_info_t){.tile_width = (uint8_t)((w + 7) / 8),
                                          .tile_height = (uint8_t)((h + 7) / 8),
                                          .pixel_width = w,
                                          .pixel_height = h};
  d->mono = solar_os_memory_calloc(
      (size_t)d->display_info.tile_width * d->display_info.tile_height, 8,
      SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "display.link.mono");
  d->pixels = solar_os_memory_alloc((size_t)w * h * 2,
                                    SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                    "display.link.rgb");
  if (!d->mono || !d->pixels) {
    solar_os_memory_free(d->mono); solar_os_memory_free(d->pixels);
    d->mono = NULL; d->pixels = NULL;
    return false;
  }
  memset(d->pixels, 0xff, (size_t)w * h * 2);
  u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_cad_empty, u8x8_dummy_cb,
                    u8x8_dummy_cb);
  u8g2_SetupBuffer(&d->u8g2, d->mono, d->display_info.tile_height,
                   u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
  u8g2_InitDisplay(&d->u8g2);
  u8g2_ClearBuffer(&d->u8g2);
  return true;
}
u8g2_t *solar_os_display_link_u8g2(solar_os_display_link_t *d) {
  return d && ensure_graphics(d) ? &d->u8g2 : NULL;
}
const sdlp_info_t *solar_os_display_link_info(const solar_os_display_link_t *d) {
  return d ? &d->host.info : NULL;
}

static bool raster_valid(const uint8_t *data, size_t size, uint16_t w,
                         uint16_t h, uint16_t stride,
                         solar_os_display_format_t format,
                         const uint16_t *palette, size_t palette_size) {
  unsigned bits;
  switch (format) {
  case SOLAR_OS_DISPLAY_FORMAT_MONO1:
    bits = 1;
    break;
  case SOLAR_OS_DISPLAY_FORMAT_INDEX2:
    bits = 2;
    break;
  case SOLAR_OS_DISPLAY_FORMAT_INDEX8:
    bits = 8;
    break;
  case SOLAR_OS_DISPLAY_FORMAT_RGB565:
    bits = 16;
    break;
  default:
    return false;
  }
  size_t row = ((size_t)w * bits + 7) / 8;
  if (!data || !w || !h || stride < row ||
      size < (size_t)(h - 1) * stride + row)
    return false;
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 &&
      (!palette || palette_size < 4))
    return false;
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 &&
      (!palette || palette_size < 256))
    return false;
  if (format == SOLAR_OS_DISPLAY_FORMAT_MONO1 && palette && palette_size < 2)
    return false;
  return true;
}
static uint16_t color(const uint8_t *data, uint16_t stride, unsigned x,
                      unsigned y, solar_os_display_format_t format,
                      const uint16_t *palette, bool invert) {
  const uint8_t *p = data + (size_t)y * stride;
  unsigned index;
  if (format == SOLAR_OS_DISPLAY_FORMAT_RGB565)
    return (uint16_t)((uint16_t)p[2 * x] << 8 | p[2 * x + 1]);
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8)
    return palette[p[x]];
  if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX2) {
    index = (p[x / 4] >> ((x % 4) * 2)) & 3;
    return palette[index];
  }
  index = (p[x / 8] >> (x % 8)) & 1;
  if (invert)
    index ^= 1;
  return palette ? palette[index] : (index ? 0 : 0xffff);
}
static void native_to_logical(unsigned rot, uint16_t w, uint16_t h, unsigned nx,
                              unsigned ny, unsigned *x, unsigned *y) {
  switch (rot) {
  case 1:
    *x = ny;
    *y = w - 1 - nx;
    break;
  case 2:
    *x = w - 1 - nx;
    *y = h - 1 - ny;
    break;
  case 3:
    *x = h - 1 - ny;
    *y = nx;
    break;
  default:
    *x = nx;
    *y = ny;
    break;
  }
}
esp_err_t solar_os_display_link_surface(void *ctx,
                                        const solar_os_display_surface_t *s) {
  solar_os_display_link_t *d = ctx;
  if (!d || !s)
    return ESP_ERR_INVALID_ARG;
  uint16_t w = d->host.info.width, h = d->host.info.height;
  bool rotated = s->rotation == 1 || s->rotation == 3;
  if (s->rotation > 3 || s->native_width != w || s->native_height != h ||
      s->width != (rotated ? h : w) || s->height != (rotated ? w : h) ||
      !raster_valid(s->data, s->data_size, s->width, s->height, s->stride,
                    s->format, s->palette_rgb565, s->palette_size))
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(d->mutex, portMAX_DELAY);
  if (!ensure_graphics(d)) { xSemaphoreGive(d->mutex); return ESP_ERR_NO_MEM; }
  for (unsigned ny = 0; ny < h; ny++)
    for (unsigned nx = 0; nx < w; nx++) {
      unsigned x, y;
      native_to_logical(s->rotation, w, h, nx, ny, &x, &y);
      d->pixels[(size_t)ny * w + nx] =
          color(s->data, s->stride, x, y, s->format, s->palette_rgb565, false);
    }
  esp_err_t e = upload(d);
  d->last_error = e;
  xSemaphoreGive(d->mutex);
  return e;
}
esp_err_t solar_os_display_link_frame(void *ctx,
                                      const solar_os_display_raster_t *f) {
  solar_os_display_link_t *d = ctx;
  if (!d || !f)
    return ESP_ERR_INVALID_ARG;
  unsigned rot = d->u8g2.cb == U8G2_R1   ? 1
                 : d->u8g2.cb == U8G2_R2 ? 2
                 : d->u8g2.cb == U8G2_R3 ? 3
                                         : 0;
  uint16_t w = d->host.info.width, h = d->host.info.height,
           lw = (rot & 1) ? h : w, lh = (rot & 1) ? w : h;
  if (!f->width || !f->height || (uint32_t)f->x + f->width > lw ||
      (uint32_t)f->y + f->height > lh ||
      !raster_valid(f->data, f->data_size, f->source_width, f->source_height,
                    f->source_stride, f->format, f->palette_rgb565,
                    f->palette_size))
    return ESP_ERR_INVALID_ARG;
  uint16_t bg = f->background_rgb565;
  if (f->format != SOLAR_OS_DISPLAY_FORMAT_RGB565) {
    if (f->palette_rgb565) {
      if (f->background_index >= f->palette_size)
        return ESP_ERR_INVALID_ARG;
      bg = f->palette_rgb565[f->background_index];
    } else
      bg = ((f->background_index != 0) ^ f->palette_inverted) ? 0 : 0xffff;
  }
  xSemaphoreTake(d->mutex, portMAX_DELAY);
  if (!ensure_graphics(d)) { xSemaphoreGive(d->mutex); return ESP_ERR_NO_MEM; }
  for (unsigned ny = 0; ny < h; ny++)
    for (unsigned nx = 0; nx < w; nx++) {
      unsigned x, y;
      native_to_logical(rot, w, h, nx, ny, &x, &y);
      if (x >= f->x && y >= f->y && x < (unsigned)f->x + f->width &&
          y < (unsigned)f->y + f->height) {
        unsigned sx = (x - f->x) * (uint32_t)f->source_width / f->width,
                 sy = (y - f->y) * (uint32_t)f->source_height / f->height;
        d->pixels[(size_t)ny * w + nx] =
            color(f->data, f->source_stride, sx, sy, f->format,
                  f->palette_rgb565, f->palette_inverted);
      } else if (f->clear_background)
        d->pixels[(size_t)ny * w + nx] = bg;
    }
  esp_err_t e = upload(d);
  d->last_error = e;
  xSemaphoreGive(d->mutex);
  return e;
}
esp_err_t solar_os_display_link_power_state(void *ctx) {
  solar_os_display_link_t *d = ctx;
  if (!d)
    return ESP_ERR_INVALID_ARG;
  u8g2_SendBuffer(&d->u8g2);
  return d->last_error;
}

unsigned solar_os_display_link_format(const solar_os_display_link_t *d) {
  return d ? d->host.format : 0;
}
esp_err_t solar_os_display_link_present(solar_os_display_link_t *d,
    const uint8_t *pixels, size_t size) {
  if (!d || !pixels) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(d->mutex, portMAX_DELAY);
  esp_err_t error = ESP_OK;
  if (!d->host.configured) {
    error = connect_endpoint(d, false);
    if (error == ESP_OK) d->frame_id = 0;
  }
  size_t expected = (size_t)sdlp_row_bytes(d->host.info.width, d->host.format) * d->host.info.height;
  if (error == ESP_OK && size != expected) error = ESP_ERR_INVALID_SIZE;
  if (error == ESP_OK && d->frame_id == UINT32_MAX) error = ESP_ERR_INVALID_STATE;
  if (error == ESP_OK) {
    int r = sdlp_host_upload_frame(&d->host, ++d->frame_id, pixels, size,
        size <= d->wire_size ? d->retained : NULL, &d->retained_valid);
    error = result(r);
    if (r) { d->retained_valid = false; d->host.configured = false; }
  }
  xSemaphoreGive(d->mutex);
  return error;
}
