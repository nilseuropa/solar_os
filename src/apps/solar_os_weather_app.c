#include "solar_os_weather_app.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_json.h"
#include "solar_os_http_client.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_queue.h"
#include "solar_os_sensors.h"
#include "solar_os_task.h"
#include "solar_os_time.h"
#include "solar_os_wifi.h"

/*
 * NOTE FOR REVIEW — bindings confirmed against nilseuropa/solar_os:
 *   solar_os_gfx.h, solar_os_http_client.h, solar_os_json.h (cJSON-backed),
 *   solar_os_sensors.h (solar_os_sensors_read_environment), the NVS blob
 *   pattern in solar_os_webradio_catalog.c, and the worker task/queue
 *   pattern in solar_os_curl.c were all read directly from source.
 *   Context accessors (argc/argv/gfx/request_exit/set_graphics_active)
 *   live directly in solar_os.h (no separate solar_os_context.h), and
 *   are used exactly as solar_os_clock.c uses them.
 *   Still open: the exact package/Kconfig plumbing to guard this app
 *   behind a new SOLAR_OS_PACKAGE_APP_WEATHER flag (see integration
 *   notes at the bottom of this file) — that wiring lives outside any
 *   single .c file and needs a maintainer's confirmation.
 */

#define WEATHER_TASK_STACK 12288
#define WEATHER_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(WEATHER_TASK_STACK);

#define WEATHER_EVENT_QUEUE_LEN 4
#define WEATHER_HTTP_TIMEOUT_MS 8000
#define WEATHER_GEOCODE_BUFFER_MAX (4U * 1024U)
#define WEATHER_FORECAST_BUFFER_MAX (12U * 1024U)
#define WEATHER_PLACE_MAX 64
#define WEATHER_ERROR_MAX 96
#define WEATHER_DAILY_COUNT 5
#define WEATHER_REFRESH_INTERVAL_MS (15U * 60U * 1000U)
#define WEATHER_TICK_INTERVAL_MS 1000U

#define WEATHER_NVS_NAMESPACE "weather"
#define WEATHER_NVS_CONFIG_KEY "config"
#define WEATHER_NVS_CACHE_KEY "cache"
#define WEATHER_CONFIG_MAGIC 0x57544843U /* "WTHC" */
#define WEATHER_CACHE_MAGIC 0x57544843U + 1U
#define WEATHER_BLOB_VERSION 1U

typedef enum {
    WEATHER_EVENT_STATUS = 0,
    WEATHER_EVENT_DONE,
} weather_event_type_t;

typedef struct {
    uint8_t weather_code;
    float temp_max_c;
    float temp_min_c;
} weather_daily_t;

typedef struct {
    float temp_c;
    float feels_c;
    float humidity_pct;
    float wind_kmh;
    float pressure_hpa;
    uint8_t weather_code;
} weather_current_t;

typedef struct {
    bool valid;
    char place[WEATHER_PLACE_MAX];
    weather_current_t current;
    weather_daily_t daily[WEATHER_DAILY_COUNT];
    size_t daily_count;
    uint64_t fetched_at_ms;
} weather_snapshot_t;

typedef struct {
    weather_event_type_t type;
    bool success;
    weather_snapshot_t snapshot;
    char message[WEATHER_ERROR_MAX];
} weather_event_t;

/* NVS-persisted config: last resolved place + coordinates. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    char place[WEATHER_PLACE_MAX];
    double lat;
    double lon;
    bool has_location;
} weather_config_blob_t;

/* NVS-persisted cache: last successful snapshot, for offline fallback. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    weather_snapshot_t snapshot;
} weather_cache_blob_t;

typedef struct {
    weather_config_blob_t config;
    weather_snapshot_t forecast;      /* live or last-known-good, see forecast_stale */
    bool forecast_stale;
    bool forecast_ever_loaded;
    solar_os_environment_t environment;
    bool environment_valid;
    bool environment_stale;

    QueueHandle_t events;
    TaskHandle_t task;
    volatile bool task_running;
    volatile bool stop_requested;
    char pending_place[WEATHER_PLACE_MAX]; /* place requested for the in-flight fetch */

    uint64_t last_fetch_started_ms;
    char status_message[WEATHER_ERROR_MAX];
    bool have_error;
    char error_message[WEATHER_ERROR_MAX];

    bool suspended;
} weather_state_t;

