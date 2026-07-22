#include "solar_os_chat_sync_job.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_chat.h"
#include "solar_os_chat_transport_gateway.h"
#include "solar_os_inbox.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include "solar_os_wifi.h"

#define CHAT_SYNC_RETRY_MIN_MS 1000U
#define CHAT_SYNC_RETRY_MAX_MS 60000U
#define CHAT_SYNC_EVENTS_PER_TICK 12U
#define CHAT_SYNC_WORKER_STACK 6144U
#define CHAT_SYNC_WORKER_PRIORITY (tskIDLE_PRIORITY + 1)
#define CHAT_SYNC_WORKER_INTERVAL_MS 100U
#define CHAT_SYNC_STOP_WAIT_MS 3000U

typedef struct {
    bool running;
    volatile bool stop_requested;
    volatile bool worker_done;
    bool rejoin_pending;
    bool transport_busy;
    uint32_t config_revision;
    uint32_t next_retry_ms;
    uint32_t retry_delay_ms;
    uint32_t inflight_id;
    size_t rejoin_index;
    size_t rejoin_count;
    const solar_os_chat_transport_t *transport;
    TaskHandle_t worker_task;
    solar_os_chat_transport_event_t *event;
    solar_os_chat_command_t *command;
    solar_os_chat_channel_t *channels;
    char cursor[SOLAR_OS_CHAT_TRANSPORT_CURSOR_MAX];
    char cursor_endpoint[SOLAR_OS_CHAT_URL_MAX];
} chat_sync_state_t;

static chat_sync_state_t chat_sync;
static const char *TAG = "chat_sync";

static void chat_sync_worker(void *arg);

static void chat_sync_schedule_retry(uint32_t now_ms)
{
    if (chat_sync.retry_delay_ms == 0) {
        chat_sync.retry_delay_ms = CHAT_SYNC_RETRY_MIN_MS;
    }
    chat_sync.next_retry_ms = now_ms + chat_sync.retry_delay_ms;
    if (chat_sync.retry_delay_ms < CHAT_SYNC_RETRY_MAX_MS) {
        uint32_t next = chat_sync.retry_delay_ms * 2U;
        chat_sync.retry_delay_ms = next < CHAT_SYNC_RETRY_MAX_MS ?
            next : CHAT_SYNC_RETRY_MAX_MS;
    }
}

static bool chat_sync_url_is_local(const char *url)
{
    if (url == NULL) {
        return false;
    }
    const char *host = strstr(url, "://");
    host = host != NULL ? host + 3 : url;
    static const char loopback[] = "127.0.0.1";
    static const char localhost[] = "localhost";
    const size_t loopback_len = sizeof(loopback) - 1U;
    const size_t localhost_len = sizeof(localhost) - 1U;
    return (strncmp(host, loopback, loopback_len) == 0 &&
            (host[loopback_len] == '\0' || host[loopback_len] == ':' ||
             host[loopback_len] == '/')) ||
           (strncmp(host, localhost, localhost_len) == 0 &&
            (host[localhost_len] == '\0' || host[localhost_len] == ':' ||
             host[localhost_len] == '/'));
}

static void chat_sync_publish_inbox(const solar_os_chat_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_CHAT_EVENT_MESSAGE) {
        return;
    }
    char title[SOLAR_OS_INBOX_TITLE_MAX];
    if (event->channel[0] != '\0') {
        snprintf(title, sizeof(title), "Chat #%s", event->channel);
    } else {
        strlcpy(title, "Chat message", sizeof(title));
    }
    const solar_os_inbox_publish_t notification = {
        .source = "chat",
        .topic = event->channel,
        .sender = event->from,
        .title = title,
        .body = event->text,
        .timestamp_ms = event->timestamp,
        .priority = SOLAR_OS_INBOX_PRIORITY_NORMAL,
    };
    const esp_err_t err = solar_os_inbox_publish(&notification, NULL);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "inbox publish failed: %s", esp_err_to_name(err));
    }
}

static void chat_sync_begin_rejoin(void)
{
    chat_sync.rejoin_count = solar_os_chat_channel_snapshot(
        chat_sync.channels,
        SOLAR_OS_CHAT_CHANNEL_CAPACITY);
    chat_sync.rejoin_index = 0;
    chat_sync.rejoin_pending = true;
    chat_sync.transport_busy = false;
    chat_sync.inflight_id = 0;
}

