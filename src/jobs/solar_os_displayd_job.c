#include "solar_os_displayd_job.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "solar_os_display.h"
#include "solar_os_http_server.h"
#include "solar_os_jobs.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_power.h"
#include "solar_os_queue.h"
#include "solar_os_sessions.h"
#include "solar_os_shell_io.h"
#include "solar_os_virtual_display.h"

#define DISPLAYD_ROUTE_OWNER "job:displayd"
#define DISPLAYD_DEFAULT_TARGET "display0"
#define DISPLAYD_VIRTUAL_TARGET "web0"
#define DISPLAYD_INPUT_QUEUE_LEN 64U
#define DISPLAYD_INPUT_BODY_MAX 32U
#define DISPLAYD_TICK_INTERVAL_MS 25U
#define DISPLAYD_TICK_DEADLINE_MS 50U

static const char *TAG = "solar_os_displayd";

typedef struct {
    bool running;
    bool draining;
    bool detached_session;
    uint8_t session_id;
    solar_os_virtual_display_t *virtual_display;
    char target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char frame_uri[SOLAR_OS_HTTP_ROUTE_URI_MAX];
    char raw_uri[SOLAR_OS_HTTP_ROUTE_URI_MAX];
    char rgb_uri[SOLAR_OS_HTTP_ROUTE_URI_MAX];
    char input_uri[SOLAR_OS_HTTP_ROUTE_URI_MAX];
    uint16_t width;
    uint16_t height;
    uint16_t native_width;
    uint16_t native_height;
    uint16_t native_stride;
    solar_os_display_rotation_t rotation;
    bool black_is_one;
    bool color;
    uint8_t *raw_response;
    size_t raw_response_size;
    QueueHandle_t input;
    uint32_t frame_requests;
    uint32_t input_requests;
    uint32_t dropped_input;
} displayd_state_t;

static displayd_state_t displayd;
static portMUX_TYPE displayd_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static void displayd_stats_add(uint32_t *counter, uint32_t value)
{
    portENTER_CRITICAL(&displayd_stats_lock);
    *counter += value;
    portEXIT_CRITICAL(&displayd_stats_lock);
}

static void displayd_stats_snapshot(uint32_t *frame_requests,
                                    uint32_t *input_requests,
                                    uint32_t *dropped_input)
{
    portENTER_CRITICAL(&displayd_stats_lock);
    *frame_requests = displayd.frame_requests;
    *input_requests = displayd.input_requests;
    *dropped_input = displayd.dropped_input;
    portEXIT_CRITICAL(&displayd_stats_lock);
}

static esp_err_t displayd_release_resources(void)
{
    esp_err_t ret = ESP_OK;
    if (displayd.detached_session && displayd.target[0] != '\0') {
        const esp_err_t close_err = solar_os_sessions_close_display(displayd.target);
        if (close_err != ESP_OK && close_err != ESP_ERR_NOT_FOUND) {
            ret = close_err;
        } else {
            displayd.detached_session = false;
        }
    }
    if (displayd.target[0] != '\0') {
        solar_os_display_stop_frame_export(displayd.target);
    }

    solar_os_memory_free(displayd.raw_response);
    displayd.raw_response = NULL;
    displayd.raw_response_size = 0;
    solar_os_queue_delete(displayd.input);
    displayd.input = NULL;

    if (displayd.virtual_display != NULL) {
        const esp_err_t destroy_err =
            solar_os_virtual_display_destroy(displayd.virtual_display);
        if (destroy_err != ESP_OK) {
            return destroy_err;
        }
        displayd.virtual_display = NULL;
    }
    if (ret == ESP_OK) {
        memset(&displayd, 0, sizeof(displayd));
    }
    return ret;
}