static const char *TAG = "solar_os_weather";
static void *weather_state_storage;
#define weather_state (*(weather_state_t *)weather_state_storage)

/* ------------------------------------------------------------------ */
/* WMO weather code -> short label, used next to the current icon.    */
/* ------------------------------------------------------------------ */
static const char *weather_wmo_label(uint8_t code)
{
    switch (code) {
    case 0: return "clear sky";
    case 1: case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 61: case 63: case 65: return "rain";
    case 71: case 73: case 75: return "snow";
    case 80: case 81: case 82: return "showers";
    case 95: case 96: case 99: return "thunderstorm";
    default: return "unknown";
    }
}

typedef enum {
    WEATHER_ICON_SUN,
    WEATHER_ICON_PARTIAL,
    WEATHER_ICON_CLOUD,
    WEATHER_ICON_RAIN,
    WEATHER_ICON_STORM,
} weather_icon_t;

static weather_icon_t weather_wmo_icon(uint8_t code)
{
    if (code == 0) return WEATHER_ICON_SUN;
    if (code == 1 || code == 2) return WEATHER_ICON_PARTIAL;
    if (code == 95 || code == 96 || code == 99) return WEATHER_ICON_STORM;
    if ((code >= 51 && code <= 82)) return WEATHER_ICON_RAIN;
    return WEATHER_ICON_CLOUD;
}

/* ------------------------------------------------------------------ */
/* NVS persistence — mirrors the blob pattern in                      */
/* solar_os_webradio_catalog.c (magic + version + fixed struct).      */
/* ------------------------------------------------------------------ */
static esp_err_t weather_nvs_load(const char *key, void *out, size_t out_len, uint32_t expect_magic)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = out_len;
    err = nvs_get_blob(handle, key, out, &required);
    nvs_close(handle);
    if (err != ESP_OK || required != out_len) {
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    const uint32_t *magic = (const uint32_t *)out;
    if (*magic != expect_magic) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t weather_nvs_save(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void weather_load_persisted_state(void)
{
    weather_config_blob_t config = {0};
    if (weather_nvs_load(WEATHER_NVS_CONFIG_KEY, &config, sizeof(config),
                         WEATHER_CONFIG_MAGIC) == ESP_OK) {
        weather_state.config = config;
    }

    weather_cache_blob_t cache = {0};
    if (weather_nvs_load(WEATHER_NVS_CACHE_KEY, &cache, sizeof(cache),
                         WEATHER_CACHE_MAGIC) == ESP_OK) {
        weather_state.forecast = cache.snapshot;
        weather_state.forecast_stale = true;
        weather_state.forecast_ever_loaded = true;
    }
}

static void weather_save_config(void)
{
    weather_state.config.magic = WEATHER_CONFIG_MAGIC;
    weather_state.config.version = WEATHER_BLOB_VERSION;
    esp_err_t err = weather_nvs_save(WEATHER_NVS_CONFIG_KEY, &weather_state.config,
                                     sizeof(weather_state.config));
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "failed to persist config: %s", esp_err_to_name(err));
    }
}

static void weather_save_cache(const weather_snapshot_t *snapshot)
{
    weather_cache_blob_t blob = {
        .magic = WEATHER_CACHE_MAGIC,
        .version = WEATHER_BLOB_VERSION,
        .snapshot = *snapshot,
    };
    esp_err_t err = weather_nvs_save(WEATHER_NVS_CACHE_KEY, &blob, sizeof(blob));
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "failed to persist cache: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------ */
/* HTTP fetch into a bounded heap buffer.                             */
/* ------------------------------------------------------------------ */
typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool truncated;
} weather_fetch_buffer_t;