static void chat_sync_drain_events(uint32_t now_ms)
{
    for (size_t i = 0; i < CHAT_SYNC_EVENTS_PER_TICK; i++) {
        const esp_err_t err = chat_sync.transport->read_event(chat_sync.event, 0);
        if (err != ESP_OK) {
            break;
        }
        solar_os_chat_event_t *event = &chat_sync.event->event;
        if (chat_sync.event->cursor[0] != '\0') {
            strlcpy(chat_sync.cursor,
                    chat_sync.event->cursor,
                    sizeof(chat_sync.cursor));
        }
        if (event->type == SOLAR_OS_CHAT_EVENT_COMMAND_SENT) {
            chat_sync.transport_busy = false;
            if (chat_sync.rejoin_pending) {
                chat_sync.rejoin_index++;
            } else if (chat_sync.inflight_id != 0 &&
                       event->command_id == chat_sync.inflight_id) {
                (void)solar_os_chat_outbox_ack(chat_sync.inflight_id);
                chat_sync.inflight_id = 0;
            }
            continue;
        }
        (void)solar_os_chat_sync_publish(event);
        chat_sync_publish_inbox(event);
        if (event->type == SOLAR_OS_CHAT_EVENT_CONNECTED) {
            chat_sync.retry_delay_ms = CHAT_SYNC_RETRY_MIN_MS;
            chat_sync.next_retry_ms = 0;
            chat_sync_begin_rejoin();
        } else if (event->type == SOLAR_OS_CHAT_EVENT_DISCONNECTED ||
                   event->type == SOLAR_OS_CHAT_EVENT_ERROR) {
            chat_sync.transport_busy = false;
            chat_sync.inflight_id = 0;
            chat_sync.rejoin_pending = false;
            chat_sync_schedule_retry(now_ms);
        }
    }
}

static void chat_sync_submit_next(void)
{
    if (chat_sync.transport_busy) {
        return;
    }
    if (chat_sync.rejoin_pending) {
        while (chat_sync.rejoin_index < chat_sync.rejoin_count &&
               !chat_sync.channels[chat_sync.rejoin_index].joined) {
            chat_sync.rejoin_index++;
        }
        if (chat_sync.rejoin_index >= chat_sync.rejoin_count) {
            chat_sync.rejoin_pending = false;
        } else {
            memset(chat_sync.command, 0, sizeof(*chat_sync.command));
            chat_sync.command->type = SOLAR_OS_CHAT_COMMAND_JOIN;
            strlcpy(chat_sync.command->channel,
                    chat_sync.channels[chat_sync.rejoin_index].name,
                    sizeof(chat_sync.command->channel));
            if (chat_sync.transport->submit(chat_sync.command) == ESP_OK) {
                chat_sync.transport_busy = true;
            }
            return;
        }
    }
    if (solar_os_chat_outbox_peek(chat_sync.command) != ESP_OK) {
        return;
    }
    if (chat_sync.transport->submit(chat_sync.command) == ESP_OK) {
        chat_sync.inflight_id = chat_sync.command->id;
        chat_sync.transport_busy = true;
    }
}