static const char display_page[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>SolarOS display</title><style>"
    ":root{color-scheme:light dark;font:15px system-ui,sans-serif}"
    "body{margin:0;display:grid;min-height:100vh;place-items:center;background:#20231f}"
    "main{width:min(94vw,900px);padding:20px;box-sizing:border-box}"
    "header{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:12px}"
    "h1{font-size:18px;margin:0 auto 0 0;color:#f1f3ed}"
    "input,button{font:inherit;padding:8px 10px;border-radius:5px;border:1px solid #777}"
    "input{min-width:250px}button{cursor:pointer}"
    "canvas{display:block;width:100%;height:auto;background:#fff;image-rendering:pixelated;"
    "outline:1px solid #62675e;box-shadow:0 8px 30px #0008}"
    "canvas:focus{outline:3px solid #d6e87a}"
    "#status{color:#cdd2c7;margin-top:10px;min-height:1.3em}"
    "</style></head><body><main><header><h1>SolarOS display</h1>"
    "<input id=token type=password inputmode=numeric maxlength=6 autocomplete=off "
    "placeholder=\"6-digit code\">"
    "<button id=connect>Connect</button></header>"
    "<canvas id=screen tabindex=0 aria-label=\"SolarOS remote display\"></canvas>"
    "<div id=status>Enter the token printed by <code>job start displayd</code>.</div>"
    "</main><script>"
    "const tokenEl=document.querySelector('#token'),button=document.querySelector('#connect'),"
    "canvas=document.querySelector('#screen'),statusEl=document.querySelector('#status'),"
    "ctx=canvas.getContext('2d');"
    "let token='',frameUrl='',inputUrl='',displayMeta=null,etag='',timer=0,image=null,"
    "inputQueue=[],inputSending=false;"
    "const headers=()=>({Authorization:'Bearer '+token});"
    "function status(s){statusEl.textContent=s}"
    "function raw(bytes,m){"
    "const w=m.width,h=m.height,nw=m.native_width,nh=m.native_height,s=m.native_stride;"
    "if(!image||image.width!==w||image.height!==h){"
    "canvas.width=w;canvas.height=h;image=ctx.createImageData(w,h)}"
    "const d=image.data,b=m.black_is_one;d.fill(255);"
    "function mark(x,y,nx,ny){const i=(y*w+x)*4;"
    "if(m.pixel_format==='rgb565'){const p=(ny*nw+nx)*2,c=(bytes[p]<<8)|bytes[p+1];"
    "d[i]=Math.round((c>>>11)*255/31);d[i+1]=Math.round(((c>>>5)&63)*255/63);"
    "d[i+2]=Math.round((c&31)*255/31);return}"
    "const set=(bytes[(ny>>3)*s+nx]&(1<<(ny&7)))!==0;"
    "if(b?set:!set)d[i]=d[i+1]=d[i+2]=0}"
    "let x,y,nx,ny;"
    "if(m.rotation===1){for(y=0;y<h;y++){nx=nw-1-y;"
    "for(x=0;x<w;x++){ny=x;mark(x,y,nx,ny)}}}"
    "else if(m.rotation===2){for(y=0;y<h;y++){ny=nh-1-y;"
    "for(x=0;x<w;x++){nx=nw-1-x;mark(x,y,nx,ny)}}}"
    "else if(m.rotation===3){for(y=0;y<h;y++){nx=y;"
    "for(x=0;x<w;x++){ny=nh-1-x;mark(x,y,nx,ny)}}}"
    "else{for(y=0;y<h;y++){ny=y;"
    "for(x=0;x<w;x++){nx=x;mark(x,y,nx,ny)}}}"
    "ctx.putImageData(image,0,0)}"
    "async function poll(){clearTimeout(timer);if(!token||!frameUrl)return;"
    "try{const h=headers();if(etag)h['If-None-Match']=etag;"
    "const r=await fetch(frameUrl,{headers:h,cache:'no-store'});"
    "if(r.status===401)throw Error('authentication failed');"
    "if(r.status===200){etag=r.headers.get('ETag')||'';"
    "raw(new Uint8Array(await r.arrayBuffer()),displayMeta);"
    "status('Connected - click the display to send keys')}"
    "else if(r.status!==304&&r.status!==503)throw Error('frame HTTP '+r.status)"
    "}catch(e){status(e.message)}timer=setTimeout(poll,50)}"
    "async function connect(){token=tokenEl.value.trim();etag='';"
    "if(!/^\\d{6}$/.test(token)){status('Enter the 6-digit code');return}"
    "try{const r=await fetch('/api/displays',{headers:headers(),cache:'no-store'});"
    "if(!r.ok)throw Error(r.status===401?'authentication failed':'display list HTTP '+r.status);"
    "const a=await r.json();if(!a.displays.length)throw Error('no exported display');"
    "displayMeta=a.displays[0];frameUrl=displayMeta.pixel_format==='rgb565'?"
    "displayMeta.frame_rgb565:displayMeta.frame_raw;inputUrl=displayMeta.input;"
    "canvas.focus();poll()}"
    "catch(e){status(e.message)}}"
    "button.onclick=connect;tokenEl.addEventListener('keydown',e=>{if(e.key==='Enter')connect()});"
    "const keymap={ArrowUp:128,ArrowDown:129,ArrowLeft:130,ArrowRight:131,"
    "PageUp:132,PageDown:133,Home:148,End:149,Delete:150,Escape:27,"
    "Backspace:8,Enter:13,Tab:9};"
    "async function flushInput(){if(inputSending||!inputQueue.length)return;inputSending=true;"
    "try{while(inputQueue.length){const batch=inputQueue.splice(0,32);"
    "const r=await fetch(inputUrl,"
    "{method:'POST',headers:{...headers(),'Content-Type':'application/octet-stream'},"
    "body:new Uint8Array(batch)});if(!r.ok)throw Error('input HTTP '+r.status)}}"
    "catch(x){status(x.message)}finally{inputSending=false;if(inputQueue.length)flushInput()}}"
    "function sendKey(e){let k=keymap[e.key];"
    "if(e.ctrlKey&&e.key===']')k=146;"
    "else if(k===undefined&&e.key.length===1&&!e.metaKey&&!e.altKey){"
    "const c=e.key.charCodeAt(0);if(c<128)k=e.ctrlKey?(c&31):c}"
    "if(k===undefined)return;e.preventDefault();inputQueue.push(k);flushInput()}"
    "canvas.addEventListener('pointerdown',()=>canvas.focus());"
    "document.addEventListener('keydown',e=>{"
    "if(!token||document.activeElement===tokenEl||document.activeElement===button)return;"
    "sendKey(e)});"
    "</script></body></html>";

static esp_err_t displayd_send_503(httpd_req_t *req, const char *message)
{
    (void)httpd_resp_set_status(req, "503 Service Unavailable");
    (void)httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t displayd_page_handler(httpd_req_t *req, void *user)
{
    (void)user;
    (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_set_hdr(req,
                             "Content-Security-Policy",
                             "default-src 'self'; script-src 'unsafe-inline'; "
                             "style-src 'unsafe-inline'; connect-src 'self'");
    (void)httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, display_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t displayd_list_handler(httpd_req_t *req, void *user)
{
    displayd_state_t *state = (displayd_state_t *)user;
    solar_os_display_target_t target;
    if (state == NULL || !solar_os_display_find_target(state->target, &target)) {
        return httpd_resp_send_404(req);
    }

    solar_os_display_frame_t frame;
    uint32_t frame_id = 0;
    if (solar_os_display_acquire_frame(state->target, &frame) == ESP_OK) {
        frame_id = frame.frame_id;
        solar_os_display_release_frame(&frame);
    }

    char body[768];
    const int len = snprintf(body,
                             sizeof(body),
                             "{\"displays\":[{\"name\":\"%s\",\"source\":\"%s\","
                             "\"driver\":\"%s\",\"width\":%u,\"height\":%u,"
                             "\"native_width\":%u,\"native_height\":%u,"
                             "\"native_stride\":%u,\"rotation\":%u,"
                             "\"black_is_one\":%s,\"frame_id\":%u,"
                             "\"frame\":\"%s\",\"frame_raw\":\"%s\","
                             "\"pixel_format\":\"%s\",\"frame_rgb565\":\"%s\","
                             "\"input\":\"%s\"}]}",
                             target.name,
                             target.source,
                             target.driver,
                             (unsigned)state->width,
                             (unsigned)state->height,
                             (unsigned)state->native_width,
                             (unsigned)state->native_height,
                             (unsigned)state->native_stride,
                             (unsigned)state->rotation,
                             state->black_is_one ? "true" : "false",
                             (unsigned)frame_id,
                             state->frame_uri,
                             state->raw_uri,
                             state->color ? "rgb565" : "mono1",
                             state->color ? state->rgb_uri : "",
                             state->input_uri);
    if (len < 0 || (size_t)len >= sizeof(body)) {
        return httpd_resp_send_err(req,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "display metadata too large");
    }
    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

static bool displayd_frame_pixel_black(const solar_os_display_frame_t *frame,
                                       uint16_t x,
                                       uint16_t y)
{
    uint16_t native_x = x;
    uint16_t native_y = y;
    switch (frame->rotation) {
    case SOLAR_OS_DISPLAY_ROTATION_90:
        native_x = (uint16_t)(frame->native_width - 1U - y);
        native_y = x;
        break;
    case SOLAR_OS_DISPLAY_ROTATION_180:
        native_x = (uint16_t)(frame->native_width - 1U - x);
        native_y = (uint16_t)(frame->native_height - 1U - y);
        break;
    case SOLAR_OS_DISPLAY_ROTATION_270:
        native_x = y;
        native_y = (uint16_t)(frame->native_height - 1U - x);
        break;
    case SOLAR_OS_DISPLAY_ROTATION_0:
    default:
        break;
    }

    if (native_x >= frame->native_width || native_y >= frame->native_height) {
        return false;
    }
    const size_t index =
        (size_t)(native_y / 8U) * frame->native_stride + native_x;
    if (index >= frame->data_size) {
        return false;
    }
    const bool set = (frame->data[index] & (uint8_t)(1U << (native_y & 7U))) != 0;
    return frame->black_is_one ? set : !set;
}

static esp_err_t displayd_frame_handler(httpd_req_t *req, void *user)
{
    displayd_state_t *state = (displayd_state_t *)user;
    if (state == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_display_frame_t frame;
    const esp_err_t acquire_err = solar_os_display_acquire_frame(state->target, &frame);
    if (acquire_err == ESP_ERR_TIMEOUT) {
        return displayd_send_503(req, "frame is being published");
    }
    if (acquire_err != ESP_OK) {
        return displayd_send_503(req, "display frame unavailable");
    }
    displayd_stats_add(&state->frame_requests, 1U);

    char etag[25];
    snprintf(etag,
             sizeof(etag),
             "\"%08x-%08x\"",
             (unsigned)frame.target_generation,
             (unsigned)frame.frame_id);
    char request_etag[sizeof(etag)];
    if (httpd_req_get_hdr_value_len(req, "If-None-Match") < sizeof(request_etag) &&
        httpd_req_get_hdr_value_str(req,
                                    "If-None-Match",
                                    request_etag,
                                    sizeof(request_etag)) == ESP_OK &&
        strcmp(request_etag, etag) == 0) {
        solar_os_display_release_frame(&frame);
        (void)httpd_resp_set_status(req, "304 Not Modified");
        (void)httpd_resp_set_hdr(req, "ETag", etag);
        return httpd_resp_send(req, NULL, 0);
    }

    const size_t row_size = ((size_t)frame.width + 7U) / 8U;
    const size_t desired_chunk_size = 1024U;
    const size_t chunk_size = row_size >= desired_chunk_size ?
        row_size :
        desired_chunk_size - desired_chunk_size % row_size;
    uint8_t *chunk = solar_os_memory_alloc(chunk_size,
                                           SOLAR_OS_MEMORY_TRANSIENT,
                                           "displayd.pbm");
    if (chunk == NULL) {
        solar_os_display_release_frame(&frame);
        return httpd_resp_send_err(req,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "out of memory");
    }

    (void)httpd_resp_set_type(req, "image/x-portable-bitmap");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    (void)httpd_resp_set_hdr(req, "ETag", etag);
    char header[32];
    const int header_len = snprintf(header,
                                    sizeof(header),
                                    "P4\n%u %u\n",
                                    (unsigned)frame.width,
                                    (unsigned)frame.height);
    esp_err_t ret = header_len > 0 && (size_t)header_len < sizeof(header) ?
        httpd_resp_send_chunk(req, header, (size_t)header_len) :
        ESP_ERR_INVALID_SIZE;

    size_t chunk_used = 0;
    for (uint16_t y = 0; ret == ESP_OK && y < frame.height; y++) {
        if (chunk_used + row_size > chunk_size) {
            ret = httpd_resp_send_chunk(req, (const char *)chunk, chunk_used);
            chunk_used = 0;
            if (ret != ESP_OK) {
                break;
            }
        }
        uint8_t *row = chunk + chunk_used;
        memset(row, 0, row_size);
        for (uint16_t x = 0; x < frame.width; x++) {
            if (displayd_frame_pixel_black(&frame, x, y)) {
                row[x / 8U] |= (uint8_t)(0x80U >> (x & 7U));
            }
        }
        chunk_used += row_size;
    }
    if (ret == ESP_OK && chunk_used > 0) {
        ret = httpd_resp_send_chunk(req, (const char *)chunk, chunk_used);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }

    solar_os_memory_free(chunk);
    solar_os_display_release_frame(&frame);
    return ret;
}

static esp_err_t displayd_send_raw_frame(httpd_req_t *req, void *user, bool color)
{
    displayd_state_t *state = (displayd_state_t *)user;
    if (state == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_display_frame_t frame;
    const esp_err_t acquire_err = solar_os_display_acquire_frame(state->target, &frame);
    if (acquire_err == ESP_ERR_TIMEOUT) {
        return displayd_send_503(req, "frame is being published");
    }
    if (acquire_err != ESP_OK) {
        return displayd_send_503(req, "display frame unavailable");
    }
    if (color && frame.rgb565_data == NULL) {
        solar_os_display_release_frame(&frame);
        return httpd_resp_send_404(req);
    }
    displayd_stats_add(&state->frame_requests, 1U);

    char etag[25];
    snprintf(etag,
             sizeof(etag),
             "\"%08x-%08x\"",
             (unsigned)frame.target_generation,
             (unsigned)frame.frame_id);
    char request_etag[sizeof(etag)];
    if (httpd_req_get_hdr_value_len(req, "If-None-Match") < sizeof(request_etag) &&
        httpd_req_get_hdr_value_str(req,
                                    "If-None-Match",
                                    request_etag,
                                    sizeof(request_etag)) == ESP_OK &&
        strcmp(request_etag, etag) == 0) {
        solar_os_display_release_frame(&frame);
        (void)httpd_resp_set_status(req, "304 Not Modified");
        (void)httpd_resp_set_hdr(req, "ETag", etag);
        return httpd_resp_send(req, NULL, 0);
    }

    const size_t frame_size = color ? frame.rgb565_size : frame.data_size;
    const uint8_t *frame_data = color ? frame.rgb565_data : frame.data;
    if (state->raw_response == NULL || state->raw_response_size < frame_size) {
        solar_os_display_release_frame(&frame);
        return httpd_resp_send_err(req,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "raw response buffer unavailable");
    }
    memcpy(state->raw_response, frame_data, frame_size);
    solar_os_display_release_frame(&frame);

    (void)httpd_resp_set_type(req, "application/octet-stream");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    (void)httpd_resp_set_hdr(req, "ETag", etag);
    return httpd_resp_send(req, (const char *)state->raw_response, frame_size);
}

static esp_err_t displayd_raw_handler(httpd_req_t *req, void *user)
{
    return displayd_send_raw_frame(req, user, false);
}

static esp_err_t displayd_rgb_handler(httpd_req_t *req, void *user)
{
    return displayd_send_raw_frame(req, user, true);
}

static esp_err_t displayd_input_handler(httpd_req_t *req, void *user)
{
    displayd_state_t *state = (displayd_state_t *)user;
    if (state == NULL || state->input == NULL) {
        return displayd_send_503(req, "input unavailable");
    }
    if (req->content_len <= 0 || req->content_len > DISPLAYD_INPUT_BODY_MAX) {
        return httpd_resp_send_err(req,
                                   HTTPD_400_BAD_REQUEST,
                                   "input must contain 1..32 bytes");
    }

    uint8_t body[DISPLAYD_INPUT_BODY_MAX];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int len = httpd_req_recv(req,
                                       (char *)body + received,
                                       (size_t)req->content_len - received);
        if (len <= 0) {
            return httpd_resp_send_err(req,
                                       HTTPD_400_BAD_REQUEST,
                                       "incomplete input");
        }
        received += (size_t)len;
    }

    size_t queued = 0;
    for (; queued < received; queued++) {
        if (xQueueSend(state->input, &body[queued], 0) != pdTRUE) {
            displayd_stats_add(&state->dropped_input,
                               (uint32_t)(received - queued));
            break;
        }
    }
    memset(body, 0, sizeof(body));
    displayd_stats_add(&state->input_requests, 1U);

    if (queued != received) {
        return displayd_send_503(req, "input queue full");
    }
    (void)httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t displayd_register_routes(void)
{
    const solar_os_http_route_t routes[] = {
        {
            .owner = DISPLAYD_ROUTE_OWNER,
            .uri = "/display",
            .method = HTTP_GET,
            .auth = SOLAR_OS_HTTP_AUTH_PUBLIC,
            .handler = displayd_page_handler,
            .user = &displayd,
        },
        {
            .owner = DISPLAYD_ROUTE_OWNER,
            .uri = "/display/",
            .method = HTTP_GET,
            .auth = SOLAR_OS_HTTP_AUTH_PUBLIC,
            .handler = displayd_page_handler,
            .user = &displayd,
        },
        {
            .owner = DISPLAYD_ROUTE_OWNER,
            .uri = "/api/displays",
            .method = HTTP_GET,
            .auth = SOLAR_OS_HTTP_AUTH_VIEW,
            .handler = displayd_list_handler,
            .user = &displayd,
        },
        {
            .owner = DISPLAYD_ROUTE_OWNER,
            .uri = displayd.frame_uri,
            .method = HTTP_GET,
            .auth = SOLAR_OS_HTTP_AUTH_VIEW,
            .handler = displayd_frame_handler,
            .user = &displayd,
        },
        {
            .owner = DISPLAYD_ROUTE_OWNER,
            .uri = displayd.raw_uri,
            .method = HTTP_GET,
            .auth = SOLAR_OS_HTTP_AUTH_VIEW,
            .handler = displayd_raw_handler,
            .user = &displayd,
        },
        {
            .owner = DISPLAYD_ROUTE_OWNER,
            .uri = displayd.rgb_uri,
            .method = HTTP_GET,
            .auth = SOLAR_OS_HTTP_AUTH_VIEW,
            .handler = displayd_rgb_handler,
            .user = &displayd,
        },
        {
            .owner = DISPLAYD_ROUTE_OWNER,
            .uri = displayd.input_uri,
            .method = HTTP_POST,
            .auth = SOLAR_OS_HTTP_AUTH_CONTROL,
            .handler = displayd_input_handler,
            .user = &displayd,
        },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t err = solar_os_http_server_register_route(&routes[i]);
        if (err != ESP_OK) {
            (void)solar_os_http_server_unregister_owner(DISPLAYD_ROUTE_OWNER);
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t displayd_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    if (argc < 1 || argc > 2 || argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (displayd.draining) {
        const esp_err_t drain_err =
            solar_os_http_server_unregister_owner(DISPLAYD_ROUTE_OWNER);
        if (drain_err != ESP_OK && drain_err != ESP_ERR_NOT_FOUND) {
            return drain_err;
        }
        const esp_err_t cleanup_err = displayd_release_resources();
        if (cleanup_err != ESP_OK) {
            return cleanup_err;
        }
    }

    memset(&displayd, 0, sizeof(displayd));
    const char *target_name = argc == 2 ? argv[1] : DISPLAYD_DEFAULT_TARGET;
    solar_os_display_target_t target;
    if (argc == 1 && !solar_os_display_find_target(target_name, &target)) {
        target_name = DISPLAYD_VIRTUAL_TARGET;
    }
    if (strcmp(target_name, DISPLAYD_VIRTUAL_TARGET) == 0 &&
        !solar_os_display_find_target(target_name, &target)) {
        esp_err_t create_err =
            solar_os_virtual_display_create(target_name, &displayd.virtual_display);
        if (create_err != ESP_OK) {
            return create_err;
        }
    }
    if (!solar_os_display_find_target(target_name, &target)) {
        (void)displayd_release_resources();
        return ESP_ERR_NOT_FOUND;
    }
    if (!target.ready || target.u8g2 == NULL) {
        (void)displayd_release_resources();
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(displayd.target, target.name, sizeof(displayd.target));
    if (snprintf(displayd.frame_uri,
                 sizeof(displayd.frame_uri),
                 "/api/displays/%s/frame.pbm",
                 displayd.target) >= (int)sizeof(displayd.frame_uri) ||
        snprintf(displayd.raw_uri,
                 sizeof(displayd.raw_uri),
                 "/api/displays/%s/frame.raw",
                 displayd.target) >= (int)sizeof(displayd.raw_uri) ||
        snprintf(displayd.rgb_uri,
                 sizeof(displayd.rgb_uri),
                 "/api/displays/%s/frame.rgb565",
                 displayd.target) >= (int)sizeof(displayd.rgb_uri) ||
        snprintf(displayd.input_uri,
                 sizeof(displayd.input_uri),
                 "/api/displays/%s/input",
                 displayd.target) >= (int)sizeof(displayd.input_uri)) {
        (void)displayd_release_resources();
        return ESP_ERR_INVALID_SIZE;
    }

    displayd.input = solar_os_queue_create(DISPLAYD_INPUT_QUEUE_LEN, sizeof(uint8_t));
    if (displayd.input == NULL) {
        (void)displayd_release_resources();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    uint8_t active_session = 0;
    const bool target_has_session =
        solar_os_sessions_active_for_display(displayd.target, &active_session);
    if (!target_has_session && target.owner[0] == '\0') {
        char busy_owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
        err = solar_os_sessions_create_detached_display_shell(displayd.target,
                                                              &displayd.session_id,
                                                              busy_owner,
                                                              sizeof(busy_owner));
        if (err == ESP_OK) {
            displayd.detached_session = true;
        }
    }
    if (err == ESP_OK) {
        err = solar_os_display_start_frame_export(displayd.target);
    }
    solar_os_display_frame_t initial_frame;
    if (err == ESP_OK) {
        err = solar_os_display_acquire_frame(displayd.target, &initial_frame);
        if (err == ESP_OK) {
            displayd.width = initial_frame.width;
            displayd.height = initial_frame.height;
            displayd.native_width = initial_frame.native_width;
            displayd.native_height = initial_frame.native_height;
            displayd.native_stride = initial_frame.native_stride;
            displayd.rotation = initial_frame.rotation;
            displayd.black_is_one = initial_frame.black_is_one;
            displayd.color = initial_frame.rgb565_data != NULL;
            displayd.raw_response_size = initial_frame.rgb565_size > initial_frame.data_size ?
                initial_frame.rgb565_size : initial_frame.data_size;
            solar_os_display_release_frame(&initial_frame);
            displayd.raw_response =
                solar_os_memory_alloc(displayd.raw_response_size,
                                      SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                      "displayd.raw");
            if (displayd.raw_response == NULL) {
                err = ESP_ERR_NO_MEM;
            }
        }
    }
    if (err == ESP_OK) {
        err = displayd_register_routes();
    }
    if (err != ESP_OK) {
        const esp_err_t cleanup_err = displayd_release_resources();
        if (cleanup_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "start cleanup failed: %s",
                          esp_err_to_name(cleanup_err));
        }
        return err;
    }

    displayd.running = true;
    (void)solar_os_jobs_note_resource(solar_os_displayd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      displayd.target,
                                      displayd.detached_session ? "virtual" : "mirror");
    char port[16];
    snprintf(port, sizeof(port), "tcp:%u", (unsigned)solar_os_http_server_port());
    (void)solar_os_jobs_note_resource(solar_os_displayd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      port,
                                      "listen");

    char token[SOLAR_OS_HTTP_BEARER_TOKEN_MAX];
    solar_os_shell_io_t *io = ctx != NULL ? solar_os_context_shell_io(ctx) : NULL;
    if (io != NULL && solar_os_http_server_get_bearer_token(token, sizeof(token))) {
        solar_os_shell_io_printf(io,
                                 "displayd: open http://<device>:%u/display\n"
                                 "displayd access code: %s\n",
                                 (unsigned)solar_os_http_server_port(),
                                 token);
        if (displayd.detached_session) {
            solar_os_shell_io_printf(io,
                                     "displayd: %s shell is session %u\n",
                                     displayd.target,
                                     (unsigned)displayd.session_id);
        }
        solar_os_shell_io_flush(io);
        memset(token, 0, sizeof(token));
    }

    SOLAR_OS_LOGI(TAG,
                  "%s %s on port %u",
                  displayd.detached_session ? "serving" : "mirroring",
                  displayd.target,
                  (unsigned)solar_os_http_server_port());
    return ESP_OK;
}

static void displayd_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    displayd.running = false;
    const esp_err_t unregister_err =
        solar_os_http_server_unregister_owner(DISPLAYD_ROUTE_OWNER);
    if (unregister_err != ESP_OK && unregister_err != ESP_ERR_NOT_FOUND) {
        SOLAR_OS_LOGW(TAG, "route unregister failed: %s", esp_err_to_name(unregister_err));
    }
    if (unregister_err == ESP_ERR_TIMEOUT) {
        displayd.draining = true;
        return;
    }
    uint32_t frame_requests = 0;
    uint32_t input_requests = 0;
    uint32_t dropped_input = 0;
    displayd_stats_snapshot(&frame_requests, &input_requests, &dropped_input);
    const esp_err_t cleanup_err = displayd_release_resources();
    if (cleanup_err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "cleanup failed: %s", esp_err_to_name(cleanup_err));
        displayd.draining = true;
        return;
    }
    SOLAR_OS_LOGI(TAG,
                  "stopped: frames=%u input=%u dropped=%u",
                  (unsigned)frame_requests,
                  (unsigned)input_requests,
                  (unsigned)dropped_input);
}

static bool displayd_job_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (!displayd.running ||
        displayd.input == NULL ||
        event == NULL ||
        event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }

    uint8_t ch = 0;
    size_t drained = 0;
    while (drained++ < DISPLAYD_INPUT_BODY_MAX &&
           xQueueReceive(displayd.input, &ch, 0) == pdTRUE) {
        const solar_os_event_t input_event = {
            .type = SOLAR_OS_EVENT_CHAR,
            .data.ch = (char)ch,
        };
        solar_os_power_note_activity(event->data.tick_ms);
        uint8_t active_session = 0;
        bool dispatched =
            solar_os_sessions_active_for_display(displayd.target, &active_session) &&
            solar_os_sessions_dispatch_session_event(active_session, &input_event);
        if (!dispatched) {
            displayd_stats_add(&displayd.dropped_input, 1U);
        }
    }
    return false;
}

const solar_os_job_t solar_os_displayd_job = {
    .name = "displayd",
    .summary = "authenticated HTTP display",
    .start = displayd_job_start,
    .stop = displayd_job_stop,
    .event = displayd_job_event,
    .worker_stack_bytes = 0,
    .tick_interval_ms = DISPLAYD_TICK_INTERVAL_MS,
    .tick_deadline_ms = DISPLAYD_TICK_DEADLINE_MS,
};