static esp_err_t weather_http_event(const solar_os_http_event_t *event, void *user_data)
{
    weather_fetch_buffer_t *buf = (weather_fetch_buffer_t *)user_data;
    if (event == NULL || event->type != SOLAR_OS_HTTP_EVENT_DATA || buf == NULL) {
        return ESP_OK;
    }
    if (weather_state.stop_requested) {
        return ESP_FAIL;
    }

    size_t remaining = buf->capacity > buf->length ? buf->capacity - buf->length - 1 : 0;
    size_t to_copy = event->data_len > remaining ? remaining : event->data_len;
    if (to_copy < event->data_len) {
        buf->truncated = true;
    }
    if (to_copy > 0) {
        memcpy(buf->buffer + buf->length, event->data, to_copy);
        buf->length += to_copy;
        buf->buffer[buf->length] = '\0';
    }
    return ESP_OK;
}

static esp_err_t weather_http_get(const char *url, char *buffer, size_t buffer_len,
                                  int *out_status)
{
    weather_fetch_buffer_t fetch_buf = {.buffer = buffer, .capacity = buffer_len};
    buffer[0] = '\0';

    const solar_os_http_request_options_t options = {
        .url = url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
        .follow_redirects = true,
        .event_handler = weather_http_event,
        .receive_buffer_size = 1024,
        .transmit_buffer_size = 512,
        .user_agent = "SolarOS-weather/0.1",
        .user_data = &fetch_buf,
        .cancel_flag = &weather_state.stop_requested,
    };

    solar_os_http_request_t *request = NULL;
    esp_err_t err = solar_os_http_request_create(&options, &request);
    if (err != ESP_OK) {
        return err;
    }

    solar_os_http_response_t response = {0};
    err = solar_os_http_request_perform(request, &response);
    solar_os_http_request_destroy(request);

    if (out_status != NULL) {
        *out_status = response.status_code;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (fetch_buf.truncated) {
        SOLAR_OS_LOGW(TAG, "response truncated at %u bytes", (unsigned)buffer_len);
    }
    return ESP_OK;
}

/* Minimal RFC3986 percent-encoding for a single query parameter value. */
static void weather_urlencode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0' && o + 4 < out_len; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[(*p >> 4) & 0xF];
            out[o++] = hex[*p & 0xF];
        }
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Geocoding: place name -> lat/lon (Open-Meteo Geocoding API).       */
/* ------------------------------------------------------------------ */
static esp_err_t weather_geocode(const char *place, double *lat, double *lon,
                                 char *resolved_name, size_t resolved_name_len)
{
    char encoded[128];
    weather_urlencode(place, encoded, sizeof(encoded));

    char url[256];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=fr",
             encoded);

    char *body = malloc(WEATHER_GEOCODE_BUFFER_MAX);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = weather_http_get(url, body, WEATHER_GEOCODE_BUFFER_MAX, &status);
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    solar_os_json_doc_t *doc = NULL;
    err = solar_os_json_parse_cstr(body, &doc);
    free(body);
    if (err != ESP_OK) {
        return err;
    }

    const solar_os_json_value_t *root = solar_os_json_root(doc);
    const solar_os_json_value_t *first = solar_os_json_path_get(root, "results[0]");
    if (first == NULL) {
        solar_os_json_free(doc);
        return ESP_ERR_NOT_FOUND;
    }

    const solar_os_json_value_t *lat_v = solar_os_json_object_get(first, "latitude");
    const solar_os_json_value_t *lon_v = solar_os_json_object_get(first, "longitude");
    if (lat_v == NULL || lon_v == NULL || !solar_os_json_is_number(lat_v) ||
        !solar_os_json_is_number(lon_v)) {
        solar_os_json_free(doc);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *lat = cJSON_GetNumberValue((const cJSON *)lat_v);
    *lon = cJSON_GetNumberValue((const cJSON *)lon_v);

    if (resolved_name != NULL && resolved_name_len > 0) {
        const solar_os_json_value_t *name_v = solar_os_json_object_get(first, "name");
        if (name_v == NULL ||
            solar_os_json_get_string(name_v, resolved_name, resolved_name_len) != ESP_OK) {
            strlcpy(resolved_name, place, resolved_name_len);
        }
    }

    solar_os_json_free(doc);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Forecast fetch + parse (Open-Meteo Forecast API).                  */
/* ------------------------------------------------------------------ */
static double weather_json_number(const solar_os_json_value_t *root, const char *path, double fallback)
{
    const solar_os_json_value_t *v = solar_os_json_path_get(root, path);
    if (v == NULL || !solar_os_json_is_number(v)) {
        return fallback;
    }
    return cJSON_GetNumberValue((const cJSON *)v);
}

static esp_err_t weather_fetch_forecast(double lat, double lon, weather_snapshot_t *out)
{
    char url[384];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
             "weather_code,wind_speed_10m,surface_pressure"
             "&daily=temperature_2m_max,temperature_2m_min,weather_code"
             "&forecast_days=%d&timezone=auto",
             lat, lon, WEATHER_DAILY_COUNT);

    char *body = malloc(WEATHER_FORECAST_BUFFER_MAX);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = weather_http_get(url, body, WEATHER_FORECAST_BUFFER_MAX, &status);
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    solar_os_json_doc_t *doc = NULL;
    err = solar_os_json_parse_cstr(body, &doc);
    free(body);
    if (err != ESP_OK) {
        return err;
    }
    const solar_os_json_value_t *root = solar_os_json_root(doc);

    memset(out, 0, sizeof(*out));
    out->current.temp_c = (float)weather_json_number(root, "current.temperature_2m", NAN);
    out->current.feels_c = (float)weather_json_number(root, "current.apparent_temperature", NAN);
    out->current.humidity_pct = (float)weather_json_number(root, "current.relative_humidity_2m", NAN);
    out->current.wind_kmh = (float)weather_json_number(root, "current.wind_speed_10m", NAN);
    out->current.pressure_hpa = (float)weather_json_number(root, "current.surface_pressure", NAN);
    out->current.weather_code = (uint8_t)weather_json_number(root, "current.weather_code", 3);

    for (size_t i = 0; i < WEATHER_DAILY_COUNT; i++) {
        char path[48];
        snprintf(path, sizeof(path), "daily.temperature_2m_max[%u]", (unsigned)i);
        double tmax = weather_json_number(root, path, NAN);
        snprintf(path, sizeof(path), "daily.temperature_2m_min[%u]", (unsigned)i);
        double tmin = weather_json_number(root, path, NAN);
        snprintf(path, sizeof(path), "daily.weather_code[%u]", (unsigned)i);
        double code = weather_json_number(root, path, NAN);
        if (isnan(tmax) || isnan(tmin) || isnan(code)) {
            break;
        }
        out->daily[i].temp_max_c = (float)tmax;
        out->daily[i].temp_min_c = (float)tmin;
        out->daily[i].weather_code = (uint8_t)code;
        out->daily_count++;
    }

    solar_os_json_free(doc);

    if (isnan(out->current.temp_c)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out->valid = true;
    out->fetched_at_ms = (uint64_t)(esp_timer_get_time() / 1000);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Worker task: geocode (if needed) + forecast fetch, reports back    */
/* through the event queue. Never touches gfx directly (foreground    */
/* only), mirroring solar_os_curl.c.                                  */
/* ------------------------------------------------------------------ */
static void weather_send_event(const weather_event_t *event)
{
    if (weather_state.events == NULL) {
        return;
    }
    xQueueSend(weather_state.events, event, pdMS_TO_TICKS(200));
}

static void weather_task(void *arg)
{
    (void)arg;
    weather_event_t event = {.type = WEATHER_EVENT_DONE};
    strlcpy(event.snapshot.place, weather_state.pending_place, sizeof(event.snapshot.place));

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.started || !wifi.connected || !wifi.has_ip) {
        event.success = false;
        strlcpy(event.message, "wifi not connected", sizeof(event.message));
        weather_send_event(&event);
        goto done;
    }

    double lat = weather_state.config.lat;
    double lon = weather_state.config.lon;
    bool need_geocode = !weather_state.config.has_location ||
        strcmp(weather_state.config.place, weather_state.pending_place) != 0;

    if (need_geocode) {
        char resolved[WEATHER_PLACE_MAX];
        esp_err_t err = weather_geocode(weather_state.pending_place, &lat, &lon,
                                        resolved, sizeof(resolved));
        if (err != ESP_OK) {
            event.success = false;
            snprintf(event.message, sizeof(event.message),
                     "geocoding failed: %s", esp_err_to_name(err));
            weather_send_event(&event);
            goto done;
        }
        strlcpy(event.snapshot.place, resolved, sizeof(event.snapshot.place));
    }

    esp_err_t err = weather_fetch_forecast(lat, lon, &event.snapshot);
    if (err != ESP_OK) {
        event.success = false;
        snprintf(event.message, sizeof(event.message),
                 "forecast fetch failed: %s", esp_err_to_name(err));
        weather_send_event(&event);
        goto done;
    }

    event.snapshot.valid = true;
    if (event.snapshot.place[0] == '\0') {
        strlcpy(event.snapshot.place, weather_state.pending_place, sizeof(event.snapshot.place));
    }
    event.success = true;

    /* Persist resolved location + snapshot for offline fallback. */
    strlcpy(weather_state.config.place, weather_state.pending_place,
            sizeof(weather_state.config.place));
    weather_state.config.lat = lat;
    weather_state.config.lon = lon;
    weather_state.config.has_location = true;
    weather_save_config();
    weather_save_cache(&event.snapshot);

    weather_send_event(&event);

done:
    weather_state.task_running = false;
    solar_os_task_delete_internal(NULL);
}

static void weather_start_refresh(const char *place)
{
    if (weather_state.task_running) {
        return; /* a fetch is already in flight */
    }
    strlcpy(weather_state.pending_place, place, sizeof(weather_state.pending_place));
    weather_state.stop_requested = false;
    weather_state.last_fetch_started_ms = (uint64_t)(esp_timer_get_time() / 1000);
    weather_state.task_running = true;

    if (solar_os_task_create_pinned_internal(
            weather_task, "weather_fetch", WEATHER_TASK_STACK, NULL,
            WEATHER_TASK_PRIORITY, &weather_state.task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        weather_state.task_running = false;
        weather_state.have_error = true;
        strlcpy(weather_state.error_message, "failed to start fetch task",
                sizeof(weather_state.error_message));
    }
}

/* ------------------------------------------------------------------ */
/* Rendering — see weather_mockup.html for the visual reference.      */
/* Real constraint vs. the mockup: solar_os_gfx has a fixed font set   */
/* (max SOLAR_OS_GFX_FONT_BOLD_20), no arbitrary point sizes, so the   */
/* "hero" temperature below is the largest built-in bold font, not a  */
/* 56-62px custom digit like in the HTML mockup. A custom vector      */
/* digit renderer (see solar_os_clock.c's seven-segment drawing) is a */
/* natural follow-up if a bigger hero number is wanted — left open.   */
/* ------------------------------------------------------------------ */
static void weather_draw_icon(solar_os_gfx_t *gfx, weather_icon_t icon, int cx, int cy, int r)
{
    switch (icon) {
    case WEATHER_ICON_SUN:
        solar_os_gfx_fill_circle(gfx, cx, cy, r / 2);
        for (int i = 0; i < 8; i++) {
            double a = i * (3.14159265 / 4.0);
            int x0 = cx + (int)(cos(a) * r * 0.75);
            int y0 = cy + (int)(sin(a) * r * 0.75);
            int x1 = cx + (int)(cos(a) * r);
            int y1 = cy + (int)(sin(a) * r);
            solar_os_gfx_line(gfx, x0, y0, x1, y1);
        }
        break;
    case WEATHER_ICON_PARTIAL:
        solar_os_gfx_circle(gfx, cx - r / 3, cy - r / 4, r / 2);
        solar_os_gfx_fill_circle(gfx, cx + r / 4, cy + r / 5, (r * 3) / 5);
        break;
    case WEATHER_ICON_CLOUD:
        solar_os_gfx_fill_circle(gfx, cx - r / 2, cy + r / 6, r / 2);
        solar_os_gfx_fill_circle(gfx, cx, cy - r / 4, (r * 3) / 5);
        solar_os_gfx_fill_circle(gfx, cx + r / 2, cy + r / 8, r / 2);
        solar_os_gfx_fill_rect(gfx, cx - r / 2, cy, r, r / 2);
        break;
    case WEATHER_ICON_RAIN:
        weather_draw_icon(gfx, WEATHER_ICON_CLOUD, cx, cy - r / 4, (r * 3) / 4);
        for (int i = -1; i <= 1; i++) {
            solar_os_gfx_line(gfx, cx + i * (r / 3), cy + r / 3,
                              cx + i * (r / 3) - 3, cy + r);
        }
        break;
    case WEATHER_ICON_STORM: {
        weather_draw_icon(gfx, WEATHER_ICON_CLOUD, cx, cy - r / 4, (r * 3) / 4);
        const solar_os_gfx_point_t bolt[] = {
            {cx + 2, cy + r / 3}, {cx - 8, cy + r}, {cx + 2, cy + r},
            {cx - 4, cy + (r * 3) / 2},
        };
        solar_os_gfx_fill_polygon(gfx, bolt, sizeof(bolt) / sizeof(bolt[0]));
        break;
    }
    }
}

static void weather_format_temp(char *out, size_t out_len, float value)
{
    if (isnan(value)) {
        strlcpy(out, "--", out_len);
    } else {
        snprintf(out, out_len, "%.0f", (double)value);
    }
}

static void weather_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || weather_state.suspended) {
        return;
    }

    const int w = (int)solar_os_gfx_width(gfx);
    const int h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 0, 0, w - 1, h - 1);
    solar_os_gfx_line(gfx, 0, 28, w, 28);
    solar_os_gfx_line(gfx, 0, h - 45, w, h - 45);

    /* header */
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
    const char *place = weather_state.forecast.place[0] != '\0'
        ? weather_state.forecast.place
        : (weather_state.config.place[0] != '\0' ? weather_state.config.place : "(no place set)");
    solar_os_gfx_text(gfx, 8, 20, place);

    if (!weather_state.forecast.valid && !weather_state.forecast_ever_loaded) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
        solar_os_gfx_text(gfx, 8, 60, weather_state.have_error ? weather_state.error_message
                                                               : "fetching forecast...");
    } else {
        weather_icon_t icon = weather_wmo_icon(weather_state.forecast.current.weather_code);
        weather_draw_icon(gfx, icon, 70, 120, 38);

        char temp_str[8];
        weather_format_temp(temp_str, sizeof(temp_str), weather_state.forecast.current.temp_c);
        char temp_label[16];
        snprintf(temp_label, sizeof(temp_label), "%s\xC2\xB0", temp_str); /* UTF-8 degree sign */
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
        solar_os_gfx_text(gfx, 130, 90, temp_label);

        char feels_str[8];
        weather_format_temp(feels_str, sizeof(feels_str), weather_state.forecast.current.feels_c);
        char feels_label[32];
        snprintf(feels_label, sizeof(feels_label), "feels %s\xC2\xB0", feels_str);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
        solar_os_gfx_text(gfx, 132, 108, feels_label);

        if (weather_state.forecast_stale) {
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
            solar_os_gfx_text(gfx, 8, 40, "forecast: last known (offline)");
        }

        /* right-hand stats column */
        char line[32];
        int y = 40, lh = 22;
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
        snprintf(line, sizeof(line), "humidity  %.0f %%",
                 (double)weather_state.forecast.current.humidity_pct);
        solar_os_gfx_text(gfx, 260, y, line); y += lh;
        snprintf(line, sizeof(line), "wind      %.0f km/h",
                 (double)weather_state.forecast.current.wind_kmh);
        solar_os_gfx_text(gfx, 260, y, line); y += lh;
        snprintf(line, sizeof(line), "pressure  %.0f hPa",
                 (double)weather_state.forecast.current.pressure_hpa);
        solar_os_gfx_text(gfx, 260, y, line); y += lh;
        solar_os_gfx_text(gfx, 260, y, weather_wmo_label(weather_state.forecast.current.weather_code));
    }

    /* local sensor box — independent of network state */
    const int box_x = 8, box_y = h - 45 - 68, box_w = 155, box_h = 64;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, box_x, box_y, box_w, box_h);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
    solar_os_gfx_text(gfx, box_x + 8, box_y + 14, "SENSOR");
    if (weather_state.environment_valid) {
        char t[16];
        snprintf(t, sizeof(t), "%.1f\xC2\xB0" "C", (double)weather_state.environment.temperature_c);
        char rh[16];
        snprintf(rh, sizeof(rh), "%.0f%%", (double)weather_state.environment.humidity_percent);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_14);
        if (weather_state.environment_stale) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
        }
        solar_os_gfx_text(gfx, box_x + 8, box_y + 34, t);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
        solar_os_gfx_text(gfx, box_x + 8, box_y + 50, rh);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, box_x + 8, box_y + box_h - 6,
                          weather_state.environment_stale ? "stale" : "live");
    } else {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, box_x + 8, box_y + 34, "no sensor");
    }

    /* forecast strip */
    if (weather_state.forecast.daily_count > 0) {
        int n = (int)weather_state.forecast.daily_count;
        int col = w / n;
        for (int i = 0; i < n; i++) {
            int cx = col * i + col / 2;
            weather_icon_t icon = weather_wmo_icon(weather_state.forecast.daily[i].weather_code);
            weather_draw_icon(gfx, icon, cx, h - 22, 10);
            char range[16];
            char tmax[8], tmin[8];
            weather_format_temp(tmax, sizeof(tmax), weather_state.forecast.daily[i].temp_max_c);
            weather_format_temp(tmin, sizeof(tmin), weather_state.forecast.daily[i].temp_min_c);
            snprintf(range, sizeof(range), "%s/%s", tmax, tmin);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
            solar_os_gfx_text(gfx, cx - 12, h - 4, range);
            if (i > 0) {
                solar_os_gfx_line(gfx, col * i, h - 45, col * i, h);
            }
        }
    }

    solar_os_gfx_present(gfx);
}

