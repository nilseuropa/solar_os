#include "solar_os_remote_job.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_remote_input.h"
#include "solar_os_remote_screen.h"

/*
 * Remote screen share: a small standalone HTTP server (its own
 * esp_http_server instance, so it can run alongside the httpd file
 * server job) that serves the live display as a 1bpp BMP plus a
 * self-contained HTML page that refreshes it and forwards browser
 * keystrokes into SolarOS's input dispatch. No authentication -- LAN
 * use only, same trust model as the httpd/chatd jobs.
 */
#define REMOTE_JOB_DEFAULT_PORT 8080U
#define REMOTE_JOB_STACK_SIZE 6144
/* Distinct from the httpd job's control socket (esp_http_server
 * default 32768) so both servers can coexist. */
#define REMOTE_JOB_CTRL_PORT 32780
#define REMOTE_BMP_HEADER_SIZE 62U

static const char *TAG = "solar_os_remote";

typedef struct {
    bool running;
    httpd_handle_t server;
    uint16_t port;
    uint32_t frame_count;
    uint32_t key_count;
    esp_err_t last_error;
} remote_job_state_t;

static remote_job_state_t remote_job = {
    .last_error = ESP_OK,
};

/* Self-contained page: screen image auto-refreshing (~4 fps, next
 * request only after the previous one finished, so a slow link
 * degrades smoothly instead of piling up), full-keyboard capture, and
 * a button row for keys a phone keyboard doesn't have. Key codes are
 * SolarOS's own (solar_os_keys.h) for specials, plain ASCII otherwise. */
static const char remote_page[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>SolarOS remote</title><style>"
    "body{background:#111;color:#ccc;font-family:monospace;margin:0;padding:12px;"
    "display:flex;flex-direction:column;align-items:center;gap:10px}"
    "img{image-rendering:pixelated;width:min(96vw,640px);border:1px solid #444}"
    "#keys{display:flex;flex-wrap:wrap;gap:6px;justify-content:center}"
    "button{background:#222;color:#ccc;border:1px solid #555;border-radius:6px;"
    "padding:8px 12px;font-family:monospace;font-size:14px}"
    "button:active{background:#444}"
    "input{background:#222;color:#ccc;border:1px solid #555;border-radius:6px;"
    "padding:8px;width:min(90vw,400px);font-family:monospace}"
    "#st{color:#777;font-size:12px}"
    "</style></head><body>"
    "<img id=\"scr\" src=\"/screen.bmp\" alt=\"screen\">"
    "<div id=\"keys\">"
    "<button data-k=\"27\">Esc</button>"
    "<button data-k=\"9\">Tab</button>"
    "<button data-k=\"128\">&#8593;</button>"
    "<button data-k=\"129\">&#8595;</button>"
    "<button data-k=\"130\">&#8592;</button>"
    "<button data-k=\"131\">&#8594;</button>"
    "<button data-k=\"13\">Enter</button>"
    "<button data-k=\"8\">Bksp</button>"
    "<button data-k=\"32\">Space</button>"
    "<button data-k=\"146\">EXIT</button>"
    "</div>"
    "<input id=\"txt\" placeholder=\"type here (phone) or just type (desktop)\" autocomplete=\"off\">"
    "<div id=\"st\"></div>"
    "<script>"
    "var img=document.getElementById('scr'),st=document.getElementById('st'),"
    "txt=document.getElementById('txt'),sending=Promise.resolve();"
    "function refresh(){var pre=new Image();"
    "pre.onload=function(){img.src=pre.src;st.textContent='';setTimeout(refresh,250);};"
    "pre.onerror=function(){st.textContent='connection lost, retrying...';setTimeout(refresh,1500);};"
    "pre.src='/screen.bmp?'+Date.now();}"
    "refresh();"
    "function send(code){sending=sending.then(function(){"
    "return fetch('/key?c='+code).catch(function(){});});}"
    "var special={'Enter':13,'Backspace':8,'Tab':9,'Escape':27,"
    "'ArrowUp':128,'ArrowDown':129,'ArrowLeft':130,'ArrowRight':131,"
    "'PageUp':132,'PageDown':133,'Home':148,'End':149,'Delete':150};"
    "document.addEventListener('keydown',function(e){"
    "if(e.target===txt&&e.key.length===1)return;"
    "var code=null;"
    "if(special[e.key]!==undefined)code=special[e.key];"
    "else if(e.key.length===1&&e.key.charCodeAt(0)<127)code=e.key.charCodeAt(0);"
    "if(code!==null){e.preventDefault();send(code);}});"
    "txt.addEventListener('input',function(){"
    "var v=txt.value;txt.value='';"
    "for(var i=0;i<v.length;i++){var c=v.charCodeAt(i);if(c<127)send(c);}});"
    "document.querySelectorAll('#keys button').forEach(function(b){"
    "b.addEventListener('click',function(){send(parseInt(b.dataset.k,10));});});"
    "</script></body></html>";

static void remote_bmp_write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)((value >> 24) & 0xffU);
}

/* 1bpp bottom-up BMP with a 2-entry palette: 62-byte header, rows
 * padded to 4 bytes. ~10KB per frame at 320x240 -- small enough that
 * no compression is worth the code. */
