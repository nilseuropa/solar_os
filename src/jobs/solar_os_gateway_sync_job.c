#include "solar_os_gateway_sync_job.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_chat.h"
#include "solar_os_chat_transport_gateway.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_task.h"
#include "solar_os_wifi.h"

#define GATEWAY_SYNC_RETRY_MIN_MS 1000U
#define GATEWAY_SYNC_RETRY_MAX_MS 60000U
#define GATEWAY_SYNC_EVENTS_PER_TICK 12U
#define GATEWAY_SYNC_WORKER_STACK 6144U
#define GATEWAY_SYNC_WORKER_PRIORITY (tskIDLE_PRIORITY + 1)
#define GATEWAY_SYNC_WORKER_INTERVAL_MS 100U
#define GATEWAY_SYNC_STOP_WAIT_MS 3000U

typedef struct {
    bool running;
    volatile bool stop_requested;
    volatile bool worker_done;
    bool rejoin_pending;
    bool transport_busy;
    bool messaging_inflight;
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
    solar_os_messaging_outbound_t *outbound;
} gateway_sync_state_t;

static gateway_sync_state_t gateway_sync;
static const char *TAG = "gateway_sync";

static void gateway_sync_worker(void *arg);

static void gateway_sync_schedule_retry(uint32_t now_ms)
{
    if (gateway_sync.retry_delay_ms == 0) {
        gateway_sync.retry_delay_ms = GATEWAY_SYNC_RETRY_MIN_MS;
    }
    gateway_sync.next_retry_ms = now_ms + gateway_sync.retry_delay_ms;
    if (gateway_sync.retry_delay_ms < GATEWAY_SYNC_RETRY_MAX_MS) {
        uint32_t next = gateway_sync.retry_delay_ms * 2U;
        gateway_sync.retry_delay_ms = next < GATEWAY_SYNC_RETRY_MAX_MS ?
            next : GATEWAY_SYNC_RETRY_MAX_MS;
    }
}

static bool gateway_sync_url_is_local(const char *url)
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

static void gateway_sync_begin_rejoin(void)
{
    gateway_sync.rejoin_count = solar_os_chat_channel_snapshot(
        gateway_sync.channels,
        SOLAR_OS_CHAT_CHANNEL_CAPACITY);
    gateway_sync.rejoin_index = 0;
    gateway_sync.rejoin_pending = true;
    gateway_sync.transport_busy = false;
    gateway_sync.messaging_inflight = false;
    gateway_sync.inflight_id = 0;
}

static void gateway_sync_drain_events(uint32_t now_ms)
{
    for (size_t i = 0; i < GATEWAY_SYNC_EVENTS_PER_TICK; i++) {
        const esp_err_t err = gateway_sync.transport->read_event(gateway_sync.event, 0);
        if (err != ESP_OK) {
            break;
        }
        solar_os_chat_event_t *event = &gateway_sync.event->event;
        if (event->type == SOLAR_OS_CHAT_EVENT_COMMAND_SENT) {
            gateway_sync.transport_busy = false;
            if (gateway_sync.rejoin_pending) {
                gateway_sync.rejoin_index++;
            } else if (gateway_sync.messaging_inflight &&
                       gateway_sync.inflight_id != 0 &&
                       event->command_id == gateway_sync.inflight_id) {
                (void)solar_os_messaging_outbox_update(
                    gateway_sync.inflight_id,
                    SOLAR_OS_DELIVERY_SENT,
                    NULL);
                gateway_sync.inflight_id = 0;
                gateway_sync.messaging_inflight = false;
            } else if (gateway_sync.inflight_id != 0 &&
                       event->command_id == gateway_sync.inflight_id) {
                (void)solar_os_chat_outbox_ack(gateway_sync.inflight_id);
                gateway_sync.inflight_id = 0;
            }
            continue;
        }
        bool inserted = false;
        uint64_t message_key = 0;
        const esp_err_t publish_err =
            solar_os_gateway_sync_publish(event, &inserted, &message_key);
        if (publish_err != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "message store failed: %s",
                          esp_err_to_name(publish_err));
        }
        if (event->type == SOLAR_OS_CHAT_EVENT_CONNECTED) {
            gateway_sync.retry_delay_ms = GATEWAY_SYNC_RETRY_MIN_MS;
            gateway_sync.next_retry_ms = 0;
            gateway_sync_begin_rejoin();
        } else if (event->type == SOLAR_OS_CHAT_EVENT_DISCONNECTED) {
            gateway_sync.transport_busy = false;
            if (gateway_sync.messaging_inflight &&
                gateway_sync.inflight_id != 0) {
                (void)solar_os_messaging_outbox_update(
                    gateway_sync.inflight_id,
                    SOLAR_OS_DELIVERY_QUEUED,
                    "gateway disconnected before send");
            }
            gateway_sync.inflight_id = 0;
            gateway_sync.messaging_inflight = false;
            gateway_sync.rejoin_pending = false;
            gateway_sync_schedule_retry(now_ms);
        }
    }
}