static esp_err_t chat_sync_alloc_buffers(void)
{
    chat_sync.event = solar_os_memory_calloc(1,
                                             sizeof(*chat_sync.event),
                                             SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                             "chat.sync.event");
    chat_sync.command = solar_os_memory_calloc(1,
                                               sizeof(*chat_sync.command),
                                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                               "chat.sync.command");
    chat_sync.channels = solar_os_memory_calloc(SOLAR_OS_CHAT_CHANNEL_CAPACITY,
                                                sizeof(*chat_sync.channels),
                                                SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                                "chat.sync.channels");
    if (chat_sync.event == NULL ||
        chat_sync.command == NULL ||
        chat_sync.channels == NULL) {
        solar_os_memory_free(chat_sync.event);
        solar_os_memory_free(chat_sync.command);
        solar_os_memory_free(chat_sync.channels);
        chat_sync.event = NULL;
        chat_sync.command = NULL;
        chat_sync.channels = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void chat_sync_free_buffers(void)
{
    solar_os_memory_free(chat_sync.event);
    solar_os_memory_free(chat_sync.command);
    solar_os_memory_free(chat_sync.channels);
    chat_sync.event = NULL;
    chat_sync.command = NULL;
    chat_sync.channels = NULL;
}

static esp_err_t chat_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc > 1 || (argc == 1 && argv != NULL && argv[0] != NULL &&
                     strcmp(argv[0], solar_os_chat_sync_job.name) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (chat_sync.running) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&chat_sync, 0, sizeof(chat_sync));
    esp_err_t err = solar_os_chat_init();
    if (err != ESP_OK) {
        return err;
    }
    chat_sync.transport = &solar_os_chat_gateway_transport;
    err = chat_sync.transport->init();
    if (err != ESP_OK) {
        return err;
    }
    err = chat_sync_alloc_buffers();
    if (err != ESP_OK) {
        return err;
    }
    chat_sync.running = true;
    chat_sync.retry_delay_ms = CHAT_SYNC_RETRY_MIN_MS;
    (void)solar_os_chat_sync_set_status(true, false, ESP_OK, NULL);
    if (solar_os_task_create_pinned(chat_sync_worker,
                                    "chat_sync",
                                    CHAT_SYNC_WORKER_STACK,
                                    NULL,
                                    CHAT_SYNC_WORKER_PRIORITY,
                                    &chat_sync.worker_task,
                                    tskNO_AFFINITY) != pdPASS) {
        chat_sync.running = false;
        (void)solar_os_chat_sync_set_status(false,
                                            false,
                                            ESP_ERR_NO_MEM,
                                            "synchronizer worker allocation failed");
        chat_sync_free_buffers();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void chat_sync_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    chat_sync.stop_requested = true;
    if (chat_sync.worker_task != NULL) {
        (void)xTaskNotifyGive(chat_sync.worker_task);
    }
    if (!solar_os_task_wait_done(chat_sync.worker_task,
                                 &chat_sync.worker_done,
                                 CHAT_SYNC_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG,
                      "worker did not stop within %u ms",
                      (unsigned)CHAT_SYNC_STOP_WAIT_MS);
        return;
    }
    chat_sync.worker_task = NULL;
    chat_sync_free_buffers();
}

static bool chat_sync_process(uint32_t now_ms)
{
    solar_os_chat_config_t config;
    if (solar_os_chat_get_config(&config) != ESP_OK) {
        return false;
    }

    solar_os_chat_transport_status_t transport_status;
    if (chat_sync.transport->get_status(&transport_status) != ESP_OK) {
        return false;
    }
    chat_sync_drain_events(now_ms);

    if (!config.enabled || !config.configured) {
        if (transport_status.running) {
            (void)chat_sync.transport->request_stop();
        } else if (transport_status.initialized) {
            (void)chat_sync.transport->reap();
        }
        (void)solar_os_chat_sync_set_status(true, false, ESP_OK, NULL);
        chat_sync.config_revision = config.revision;
        return true;
    }

    const bool config_changed = chat_sync.config_revision != config.revision;
    if (chat_sync.config_revision != 0 && config_changed) {
        if (transport_status.running) {
            (void)chat_sync.transport->request_stop();
        } else {
            (void)chat_sync.transport->reap();
        }
        chat_sync.next_retry_ms = 0;
        chat_sync.rejoin_pending = false;
        chat_sync.transport_busy = false;
        chat_sync.inflight_id = 0;
    }
    if (config_changed && strcmp(chat_sync.cursor_endpoint, config.url) != 0) {
        chat_sync.cursor[0] = '\0';
        strlcpy(chat_sync.cursor_endpoint,
                config.url,
                sizeof(chat_sync.cursor_endpoint));
    }
    chat_sync.config_revision = config.revision;

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.has_ip && !chat_sync_url_is_local(config.url)) {
        if (transport_status.running) {
            (void)chat_sync.transport->request_stop();
        }
        (void)solar_os_chat_sync_set_status(true, false, ESP_OK, NULL);
        return true;
    }

    if (!transport_status.running && !transport_status.connected) {
        if (chat_sync.transport->reap() != ESP_OK) {
            return true;
        }
        if (chat_sync.next_retry_ms == 0 ||
            (int32_t)(now_ms - chat_sync.next_retry_ms) >= 0) {
            const esp_err_t err =
                chat_sync.transport->connect(&config, chat_sync.cursor);
            if (err != ESP_OK) {
                chat_sync_schedule_retry(now_ms);
                (void)solar_os_chat_sync_set_status(true,
                                                    false,
                                                    err,
                                                    "transport start failed");
                return true;
            }
            (void)solar_os_jobs_note_resource(solar_os_chat_sync_job.name,
                                              SOLAR_OS_JOB_RESOURCE_NET,
                                              config.url,
                                              "chat");
        }
    }

    if (chat_sync.transport->get_status(&transport_status) == ESP_OK) {
        (void)solar_os_chat_sync_set_status(true,
                                            transport_status.connected,
                                            transport_status.last_esp_error,
                                            transport_status.last_error);
        if (transport_status.connected) {
            chat_sync_submit_next();
        } else if (!transport_status.running && chat_sync.next_retry_ms == 0) {
            chat_sync_schedule_retry(now_ms);
        }
    }
    return true;
}

static void chat_sync_worker(void *arg)
{
    (void)arg;
    while (!chat_sync.stop_requested) {
        const uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        (void)chat_sync_process(now_ms);
        (void)ulTaskNotifyTake(pdTRUE,
                              pdMS_TO_TICKS(CHAT_SYNC_WORKER_INTERVAL_MS));
    }
    (void)chat_sync.transport->disconnect();
    (void)solar_os_chat_sync_set_status(false, false, ESP_OK, NULL);
    chat_sync.running = false;
    chat_sync.worker_done = true;
    vTaskDelete(NULL);
}

const solar_os_job_t solar_os_chat_sync_job = {
    .name = "chat-sync",
    .summary = "synchronize chat transports and inbox notifications",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = chat_sync_start,
    .stop = chat_sync_stop,
};