static void remote_bmp_build_header(uint8_t *header,
                                    uint16_t width,
                                    uint16_t height,
                                    uint32_t row_bytes)
{
    const uint32_t image_size = row_bytes * height;
    const uint32_t file_size = REMOTE_BMP_HEADER_SIZE + image_size;

    memset(header, 0, REMOTE_BMP_HEADER_SIZE);
    header[0] = 'B';
    header[1] = 'M';
    remote_bmp_write_u32(&header[2], file_size);
    remote_bmp_write_u32(&header[10], REMOTE_BMP_HEADER_SIZE);
    remote_bmp_write_u32(&header[14], 40);
    remote_bmp_write_u32(&header[18], width);
    remote_bmp_write_u32(&header[22], height);
    header[26] = 1; /* planes */
    header[28] = 1; /* bits per pixel */
    remote_bmp_write_u32(&header[34], image_size);
    remote_bmp_write_u32(&header[46], 2); /* palette colors */
    /* palette[0] = bit 0 = black (BGRX), palette[1] = bit 1 = white */
    header[54] = 0x00;
    header[55] = 0x00;
    header[56] = 0x00;
    header[58] = 0xff;
    header[59] = 0xff;
    header[60] = 0xff;
}

static esp_err_t remote_page_handler(httpd_req_t *req)
{
    (void)httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, remote_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t remote_screen_handler(httpd_req_t *req)
{
    uint16_t width = 0;
    uint16_t height = 0;
    esp_err_t ret = solar_os_remote_screen_get_size(&width, &height);
    if (ret != ESP_OK || width == 0 || height == 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no display");
    }

    const size_t stride = ((size_t)width + 7U) / 8U;
    const size_t row_bytes = (stride + 3U) & ~(size_t)3U; /* BMP rows pad to 4 */
    const size_t snapshot_size = stride * height;
    const size_t bmp_size = REMOTE_BMP_HEADER_SIZE + (row_bytes * height);

    uint8_t *snapshot = heap_caps_malloc(snapshot_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snapshot == NULL) {
        snapshot = heap_caps_malloc(snapshot_size, MALLOC_CAP_8BIT);
    }
    uint8_t *bmp = heap_caps_malloc(bmp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bmp == NULL) {
        bmp = heap_caps_malloc(bmp_size, MALLOC_CAP_8BIT);
    }
    if (snapshot == NULL || bmp == NULL) {
        heap_caps_free(snapshot);
        heap_caps_free(bmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    ret = solar_os_remote_screen_snapshot(snapshot, snapshot_size, &width, &height);
    if (ret != ESP_OK) {
        heap_caps_free(snapshot);
        heap_caps_free(bmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
    }

    remote_bmp_build_header(bmp, width, height, (uint32_t)row_bytes);
    uint8_t *rows = &bmp[REMOTE_BMP_HEADER_SIZE];
    memset(rows, 0, row_bytes * height);
    for (uint16_t y = 0; y < height; y++) {
        /* BMP stores rows bottom-up */
        memcpy(&rows[(size_t)(height - 1U - y) * row_bytes],
               &snapshot[(size_t)y * stride],
               stride);
    }
    heap_caps_free(snapshot);

    (void)httpd_resp_set_type(req, "image/bmp");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)bmp, (ssize_t)bmp_size);
    heap_caps_free(bmp);

    if (ret == ESP_OK) {
        remote_job.frame_count++;
    }
    return ret;
}

static esp_err_t remote_key_handler(httpd_req_t *req)
{
    char query[32];
    char value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "c", value, sizeof(value)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing c=<code>");
    }

    char *end = NULL;
    errno = 0;
    const unsigned long code = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || code == 0 || code > 0xff) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad key code");
    }

    const char ch = (char)(uint8_t)code;
    const esp_err_t ret = solar_os_remote_input_push(&ch, 1);
    if (ret != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "queue full");
    }

    remote_job.key_count++;
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t remote_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    uint16_t port = REMOTE_JOB_DEFAULT_PORT;
    if (argc == 2 && argv != NULL && argv[1] != NULL) {
        char *end = NULL;
        errno = 0;
        const unsigned long parsed = strtoul(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0 || parsed > 65535) {
            return ESP_ERR_INVALID_ARG;
        }
        port = (uint16_t)parsed;
    } else if (argc > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!solar_os_remote_screen_available()) {
        remote_job.last_error = ESP_ERR_INVALID_STATE;
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = REMOTE_JOB_CTRL_PORT;
    config.stack_size = REMOTE_JOB_STACK_SIZE;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        remote_job.last_error = ret;
        return ret;
    }

    const httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = remote_page_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t screen_uri = {
        .uri = "/screen.bmp",
        .method = HTTP_GET,
        .handler = remote_screen_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t key_uri = {
        .uri = "/key",
        .method = HTTP_GET,
        .handler = remote_key_handler,
        .user_ctx = NULL,
    };
    ret = httpd_register_uri_handler(server, &page_uri);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(server, &screen_uri);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(server, &key_uri);
    }
    if (ret != ESP_OK) {
        (void)httpd_stop(server);
        remote_job.last_error = ret;
        return ret;
    }

    remote_job.server = server;
    remote_job.port = port;
    remote_job.running = true;
    remote_job.frame_count = 0;
    remote_job.key_count = 0;
    remote_job.last_error = ESP_OK;

    char net[16];
    snprintf(net, sizeof(net), "tcp:%u", (unsigned)port);
    (void)solar_os_jobs_note_resource(solar_os_remote_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      net,
                                      "listen");

    SOLAR_OS_LOGI(TAG, "remote screen share on port %u", (unsigned)port);
    return ESP_OK;
}

static void remote_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (remote_job.server != NULL) {
        (void)httpd_stop(remote_job.server);
    }

    SOLAR_OS_LOGI(TAG,
             "stopped: frames=%u keys=%u port=%u",
             (unsigned)remote_job.frame_count,
             (unsigned)remote_job.key_count,
             (unsigned)remote_job.port);

    remote_job.server = NULL;
    remote_job.running = false;
    remote_job.last_error = ESP_OK;
}

const solar_os_job_t solar_os_remote_job = {
    .name = "remote",
    .summary = "web screen share + keyboard: job start remote [port]",
    .start = remote_job_start,
    .stop = remote_job_stop,
    .event = NULL,
};
