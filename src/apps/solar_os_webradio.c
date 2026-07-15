#include "solar_os_webradio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_terminal.h"

/*
 * Internet radio player: streams an MP3 station (icecast/shoutcast
 * style plain HTTP audio) through the same decode/resample pipeline
 * aplay uses -- solar_os_audio_play_mp3_stream() with a read callback
 * as the byte source. MP3 streams only (minimp3); most stations offer
 * an MP3 mount alongside AAC ones.
 *
 * Two tasks, decoupled by a PSRAM jitter ring: a network task keeps
 * esp_http_client reads flowing into the ring, and the decode/playback
 * task pulls from it. Without the ring, every network hiccup longer
 * than the I2S DMA queue (~30ms) was an audible skip -- the ring rides
 * out multi-second stalls, at the cost of that much extra latency
 * behind the live stream, which for radio is irrelevant.
 */
#define WEBRADIO_URL_MAX 160
/* Task stacks must live in contiguous internal RAM, which is the
 * scarcest resource on these boards once WiFi+BLE+display are up --
 * webradio taking too much of it starves the remote job's HTTP server
 * of per-connection allocations (pages stop loading entirely while
 * the radio plays). Keep both stacks as tight as safely possible: the
 * decode pipeline keeps all big state on the heap (mp3dec_t included),
 * so its stack only carries minimp3 call frames. */
#define WEBRADIO_TASK_STACK 20480
/* TLS handshakes (https stations) need the larger stack; plain http
 * needs less -- but not as little as you'd hope: 6144 overflowed
 * (instant reset) on first connect. Chosen per URL at start. */
#define WEBRADIO_NET_TASK_STACK_TLS 10240
#define WEBRADIO_NET_TASK_STACK_PLAIN 8192
#define WEBRADIO_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define WEBRADIO_HTTP_TIMEOUT_MS 8000
#define WEBRADIO_HTTP_BUFFER_BYTES 4096
#define WEBRADIO_EVENT_QUEUE_LEN 8
#define WEBRADIO_MESSAGE_MAX 96
/* 128KB PSRAM = ~8 seconds of a 128kbps stream held in reserve;
 * playback starts once a quarter of it has arrived (~2s). */
#define WEBRADIO_RING_BYTES (128U * 1024U)
#define WEBRADIO_PREBUFFER_BYTES (WEBRADIO_RING_BYTES / 4U)
#define WEBRADIO_NET_CHUNK_BYTES 4096

typedef enum {
    WEBRADIO_EVENT_STATUS,
    WEBRADIO_EVENT_PROGRESS,
    WEBRADIO_EVENT_DONE,
} webradio_event_type_t;

typedef struct {
    webradio_event_type_t type;
    esp_err_t err;
    bool cancelled;
    uint32_t duration_ms;
    char message[WEBRADIO_MESSAGE_MAX];
} webradio_event_t;

typedef struct {
    char url[WEBRADIO_URL_MAX];
    uint8_t volume;
    volatile bool stop_requested;
    volatile bool task_done;
    volatile bool net_done;
    volatile bool decode_done;
    volatile bool prebuffered;
    TaskHandle_t task;
    TaskHandle_t net_task;
    QueueHandle_t events;
    RingbufHandle_t ring;
    bool finished;
    uint32_t last_reported_s;
} webradio_state_t;

static const char *TAG = "solar_os_webradio";
static webradio_state_t webradio;

static solar_os_terminal_t *webradio_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_terminal(ctx);
}

static void webradio_send_event(const webradio_event_t *event)
{
    if (webradio.events == NULL) {
        return;
    }
    (void)xQueueSend(webradio.events, event, pdMS_TO_TICKS(100));
}

static void webradio_send_status(const char *message)
{
    webradio_event_t event = {
        .type = WEBRADIO_EVENT_STATUS,
    };
    strlcpy(event.message, message, sizeof(event.message));
    webradio_send_event(&event);
}

static bool webradio_should_cancel(void *user)
{
    (void)user;
    return webradio.stop_requested;
}

