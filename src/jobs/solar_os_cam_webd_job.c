#include "solar_os_cam_webd_job.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_camera.h"
#include "solar_os_http_server.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"

#define CAM_WEBD_ROUTE_OWNER "job:cam-webd"
#define CAM_WEBD_WORKER_STACK 4096U
#define CAM_WEBD_WORKER_PRIORITY (tskIDLE_PRIORITY + 1)
#define CAM_WEBD_DEFAULT_FPS 5U
#define CAM_WEBD_MAX_FPS 30U
#define CAM_WEBD_RETRY_MS 100U
#define CAM_WEBD_STOP_POLL_MS 50U
#define CAM_WEBD_BOUNDARY "solaros-camera-frame"
#define CAM_WEBD_CONTENT_TYPE \
    "multipart/x-mixed-replace;boundary=" CAM_WEBD_BOUNDARY

typedef struct {
    bool running;
    bool draining;
    bool stop_requested;
    bool routes_registered;
    bool stream_active;
    bool auth_required;
    uint8_t fps;
    solar_os_camera_config_t camera_config;
    solar_os_camera_owner_t camera_owner;
    TaskHandle_t worker_task;
    volatile bool worker_done;
    httpd_handle_t stream_server;
    int stream_socket;
    uint32_t stream_count;
    uint32_t frame_count;
    uint32_t capture_errors;
    uint32_t send_errors;
    uint64_t jpeg_bytes;
    esp_err_t last_error;
} cam_webd_state_t;

typedef struct {
    bool running;
    bool draining;
    bool stream_active;
    bool auth_required;
    uint8_t fps;
    solar_os_camera_config_t camera_config;
    uint32_t stream_count;
    uint32_t frame_count;
    uint32_t capture_errors;
    uint32_t send_errors;
    uint64_t jpeg_bytes;
    esp_err_t last_error;
} cam_webd_snapshot_t;

static const char *TAG = "cam_webd";
static cam_webd_state_t cam_webd = {
    .stream_socket = -1,
    .last_error = ESP_OK,
};
static portMUX_TYPE cam_webd_lock = portMUX_INITIALIZER_UNLOCKED;

static void cam_webd_stream_worker(void *arg);

static void cam_webd_snapshot(cam_webd_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&cam_webd_lock);
    *snapshot = (cam_webd_snapshot_t) {
        .running = cam_webd.running,
        .draining = cam_webd.draining,
        .stream_active = cam_webd.stream_active,
        .auth_required = cam_webd.auth_required,
        .fps = cam_webd.fps,
        .camera_config = cam_webd.camera_config,
        .stream_count = cam_webd.stream_count,
        .frame_count = cam_webd.frame_count,
        .capture_errors = cam_webd.capture_errors,
        .send_errors = cam_webd.send_errors,
        .jpeg_bytes = cam_webd.jpeg_bytes,
        .last_error = cam_webd.last_error,
    };
    portEXIT_CRITICAL(&cam_webd_lock);
}

static bool cam_webd_should_stop(void)
{
    portENTER_CRITICAL(&cam_webd_lock);
    const bool stop = !cam_webd.running || cam_webd.stop_requested;
    portEXIT_CRITICAL(&cam_webd_lock);
    return stop;
}

static void cam_webd_note_capture(esp_err_t error)
{
    portENTER_CRITICAL(&cam_webd_lock);
    if (error == ESP_OK) {
        cam_webd.frame_count++;
    } else {
        cam_webd.capture_errors++;
        cam_webd.last_error = error;
    }
    portEXIT_CRITICAL(&cam_webd_lock);
}

static void cam_webd_note_send(size_t jpeg_bytes, esp_err_t error)
{
    portENTER_CRITICAL(&cam_webd_lock);
    if (error == ESP_OK) {
        cam_webd.jpeg_bytes += jpeg_bytes;
    } else {
        cam_webd.send_errors++;
        cam_webd.last_error = error;
    }
    portEXIT_CRITICAL(&cam_webd_lock);
}

static void cam_webd_delay(uint32_t delay_ms)
{
    while (delay_ms > 0U && !cam_webd_should_stop()) {
        const uint32_t slice = delay_ms < CAM_WEBD_STOP_POLL_MS ?
            delay_ms : CAM_WEBD_STOP_POLL_MS;
        TickType_t ticks = pdMS_TO_TICKS(slice);
        if (ticks == 0) {
            ticks = 1;
        }
        vTaskDelay(ticks);
        delay_ms -= slice;
    }
}