static void gateway_sync_submit_next(void)
{
    if (gateway_sync.transport_busy) {
        return;
    }
    if (gateway_sync.rejoin_pending) {
        while (gateway_sync.rejoin_index < gateway_sync.rejoin_count &&
               !gateway_sync.channels[gateway_sync.rejoin_index].desired) {
            gateway_sync.rejoin_index++;
        }
        if (gateway_sync.rejoin_index >= gateway_sync.rejoin_count) {
            gateway_sync.rejoin_pending = false;
        } else {
            memset(gateway_sync.command, 0, sizeof(*gateway_sync.command));
            gateway_sync.command->type = SOLAR_OS_CHAT_COMMAND_JOIN;
            strlcpy(gateway_sync.command->channel,
                    gateway_sync.channels[gateway_sync.rejoin_index].name,
                    sizeof(gateway_sync.command->channel));
            gateway_sync.command->cursor = solar_os_chat_channel_cursor(
                gateway_sync.command->channel);
            if (gateway_sync.transport->submit(gateway_sync.command) == ESP_OK) {
                gateway_sync.transport_busy = true;
            }
            return;
        }
    }
    if (solar_os_chat_outbox_peek(gateway_sync.command) != ESP_OK) {
        if (solar_os_messaging_outbox_peek(
                SOLAR_OS_MESSAGING_PROVIDER_GATEWAY,
                gateway_sync.outbound) != ESP_OK) {
            return;
        }
        memset(gateway_sync.command, 0, sizeof(*gateway_sync.command));
        gateway_sync.command->id = gateway_sync.outbound->id;
        gateway_sync.command->type = SOLAR_OS_CHAT_COMMAND_MESSAGE;
        strlcpy(gateway_sync.command->channel,
                gateway_sync.outbound->provider_key,
                sizeof(gateway_sync.command->channel));
        strlcpy(gateway_sync.command->text,
                gateway_sync.outbound->body,
                sizeof(gateway_sync.command->text));
        if (gateway_sync.transport->submit(gateway_sync.command) == ESP_OK) {
            gateway_sync.inflight_id = gateway_sync.outbound->id;
            gateway_sync.messaging_inflight = true;
            gateway_sync.transport_busy = true;
            (void)solar_os_messaging_outbox_update(
                gateway_sync.outbound->id,
                SOLAR_OS_DELIVERY_SENDING,
                NULL);
        }
        return;
    }
    if (gateway_sync.command->type == SOLAR_OS_CHAT_COMMAND_JOIN) {
        gateway_sync.command->cursor = solar_os_chat_channel_cursor(
            gateway_sync.command->channel);
    }
    if (gateway_sync.transport->submit(gateway_sync.command) == ESP_OK) {
        gateway_sync.inflight_id = gateway_sync.command->id;
        gateway_sync.messaging_inflight = false;
        gateway_sync.transport_busy = true;
    }
}