/* ------------------------------------------------------------------ */
/* App lifecycle                                                      */
/* ------------------------------------------------------------------ */
static bool weather_parse_args(solar_os_context_t *ctx, char *place, size_t place_len)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc <= 1) {
        place[0] = '\0';
        return true;
    }
    if (argc != 2) {
        return false;
    }
    strlcpy(place, solar_os_context_argv(ctx, 1), place_len);
    return true;
}

static esp_err_t weather_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char place[WEATHER_PLACE_MAX];
    if (!weather_parse_args(ctx, place, sizeof(place))) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&weather_state, 0, sizeof(weather_state));
    weather_state.events = solar_os_queue_create(WEATHER_EVENT_QUEUE_LEN, sizeof(weather_event_t));
    if (weather_state.events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    weather_load_persisted_state();

    const char *effective_place = place[0] != '\0' ? place
        : (weather_state.config.place[0] != '\0' ? weather_state.config.place : NULL);

    solar_os_sensors_init();

    solar_os_context_set_graphics_active(ctx, true);

    if (effective_place != NULL) {
        weather_start_refresh(effective_place);
    } else {
        weather_state.have_error = true;
        strlcpy(weather_state.error_message,
                "usage: weather <place> (no place configured yet)",
                sizeof(weather_state.error_message));
    }

    weather_render(ctx);
    return ESP_OK;
}