static esp_err_t cam_webd_release_resources(void)
{
    portENTER_CRITICAL(&cam_webd_lock);
    const bool routes_registered = cam_webd.routes_registered;
    portEXIT_CRITICAL(&cam_webd_lock);

    if (routes_registered) {
        const esp_err_t unregister_error =
            solar_os_http_server_unregister_owner(CAM_WEBD_ROUTE_OWNER);
        if (unregister_error != ESP_OK && unregister_error != ESP_ERR_NOT_FOUND) {
            return unregister_error;
        }
        portENTER_CRITICAL(&cam_webd_lock);
        cam_webd.routes_registered = false;
        portEXIT_CRITICAL(&cam_webd_lock);
    }

    if (cam_webd.camera_owner.generation != 0U) {
        esp_err_t error = solar_os_camera_stop(&cam_webd.camera_owner);
        if (error != ESP_OK) {
            return error;
        }
        error = solar_os_camera_release_owner(&cam_webd.camera_owner);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

static esp_err_t cam_webd_send_503(httpd_req_t *req, const char *message)
{
    (void)httpd_resp_set_status(req, "503 Service Unavailable");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t cam_webd_status_handler(httpd_req_t *req, void *user)
{
    (void)user;
    cam_webd_snapshot_t state;
    cam_webd_snapshot(&state);
    char response[384];
    const int written = snprintf(
        response,
        sizeof(response),
        "{\"running\":%s,\"draining\":%s,\"stream_active\":%s,"
        "\"auth\":\"%s\","
        "\"frame_size\":\"%ux%u\",\"format\":\"jpeg\",\"jpeg_quality\":%u,"
        "\"fps\":%u,\"streams\":%" PRIu32 ",\"frames\":%" PRIu32 ","
        "\"jpeg_bytes\":%" PRIu64 ",\"capture_errors\":%" PRIu32 ","
        "\"send_errors\":%" PRIu32 ",\"last_error\":\"%s\"}",
        state.running ? "true" : "false",
        state.draining ? "true" : "false",
        state.stream_active ? "true" : "false",
        state.auth_required ? "required" : "none",
        (unsigned)state.camera_config.width, (unsigned)state.camera_config.height,
        (unsigned)state.camera_config.jpeg_quality,
        (unsigned)state.fps,
        state.stream_count,
        state.frame_count,
        state.jpeg_bytes,
        state.capture_errors,
        state.send_errors,
        esp_err_to_name(state.last_error));
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return httpd_resp_send_err(req,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "status response too large");
    }
    (void)httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, (ssize_t)written);
}

static esp_err_t cam_webd_snapshot_handler(httpd_req_t *req, void *user)
{
    (void)user;
    portENTER_CRITICAL(&cam_webd_lock);
    const bool available = cam_webd.running && !cam_webd.stop_requested &&
        !cam_webd.stream_active;
    portEXIT_CRITICAL(&cam_webd_lock);
    if (!available) {
        return cam_webd_send_503(req, "camera stream is busy");
    }

    const solar_os_camera_frame_t *frame = NULL;
    esp_err_t error = solar_os_camera_capture(&cam_webd.camera_owner, &frame);
    cam_webd_note_capture(error);
    if (error != ESP_OK) {
        return cam_webd_send_503(req, esp_err_to_name(error));
    }

    const size_t frame_length = frame->length;
    (void)httpd_resp_set_type(req, "image/jpeg");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    error = httpd_resp_send(req, (const char *)frame->data, frame_length);
    const esp_err_t release_error =
        solar_os_camera_release_frame(&cam_webd.camera_owner, frame);
    if (error == ESP_OK && release_error != ESP_OK) {
        error = release_error;
    }
    cam_webd_note_send(frame_length, error);
    return error;
}

static esp_err_t cam_webd_stream_handler(httpd_req_t *req, void *user)
{
    (void)user;
    portENTER_CRITICAL(&cam_webd_lock);
    const bool available = cam_webd.running && !cam_webd.stop_requested &&
        !cam_webd.stream_active && cam_webd.worker_task == NULL;
    if (available) {
        cam_webd.stream_active = true;
        cam_webd.worker_done = false;
        cam_webd.stream_server = req->handle;
        cam_webd.stream_socket = httpd_req_to_sockfd(req);
        cam_webd.stream_count++;
    }
    portEXIT_CRITICAL(&cam_webd_lock);

    if (!available) {
        (void)cam_webd_send_503(req, "camera stream already has a client");
        return ESP_FAIL;
    }

    if (solar_os_task_create_pinned_internal(
            cam_webd_stream_worker,
            "cam-webd-stream",
            CAM_WEBD_WORKER_STACK,
            req,
            CAM_WEBD_WORKER_PRIORITY,
            &cam_webd.worker_task,
            tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        portENTER_CRITICAL(&cam_webd_lock);
        cam_webd.stream_active = false;
        cam_webd.stream_server = NULL;
        cam_webd.stream_socket = -1;
        cam_webd.last_error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&cam_webd_lock);
        (void)cam_webd_send_503(req, "camera stream worker unavailable");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t cam_webd_register_routes(void)
{
    const solar_os_http_auth_t auth = cam_webd.auth_required ?
        SOLAR_OS_HTTP_AUTH_VIEW : SOLAR_OS_HTTP_AUTH_PUBLIC;
    const solar_os_http_route_t routes[] = {
        {
            .owner = CAM_WEBD_ROUTE_OWNER,
            .uri = "/api/camera",
            .method = HTTP_GET,
            .auth = auth,
            .handler = cam_webd_status_handler,
        },
        {
            .owner = CAM_WEBD_ROUTE_OWNER,
            .uri = "/camera.jpg",
            .method = HTTP_GET,
            .auth = auth,
            .handler = cam_webd_snapshot_handler,
        },
        {
            .owner = CAM_WEBD_ROUTE_OWNER,
            .uri = "/camera.mjpeg",
            .method = HTTP_GET,
            .asynchronous = true,
            .auth = auth,
            .handler = cam_webd_stream_handler,
        },
    };

    for (size_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t error = solar_os_http_server_register_route(&routes[i]);
        if (error != ESP_OK) {
            return error;
        }
        portENTER_CRITICAL(&cam_webd_lock);
        cam_webd.routes_registered = true;
        portEXIT_CRITICAL(&cam_webd_lock);
    }
    return ESP_OK;
}

static bool cam_webd_parse_fps(const char *text, uint8_t *fps)
{
    if (text == NULL || fps == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0U || value > CAM_WEBD_MAX_FPS) {
        return false;
    }
    *fps = (uint8_t)value;
    return true;
}

static bool cam_webd_parse_auth(const char *text, bool *auth_required)
{
    if (text == NULL || auth_required == NULL) {
        return false;
    }
    if (strcmp(text, "auth=none") == 0) {
        *auth_required = false;
        return true;
    }
    if (strcmp(text, "auth=required") == 0) {
        *auth_required = true;
        return true;
    }
    return false;
}

static esp_err_t cam_webd_job_start(solar_os_context_t *ctx,
                                    int argc,
                                    char **argv)
{
    if (argc < 1 || argc > 4 || argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cam_webd.draining) {
        if (cam_webd.worker_task != NULL && !cam_webd.worker_done) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t cleanup_error = cam_webd_release_resources();
        if (cleanup_error != ESP_OK) {
            return cleanup_error;
        }
    }

    solar_os_camera_config_t config = solar_os_camera_default_config();
    uint8_t fps = CAM_WEBD_DEFAULT_FPS;
    bool auth_required = false;
    bool frame_size_set = false;
    bool fps_set = false;
    bool auth_set = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL || argv[i][0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        if (!frame_size_set && solar_os_camera_parse_resolution(argv[i], &config.width, &config.height)) {
            frame_size_set = true;
        } else if (!fps_set && cam_webd_parse_fps(argv[i], &fps)) {
            fps_set = true;
        } else if (!auth_set && cam_webd_parse_auth(argv[i], &auth_required)) {
            auth_set = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }

    memset(&cam_webd, 0, sizeof(cam_webd));
    cam_webd.stream_socket = -1;
    cam_webd.fps = fps;
    cam_webd.auth_required = auth_required;
    cam_webd.camera_config = config;
    cam_webd.last_error = ESP_OK;

    esp_err_t error = solar_os_camera_acquire(CAM_WEBD_ROUTE_OWNER,
                                              &cam_webd.camera_owner);
    if (error == ESP_OK) {
        error = solar_os_camera_start(&cam_webd.camera_owner,
                                      &cam_webd.camera_config);
    }
    if (error == ESP_OK) {
        portENTER_CRITICAL(&cam_webd_lock);
        cam_webd.running = true;
        portEXIT_CRITICAL(&cam_webd_lock);
        error = cam_webd_register_routes();
    }
    if (error != ESP_OK) {
        portENTER_CRITICAL(&cam_webd_lock);
        cam_webd.running = false;
        cam_webd.last_error = error;
        portEXIT_CRITICAL(&cam_webd_lock);
        const esp_err_t cleanup_error = cam_webd_release_resources();
        if (cleanup_error != ESP_OK) {
            cam_webd.draining = true;
            SOLAR_OS_LOGW(TAG,
                          "start cleanup failed: %s",
                          esp_err_to_name(cleanup_error));
        }
        return error;
    }

    (void)solar_os_jobs_note_resource(solar_os_cam_webd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      "camera",
                                      "exclusive lease");
    char port[16];
    snprintf(port, sizeof(port), "tcp:%u", (unsigned)solar_os_http_server_port());
    (void)solar_os_jobs_note_resource(solar_os_cam_webd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      port,
                                      "MJPEG");

    solar_os_shell_io_t *io = ctx != NULL ? solar_os_context_shell_io(ctx) : NULL;
    if (io != NULL) {
        solar_os_shell_io_printf(
            io,
            "cam-webd: stream http://<device>:%u/camera.mjpeg\n"
            "cam-webd: snapshot http://<device>:%u/camera.jpg\n",
            (unsigned)solar_os_http_server_port(),
            (unsigned)solar_os_http_server_port());
        if (auth_required) {
            char token[SOLAR_OS_HTTP_BEARER_TOKEN_MAX];
            if (solar_os_http_server_get_bearer_token(token, sizeof(token))) {
                solar_os_shell_io_printf(io,
                                         "cam-webd access code: %s\n",
                                         token);
                memset(token, 0, sizeof(token));
            }
        } else {
            solar_os_shell_io_writeln(
                io,
                "cam-webd: WARNING: unauthenticated camera access");
        }
        solar_os_shell_io_flush(io);
    }

    SOLAR_OS_LOGI(TAG,
                  "started: %ux%u JPEG quality=%u fps=%u auth=%s",
                  (unsigned)config.width, (unsigned)config.height,
                  (unsigned)config.jpeg_quality,
                  (unsigned)fps,
                  auth_required ? "required" : "none");
    return ESP_OK;
}

static void cam_webd_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&cam_webd_lock);
    cam_webd.running = false;
    cam_webd.stop_requested = true;
    const TaskHandle_t worker = cam_webd.worker_task;
    httpd_handle_t server = cam_webd.stream_server;
    const int socket = cam_webd.stream_socket;
    portEXIT_CRITICAL(&cam_webd_lock);

    if (server != NULL && socket >= 0) {
        (void)httpd_sess_trigger_close(server, socket);
    }
    if (worker != NULL &&
        !solar_os_task_wait_done(worker,
                                 &cam_webd.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        portENTER_CRITICAL(&cam_webd_lock);
        cam_webd.draining = true;
        portEXIT_CRITICAL(&cam_webd_lock);
        SOLAR_OS_LOGW(TAG, "stream worker is still draining");
        return;
    }

    portENTER_CRITICAL(&cam_webd_lock);
    cam_webd.worker_task = NULL;
    portEXIT_CRITICAL(&cam_webd_lock);
    const esp_err_t cleanup_error = cam_webd_release_resources();
    if (cleanup_error != ESP_OK) {
        portENTER_CRITICAL(&cam_webd_lock);
        cam_webd.draining = true;
        cam_webd.last_error = cleanup_error;
        portEXIT_CRITICAL(&cam_webd_lock);
        SOLAR_OS_LOGW(TAG, "cleanup failed: %s", esp_err_to_name(cleanup_error));
        return;
    }

    cam_webd_snapshot_t state;
    cam_webd_snapshot(&state);
    SOLAR_OS_LOGI(TAG,
                  "stopped: streams=%" PRIu32 " frames=%" PRIu32
                  " bytes=%" PRIu64 " capture-errors=%" PRIu32
                  " send-errors=%" PRIu32,
                  state.stream_count,
                  state.frame_count,
                  state.jpeg_bytes,
                  state.capture_errors,
                  state.send_errors);
}

static bool cam_webd_job_event(solar_os_context_t *ctx,
                               const solar_os_event_t *event)
{
    (void)ctx;
    if (event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }
    portENTER_CRITICAL(&cam_webd_lock);
    if (cam_webd.worker_task != NULL && cam_webd.worker_done) {
        cam_webd.worker_task = NULL;
        cam_webd.worker_done = false;
    }
    portEXIT_CRITICAL(&cam_webd_lock);
    return false;
}

static void cam_webd_job_detail(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = ctx != NULL ? solar_os_context_shell_io(ctx) : NULL;
    if (io == NULL) {
        return;
    }
    cam_webd_snapshot_t state;
    cam_webd_snapshot(&state);
    solar_os_shell_io_printf(
        io,
        "  camera: %ux%u JPEG quality=%u fps=%u auth=%s client=%s\n"
        "  stream: count=%" PRIu32 " frames=%" PRIu32 " bytes=%" PRIu64
        " capture-errors=%" PRIu32 " send-errors=%" PRIu32 " last=%s\n",
        (unsigned)state.camera_config.width, (unsigned)state.camera_config.height,
        (unsigned)state.camera_config.jpeg_quality,
        (unsigned)state.fps,
        state.auth_required ? "required" : "none",
        state.stream_active ? "connected" : "none",
        state.stream_count,
        state.frame_count,
        state.jpeg_bytes,
        state.capture_errors,
        state.send_errors,
        esp_err_to_name(state.last_error));
}

static void cam_webd_stream_worker(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    (void)httpd_resp_set_type(req, CAM_WEBD_CONTENT_TYPE);
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_set_hdr(req, "Connection", "close");

    const uint32_t frame_period_ms = 1000U / cam_webd.fps;
    while (!cam_webd_should_stop()) {
        const TickType_t frame_start = xTaskGetTickCount();
        const solar_os_camera_frame_t *frame = NULL;
        esp_err_t error = solar_os_camera_capture(&cam_webd.camera_owner, &frame);
        cam_webd_note_capture(error);
        if (error != ESP_OK) {
            cam_webd_delay(CAM_WEBD_RETRY_MS);
            continue;
        }

        char part_header[128];
        const int header_len = snprintf(
            part_header,
            sizeof(part_header),
            "--" CAM_WEBD_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            (unsigned)frame->length);
        if (header_len < 0 || (size_t)header_len >= sizeof(part_header)) {
            error = ESP_ERR_INVALID_SIZE;
        } else {
            error = httpd_resp_send_chunk(req, part_header, (size_t)header_len);
        }
        if (error == ESP_OK) {
            error = httpd_resp_send_chunk(req,
                                          (const char *)frame->data,
                                          frame->length);
        }
        if (error == ESP_OK) {
            error = httpd_resp_send_chunk(req, "\r\n", 2U);
        }
        const size_t frame_length = frame->length;
        const esp_err_t release_error =
            solar_os_camera_release_frame(&cam_webd.camera_owner, frame);
        if (error == ESP_OK && release_error != ESP_OK) {
            error = release_error;
        }
        cam_webd_note_send(frame_length, error);
        if (error != ESP_OK) {
            break;
        }

        const uint32_t elapsed_ms =
            (uint32_t)((xTaskGetTickCount() - frame_start) * portTICK_PERIOD_MS);
        if (elapsed_ms < frame_period_ms) {
            cam_webd_delay(frame_period_ms - elapsed_ms);
        }
    }

    (void)solar_os_http_server_complete_async(req);

    portENTER_CRITICAL(&cam_webd_lock);
    cam_webd.stream_active = false;
    cam_webd.stream_server = NULL;
    cam_webd.stream_socket = -1;
    const bool release_after_stop = !cam_webd.running || cam_webd.stop_requested;
    portEXIT_CRITICAL(&cam_webd_lock);

    if (release_after_stop) {
        const esp_err_t cleanup_error = cam_webd_release_resources();
        if (cleanup_error != ESP_OK) {
            portENTER_CRITICAL(&cam_webd_lock);
            cam_webd.draining = true;
            cam_webd.last_error = cleanup_error;
            portEXIT_CRITICAL(&cam_webd_lock);
        }
    }
    cam_webd.worker_done = true;
    solar_os_task_delete_internal(NULL);
}

const solar_os_job_t solar_os_cam_webd_job = {
    .name = "cam-webd",
    .summary = "HTTP camera stream",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = cam_webd_job_start,
    .stop = cam_webd_job_stop,
    .event = cam_webd_job_event,
    .worker_stack_bytes = CAM_WEBD_WORKER_STACK,
    .worker_stack_external = false,
    .detail = cam_webd_job_detail,
};
