#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_STATE 2
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
typedef struct {
    const uint8_t *data, *rgb565_data;
    size_t data_size, rgb565_size;
    uint32_t target_generation, frame_id;
} solar_os_display_frame_t;
typedef struct {
    const char *target;
    uint8_t *raw_response;
    size_t raw_response_size;
    uint32_t frame_requests;
} displayd_state_t;
typedef struct { int status; bool cached; size_t sent; } httpd_req_t;
static uint8_t mono[] = {0x12, 0x34}, color[] = {0xf8, 0, 0x07, 0xe0};
static solar_os_display_frame_t snapshot;
static bool acquired;
static unsigned releases;
static esp_err_t acquire_error;
static esp_err_t solar_os_display_acquire_frame(const char *target, solar_os_display_frame_t *frame)
{
    assert(!strcmp(target, "display0"));
    if (acquire_error) return acquire_error;
    assert(!acquired);
    acquired = true; *frame = snapshot; return ESP_OK;
}
static void solar_os_display_release_frame(solar_os_display_frame_t *frame)
{
    assert(acquired); acquired = false; releases++;
    /* The display may immediately overwrite its published snapshot. */
    memset(mono, 0xcc, sizeof(mono)); memset(color, 0xcc, sizeof(color));
    memset(frame, 0, sizeof(*frame));
}
static void displayd_stats_add(uint32_t *value, uint32_t increment) { *value += increment; }
static esp_err_t displayd_send_503(httpd_req_t *req, const char *message)
{ (void)message; assert(!acquired); req->status = 503; return ESP_OK; }
static esp_err_t httpd_resp_send_404(httpd_req_t *req)
{ assert(!acquired); req->status = 404; return ESP_OK; }
static esp_err_t httpd_resp_send_err(httpd_req_t *req, int status, const char *message)
{ (void)message; assert(!acquired); req->status = status; return ESP_OK; }
static size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *name)
{ (void)req; (void)name; return 19; }
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *name, char *value, size_t size)
{ (void)name; snprintf(value, size, "%s", req->cached ? "\"00000007-00000009\"" : "different"); return ESP_OK; }
static esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{ req->status = atoi(status); return ESP_OK; }
static esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *name, const char *value)
{ (void)req; (void)name; (void)value; return ESP_OK; }
static esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{ (void)req; assert(!strcmp(type, "application/octet-stream")); return ESP_OK; }
static esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, size_t size)
{
    assert(!acquired);
    static const uint8_t expected_color[] = {0xf8, 0, 0x07, 0xe0};
    static const uint8_t expected_mono[] = {0x12, 0x34};
    if (size == sizeof(color)) assert(!memcmp(data, expected_color, size));
    else if (size == sizeof(mono)) assert(!memcmp(data, expected_mono, size));
    else assert(size == 0);
    req->sent = size; return ESP_OK;
}
#include "displayd_raw.inc"

static void reset(void)
{
    mono[0] = 0x12; mono[1] = 0x34;
    color[0] = 0xf8; color[1] = 0; color[2] = 0x07; color[3] = 0xe0;
    snapshot = (solar_os_display_frame_t){.data = mono, .data_size = sizeof(mono),
        .rgb565_data = color, .rgb565_size = sizeof(color), .target_generation = 7, .frame_id = 9};
    acquired = false; releases = 0; acquire_error = ESP_OK;
}
int main(void)
{
    uint8_t transmit[4];
    displayd_state_t state = {.target = "display0", .raw_response = transmit, .raw_response_size = 4};
    httpd_req_t request = {0};
    reset(); assert(displayd_rgb_handler(&request, &state) == ESP_OK);
    assert(releases == 1 && request.sent == 4);
    reset(); assert(displayd_raw_handler(&request, &state) == ESP_OK);
    assert(releases == 1 && request.sent == 2);
    reset(); request.cached = true;
    assert(displayd_rgb_handler(&request, &state) == ESP_OK);
    assert(releases == 1 && request.status == 304 && request.sent == 0);
    reset(); request.cached = false; snapshot.rgb565_data = NULL;
    assert(displayd_rgb_handler(&request, &state) == ESP_OK);
    assert(releases == 1 && request.status == 404);
    reset(); state.raw_response_size = 2;
    assert(displayd_rgb_handler(&request, &state) == ESP_OK);
    assert(releases == 1 && request.status == 500);
    reset(); acquire_error = ESP_ERR_TIMEOUT;
    assert(displayd_rgb_handler(&request, &state) == ESP_OK);
    assert(releases == 0 && request.status == 503);
    puts("displayd HTTP: exact color/mono payloads, immutable copies, conditional requests and error cleanup passed");
}