static void weather_stop(solar_os_context_t *ctx)
{
    weather_state.stop_requested = true;
    for (uint32_t i = 0; i < 60 && weather_state.task_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (weather_state.events != NULL) {
        solar_os_queue_delete(weather_state.events);
        weather_state.events = NULL;
    }
    solar_os_context_set_graphics_active(ctx, false);
}

static void weather_suspend(solar_os_context_t *ctx)
{
    weather_state.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void weather_resume(solar_os_context_t *ctx)
{
    weather_state.suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    weather_render(ctx);
}

static void weather_drain_events(solar_os_context_t *ctx)
{
    weather_event_t event;
    while (weather_state.events != NULL &&
           xQueueReceive(weather_state.events, &event, 0) == pdPASS) {
        if (event.type != WEATHER_EVENT_DONE) {
            continue;
        }
        if (event.success) {
            weather_state.forecast = event.snapshot;
            weather_state.forecast_stale = false;
            weather_state.forecast_ever_loaded = true;
            weather_state.have_error = false;
        } else {
            weather_state.have_error = true;
            strlcpy(weather_state.error_message, event.message,
                    sizeof(weather_state.error_message));
            if (weather_state.forecast_ever_loaded) {
                weather_state.forecast_stale = true;
            }
        }
        weather_render(ctx);
    }
}

static bool weather_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const uint8_t ch = (uint8_t)event->data.ch;
        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_request_exit(ctx);
        } else {
            /* any other key forces an immediate refresh */
            const char *place = weather_state.config.place[0] != '\0'
                ? weather_state.config.place : NULL;
            if (place != NULL) {
                weather_start_refresh(place);
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        weather_drain_events(ctx);

        solar_os_environment_t environment;
        if (solar_os_sensors_read_environment(&environment) == ESP_OK) {
            weather_state.environment = environment;
            weather_state.environment_valid = true;
            weather_state.environment_stale = false;
        } else if (weather_state.environment_valid) {
            weather_state.environment_stale = true;
        }

        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (weather_state.config.has_location &&
            now_ms - weather_state.last_fetch_started_ms > WEATHER_REFRESH_INTERVAL_MS) {
            weather_start_refresh(weather_state.config.place);
        }

        if (!weather_state.suspended) {
            weather_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        weather_resume(ctx);
        return true;
    }

    return false;
}

static void weather_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    strlcpy(buffer, "weather", buffer_len);
}

static bool weather_state_release_ready(void)
{
    return !weather_state.task_running;
}

const solar_os_app_t solar_os_weather_app = {
    .name = "weather",
    .summary = "forecast + local sensor readout",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = weather_start,
    .suspend = weather_suspend,
    .resume = weather_resume,
    .stop = weather_stop,
    .event = weather_event,
    .title = weather_title,
    .state_slot = &weather_state_storage,
    .state_size = sizeof(weather_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = weather_state_release_ready,
    .worker_stack_bytes = WEATHER_TASK_STACK,
    .tick_interval_ms = WEATHER_TICK_INTERVAL_MS,
    .tick_deadline_ms = 100U,
};

/* ------------------------------------------------------------------
 * INTEGRATION NOTES (not yet done by this file alone):
 *
 * 1. Registry entry — add to src/apps/solar_os_app_registry.c, guarded
 *    like every other app:
 *
 *      #if SOLAR_OS_PACKAGE_APP_WEATHER
 *      #include "solar_os_weather_app.h"
 *      #endif
 *      ...
 *      #if SOLAR_OS_PACKAGE_APP_WEATHER
 *      APP_ENTRY("weather", "forecast + local sensor readout",
 *                &solar_os_weather_app,
 *                SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY,
 *                "weather [place]", 1, 2),
 *      #endif
 *
 * 2. Package flag — SOLAR_OS_PACKAGE_APP_WEATHER needs to be wired into
 *    the Kconfig/CMake package system the same way SOLAR_OS_PACKAGE_APP_CURL
 *    or SOLAR_OS_PACKAGE_APP_CLOCK are (likely under the existing "net"
 *    package group, since it needs Wi-Fi + solar_os_http_client). This is
 *    build-system plumbing outside this .c file — needs a maintainer's
 *    confirmation of where new app packages get declared.
 *
 * 3. Flavor — add "weather" wherever curl/clock currently sit in a
 *    flavor's [packages] section (e.g. flavors/full.toml already has
 *    net = true, which likely already covers curl; confirm whether a
 *    new flag is needed or if grouping under the same package is fine).
 *
 * 4. CMakeLists.txt — register solar_os_weather_app.c/.h alongside the
 *    other apps/ sources.
 * ------------------------------------------------------------------ */