static void webradio_progress(const solar_os_audio_wav_progress_t *progress, void *user)
{
    (void)user;
    if (progress == NULL || progress->done) {
        return;
    }

    webradio_event_t event = {
        .type = WEBRADIO_EVENT_PROGRESS,
        .duration_ms = progress->info.duration_ms,
    };
    webradio_send_event(&event);
}

/*
 * Network task: keeps the HTTP stream flowing into the PSRAM jitter
 * ring so the decode task never waits on the network directly.
 */
static void webradio_net_task(void *arg)
{
    (void)arg;

    esp_http_client_handle_t client = NULL;
    uint8_t *chunk = NULL;
    uint32_t total_in = 0;

    esp_http_client_config_t config = {
        .url = webradio.url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WEBRADIO_HTTP_TIMEOUT_MS,
        .buffer_size = WEBRADIO_HTTP_BUFFER_BYTES,
        .buffer_size_tx = 512,
        .user_agent = "SolarOS-webradio/0.1",
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    webradio_send_status("connecting...");
    client = esp_http_client_init(&config);
    if (client == NULL) {
        webradio_send_status("http client init failed");
        goto done;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        webradio_send_status("connection failed");
        goto done;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        char message[WEBRADIO_MESSAGE_MAX];
        snprintf(message, sizeof(message), "HTTP status %d", status);
        webradio_send_status(message);
        goto done;
    }

    /* Plain memcpy target (no DMA involved) -- keep it out of the
     * precious internal heap. */
    chunk = heap_caps_malloc(WEBRADIO_NET_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        chunk = heap_caps_malloc(WEBRADIO_NET_CHUNK_BYTES, MALLOC_CAP_8BIT);
    }
    if (chunk == NULL) {
        webradio_send_status("out of memory");
        goto done;
    }

    webradio_send_status("buffering...");

    while (!webradio.stop_requested && !webradio.decode_done) {
        const int got = esp_http_client_read(client, (char *)chunk, WEBRADIO_NET_CHUNK_BYTES);
        if (got <= 0) {
            break;
        }

        size_t sent = 0;
        while (sent < (size_t)got && !webradio.stop_requested && !webradio.decode_done) {
            /* Ring full = we're comfortably ahead of playback; wait for
             * the decoder to drain some. */
            if (xRingbufferSend(webradio.ring, chunk + sent, (size_t)got - sent, pdMS_TO_TICKS(200)) == pdTRUE) {
                sent = (size_t)got;
                break;
            }
        }

        total_in += (uint32_t)got;
        if (!webradio.prebuffered && total_in >= WEBRADIO_PREBUFFER_BYTES) {
            webradio.prebuffered = true;
            webradio_send_status("playing");
        }
    }

done:
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    heap_caps_free(chunk);
    /* Unblock the decoder if the stream ended before prebuffering. */
    webradio.prebuffered = true;
    webradio.net_done = true;
    vTaskDelete(NULL);
}

/* Decode-side byte source: pulls from the jitter ring. */
static int webradio_ring_read_cb(void *user, uint8_t *buffer, size_t len)
{
    (void)user;

    while (!webradio.prebuffered && !webradio.stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (!webradio.stop_requested) {
        size_t item_len = 0;
        uint8_t *item = xRingbufferReceiveUpTo(webradio.ring,
                                               &item_len,
                                               pdMS_TO_TICKS(100),
                                               len);
        if (item != NULL) {
            memcpy(buffer, item, item_len);
            vRingbufferReturnItem(webradio.ring, item);
            return (int)item_len;
        }
        if (webradio.net_done) {
            return 0;
        }
    }
    return 0;
}

static void webradio_task(void *arg)
{
    (void)arg;

    esp_err_t err = ESP_OK;
    bool cancelled = false;

    const uint32_t net_stack = strncmp(webradio.url, "https://", 8) == 0 ?
        WEBRADIO_NET_TASK_STACK_TLS :
        WEBRADIO_NET_TASK_STACK_PLAIN;
    const BaseType_t net_created = xTaskCreatePinnedToCore(webradio_net_task,
                                                           "webradio_net",
                                                           net_stack,
                                                           NULL,
                                                           WEBRADIO_TASK_PRIORITY,
                                                           &webradio.net_task,
                                                           tskNO_AFFINITY);
    if (net_created != pdPASS) {
        webradio.net_task = NULL;
        webradio.net_done = true;
        webradio_send_status("network task start failed");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    const solar_os_audio_wav_options_t options = {
        .should_cancel = webradio_should_cancel,
        .progress = webradio_progress,
        .user = NULL,
        .progress_interval_ms = SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS,
    };
    err = solar_os_audio_play_mp3_stream(webradio_ring_read_cb,
                                         NULL,
                                         webradio.volume,
                                         &options,
                                         NULL);
    cancelled = webradio.stop_requested || err == ESP_ERR_TIMEOUT;

done:
    webradio.decode_done = true;
    while (!webradio.net_done) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    webradio_event_t event = {
        .type = WEBRADIO_EVENT_DONE,
        .err = err,
        .cancelled = cancelled,
    };
    webradio_send_event(&event);

    SOLAR_OS_LOGI(TAG, "stream ended: %s%s", esp_err_to_name(err), cancelled ? " (stopped)" : "");
    webradio.task_done = true;
    vTaskDelete(NULL);
}

static bool webradio_parse_args(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    webradio.volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL;
    webradio.url[0] = '\0';

    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (arg == NULL) {
            return false;
        }
        if (strcmp(arg, "-v") == 0) {
            if (i + 1 >= argc) {
                return false;
            }
            const char *value = solar_os_context_argv(ctx, ++i);
            char *end = NULL;
            const unsigned long parsed = strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed > 100) {
                return false;
            }
            webradio.volume = (uint8_t)parsed;
            continue;
        }
        if (webradio.url[0] != '\0') {
            return false;
        }
        if (strncmp(arg, "http://", 7) != 0 && strncmp(arg, "https://", 8) != 0) {
            return false;
        }
        if (strlcpy(webradio.url, arg, sizeof(webradio.url)) >= sizeof(webradio.url)) {
            return false;
        }
    }

    return webradio.url[0] != '\0';
}

static esp_err_t webradio_start(solar_os_context_t *ctx)
{
    solar_os_terminal_t *term = webradio_terminal(ctx);

    if (webradio.task != NULL && !webradio.task_done) {
        solar_os_terminal_clear(term);
        solar_os_terminal_writeln(term, "webradio: previous stream is still stopping");
        solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
        return ESP_OK;
    }

    if (webradio.events != NULL) {
        vQueueDelete(webradio.events);
    }
    if (webradio.ring != NULL) {
        vRingbufferDelete(webradio.ring);
    }
    memset(&webradio, 0, sizeof(webradio));

    if (!webradio_parse_args(ctx)) {
        solar_os_terminal_clear(term);
        solar_os_terminal_writeln_bold(term, "webradio");
        solar_os_terminal_writeln(term, "usage: webradio <http(s)://mp3-stream-url> [-v volume]");
        solar_os_terminal_writeln(term, "MP3 streams only (pick a station's MP3 mount, not AAC)");
        solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
        return ESP_OK;
    }

    solar_os_terminal_clear(term);
    solar_os_terminal_writeln_bold(term, "webradio");
    solar_os_terminal_printf(term, "url: %s\n", webradio.url);
    solar_os_terminal_writeln(term, "ESC stops, CTRL+ALT+DEL exits");

    webradio.events = xQueueCreate(WEBRADIO_EVENT_QUEUE_LEN, sizeof(webradio_event_t));
    if (webradio.events == NULL) {
        solar_os_terminal_writeln(term, "webradio: out of memory");
        return ESP_OK;
    }

    /* Jitter ring lives in PSRAM -- internal RAM is precious and this
     * is exactly the bulk-buffer case PSRAM exists for. */
    webradio.ring = xRingbufferCreateWithCaps(WEBRADIO_RING_BYTES,
                                              RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (webradio.ring == NULL) {
        webradio.ring = xRingbufferCreate(WEBRADIO_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    }
    if (webradio.ring == NULL) {
        vQueueDelete(webradio.events);
        webradio.events = NULL;
        solar_os_terminal_writeln(term, "webradio: jitter buffer allocation failed");
        return ESP_OK;
    }

    if (xTaskCreatePinnedToCore(webradio_task,
                                "webradio",
                                WEBRADIO_TASK_STACK,
                                NULL,
                                WEBRADIO_TASK_PRIORITY,
                                &webradio.task,
                                tskNO_AFFINITY) != pdPASS) {
        vQueueDelete(webradio.events);
        webradio.events = NULL;
        vRingbufferDelete(webradio.ring);
        webradio.ring = NULL;
        solar_os_terminal_printf(term,
                                 "webradio: task start failed (stack %u, internal free %u, largest %u)\n",
                                 (unsigned)WEBRADIO_TASK_STACK,
                                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_OK;
    }

    /* SolarOS's default WIFI_PS_MAX_MODEM sleeps the radio between
     * beacons -- fine normally, but with a continuous stream plus other
     * traffic (e.g. the remote screen share) the sleep scheduling
     * starves short-lived inbound connections. Streaming audio is a
     * power-hungry activity anyway, so drop to minimum power save
     * while the radio plays; webradio_stop() restores the default. */
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    return ESP_OK;
}

static void webradio_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    webradio.stop_requested = true;
    for (uint32_t i = 0; i < 100 && webradio.task != NULL && !webradio.task_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (webradio.events != NULL) {
        vQueueDelete(webradio.events);
        webradio.events = NULL;
    }
    if (webradio.ring != NULL && webradio.task_done) {
        vRingbufferDelete(webradio.ring);
        webradio.ring = NULL;
    }
    webradio.task = NULL;

    /* Back to SolarOS's default power-save policy (see start()). */
    (void)esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

static void webradio_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    strlcpy(buffer, "webradio", buffer_len);
}

static void webradio_drain_events(solar_os_context_t *ctx)
{
    if (webradio.events == NULL) {
        return;
    }

    solar_os_terminal_t *term = webradio_terminal(ctx);
    webradio_event_t event;
    while (xQueueReceive(webradio.events, &event, 0) == pdTRUE) {
        switch (event.type) {
        case WEBRADIO_EVENT_STATUS:
            solar_os_terminal_printf(term, "%s\n", event.message);
            break;
        case WEBRADIO_EVENT_PROGRESS: {
            const uint32_t seconds = event.duration_ms / 1000U;
            if (seconds != webradio.last_reported_s && (seconds % 30U) == 0) {
                webradio.last_reported_s = seconds;
                solar_os_terminal_printf(term,
                                         "listening %02u:%02u:%02u\n",
                                         (unsigned)(seconds / 3600U),
                                         (unsigned)((seconds / 60U) % 60U),
                                         (unsigned)(seconds % 60U));
            }
            break;
        }
        case WEBRADIO_EVENT_DONE:
            webradio.finished = true;
            if (event.cancelled) {
                solar_os_terminal_writeln(term, "stopped");
            } else if (event.err == ESP_OK) {
                solar_os_terminal_writeln(term, "stream ended");
            } else {
                solar_os_terminal_printf(term, "stream failed: %s\n", esp_err_to_name(event.err));
            }
            solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
            break;
        }
    }
}

static bool webradio_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        webradio_drain_events(ctx);
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (ch == SOLAR_OS_KEY_ESCAPE) {
        if (!webradio.stop_requested && !webradio.finished) {
            webradio.stop_requested = true;
            solar_os_terminal_writeln(webradio_terminal(ctx), "stopping...");
        }
        return true;
    }

    return true;
}

const solar_os_app_t solar_os_webradio_app = {
    .name = "webradio",
    .summary = "internet radio: webradio <mp3-stream-url> [-v volume]",
    .flags = 0,
    .start = webradio_start,
    .stop = webradio_stop,
    .event = webradio_event,
    .title = webradio_title,
};