static esp_err_t gateway_sync_alloc_buffers(void)
{
    gateway_sync.event = solar_os_memory_calloc(1,
                                             sizeof(*gateway_sync.event),
                                             SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                             "chat.sync.event");
    gateway_sync.command = solar_os_memory_calloc(1,
                                               sizeof(*gateway_sync.command),
                                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                               "chat.sync.command");
    gateway_sync.channels = solar_os_memory_calloc(SOLAR_OS_CHAT_CHANNEL_CAPACITY,
                                                sizeof(*gateway_sync.channels),
                                                SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                                "chat.sync.channels");
    gateway_sync.outbound = solar_os_memory_calloc(
        1,
        sizeof(*gateway_sync.outbound),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "chat.sync.outbound");
    if (gateway_sync.event == NULL ||
        gateway_sync.command == NULL ||
        gateway_sync.channels == NULL ||
        gateway_sync.outbound == NULL) {
        solar_os_memory_free(gateway_sync.event);
        solar_os_memory_free(gateway_sync.command);
        solar_os_memory_free(gateway_sync.channels);
        solar_os_memory_free(gateway_sync.outbound);
        gateway_sync.event = NULL;
        gateway_sync.command = NULL;
        gateway_sync.channels = NULL;
        gateway_sync.outbound = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void gateway_sync_free_buffers(void)
{
    solar_os_memory_free(gateway_sync.event);
    solar_os_memory_free(gateway_sync.command);
    solar_os_memory_free(gateway_sync.channels);
    solar_os_memory_free(gateway_sync.outbound);
    gateway_sync.event = NULL;
    gateway_sync.command = NULL;
    gateway_sync.channels = NULL;
    gateway_sync.outbound = NULL;
}

static esp_err_t gateway_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc > 1 || (argc == 1 && argv != NULL && argv[0] != NULL &&
                     strcmp(argv[0], solar_os_gateway_sync_job.name) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gateway_sync.running) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&gateway_sync, 0, sizeof(gateway_sync));
    esp_err_t err = solar_os_chat_init();
    if (err != ESP_OK) {
        return err;
    }
    gateway_sync.transport = &solar_os_chat_gateway_transport;
    err = gateway_sync.transport->init();
    if (err != ESP_OK) {
        return err;
    }
    err = gateway_sync_alloc_buffers();
    if (err != ESP_OK) {
        return err;
    }
    gateway_sync.running = true;
    gateway_sync.retry_delay_ms = GATEWAY_SYNC_RETRY_MIN_MS;
    (void)solar_os_gateway_sync_set_status(true, false, ESP_OK, NULL);
    if (solar_os_task_create_pinned(gateway_sync_worker,
                                    "gateway_sync",
                                    GATEWAY_SYNC_WORKER_STACK,
                                    NULL,
                                    GATEWAY_SYNC_WORKER_PRIORITY,
                                    &gateway_sync.worker_task,
                                    tskNO_AFFINITY,
                                    SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        gateway_sync.running = false;
        (void)solar_os_gateway_sync_set_status(false,
                                            false,
                                            ESP_ERR_NO_MEM,
                                            "synchronizer worker allocation failed");
        gateway_sync_free_buffers();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void gateway_sync_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    gateway_sync.stop_requested = true;
    if (gateway_sync.worker_task != NULL) {
        (void)xTaskNotifyGive(gateway_sync.worker_task);
    }
    if (!solar_os_task_wait_done(gateway_sync.worker_task,
                                 &gateway_sync.worker_done,
                                 GATEWAY_SYNC_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG,
                      "worker did not stop within %u ms",
                      (unsigned)GATEWAY_SYNC_STOP_WAIT_MS);
        return;
    }
    gateway_sync.worker_task = NULL;
    gateway_sync_free_buffers();
}

static bool gateway_sync_process(uint32_t now_ms)
{
    solar_os_chat_config_t config;
    if (solar_os_chat_get_config(&config) != ESP_OK) {
        return false;
    }

    solar_os_chat_transport_status_t transport_status;
    if (gateway_sync.transport->get_status(&transport_status) != ESP_OK) {
        return false;
    }
    gateway_sync_drain_events(now_ms);

    if (!config.enabled || !config.configured) {
        if (transport_status.running) {
            (void)gateway_sync.transport->request_stop();
        } else if (transport_status.initialized) {
            (void)gateway_sync.transport->reap();
        }
        (void)solar_os_gateway_sync_set_status(true, false, ESP_OK, NULL);
        gateway_sync.config_revision = config.revision;
        return true;
    }

    const bool config_changed = gateway_sync.config_revision != config.revision;
    if (gateway_sync.config_revision != 0 && config_changed) {
        if (transport_status.running) {
            (void)gateway_sync.transport->request_stop();
        } else {
            (void)gateway_sync.transport->reap();
        }
        gateway_sync.next_retry_ms = 0;
        gateway_sync.rejoin_pending = false;
        gateway_sync.transport_busy = false;
        gateway_sync.messaging_inflight = false;
        gateway_sync.inflight_id = 0;
    }
    gateway_sync.config_revision = config.revision;

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.has_ip && !gateway_sync_url_is_local(config.url)) {
        if (transport_status.running) {
            (void)gateway_sync.transport->request_stop();
        }
        (void)solar_os_gateway_sync_set_status(true, false, ESP_OK, NULL);
        return true;
    }

    if (!transport_status.running && !transport_status.connected) {
        if (gateway_sync.transport->reap() != ESP_OK) {
            return true;
        }
        if (gateway_sync.next_retry_ms == 0 ||
            (int32_t)(now_ms - gateway_sync.next_retry_ms) >= 0) {
            const esp_err_t err =
                gateway_sync.transport->connect(&config);
            if (err != ESP_OK) {
                gateway_sync_schedule_retry(now_ms);
                (void)solar_os_gateway_sync_set_status(true,
                                                    false,
                                                    err,
                                                    "transport start failed");
                return true;
            }
            (void)solar_os_jobs_note_resource(solar_os_gateway_sync_job.name,
                                              SOLAR_OS_JOB_RESOURCE_NET,
                                              config.url,
                                              "chat");
        }
    }

    if (gateway_sync.transport->get_status(&transport_status) == ESP_OK) {
        (void)solar_os_gateway_sync_set_status(true,
                                            transport_status.connected,
                                            transport_status.last_esp_error,
                                            transport_status.last_error);
        if (transport_status.connected) {
            gateway_sync_submit_next();
        } else if (!transport_status.running && gateway_sync.next_retry_ms == 0) {
            gateway_sync_schedule_retry(now_ms);
        }
    }
    return true;
}

static void gateway_sync_worker(void *arg)
{
    (void)arg;
    while (!gateway_sync.stop_requested) {
        const uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        (void)gateway_sync_process(now_ms);
        (void)ulTaskNotifyTake(pdTRUE,
                              pdMS_TO_TICKS(GATEWAY_SYNC_WORKER_INTERVAL_MS));
    }
    (void)gateway_sync.transport->disconnect();
    (void)solar_os_gateway_sync_set_status(false, false, ESP_OK, NULL);
    gateway_sync.running = false;
    gateway_sync.worker_done = true;
    solar_os_task_delete(NULL);
}

const solar_os_job_t solar_os_gateway_sync_job = {
    .name = "gateway-sync",
    .summary = "synchronize the gateway messaging provider",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = gateway_sync_start,
    .stop = gateway_sync_stop,
    .worker_stack_bytes = GATEWAY_SYNC_WORKER_STACK,
};
