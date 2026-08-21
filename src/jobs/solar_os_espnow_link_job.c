#include "solar_os_espnow_link_job.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_espnow.h"
#include "solar_os_inbox.h"
#include "solar_os_jobs.h"
#include "solar_os_link_messaging.h"
#include "solar_os_link_stream.h"
#include "solar_os_log.h"
#include "solar_os_task.h"
#include "solar_os_wifi.h"

#define ESPNOW_LINK_TASK_STACK 6144U
#define ESPNOW_LINK_POLL_MS 5U
#define ESPNOW_LINK_SEND_TIMEOUT_MS 3000U
#define ESPNOW_LINK_STOP_WAIT_MS 1000U

static const char *TAG = "espnow-link";

typedef struct {
    solar_os_espnow_link_job_status_t status;
    char owner[SOLAR_OS_ESPNOW_OWNER_MAX];
    solar_os_link_frame_t pending_frame;
    bool pending_send;
    bool pending_reported;
    uint32_t pending_since_ms;
    char start_error_detail[SOLAR_OS_JOB_ERROR_DETAIL_MAX];
    volatile bool stop_requested;
    TaskHandle_t task;
} espnow_link_state_t;

static EXT_RAM_BSS_ATTR espnow_link_state_t espnow_link;

static bool report_fixed_channel_conflict(uint8_t requested_channel)
{
    if (requested_channel == 0U) {
        return false;
    }
    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    uint8_t active_channel = 0U;
    if (wifi.connected && wifi.channel != 0U) {
        active_channel = wifi.channel;
    } else if ((wifi.ap_enabled || wifi.ap_running) && wifi.ap_channel != 0U) {
        active_channel = wifi.ap_channel;
    }
    if (active_channel == 0U || active_channel == requested_channel) {
        return false;
    }

    snprintf(espnow_link.start_error_detail,
             sizeof(espnow_link.start_error_detail),
             "requested ESP-NOW channel %u conflicts with active Wi-Fi channel %u; use channel=auto or channel=%u",
             (unsigned)requested_channel,
             (unsigned)active_channel,
             (unsigned)active_channel);
    SOLAR_OS_LOGW(TAG, "%s", espnow_link.start_error_detail);
    return true;
}

static void espnow_link_error_detail(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0U) {
        return;
    }
    strlcpy(buffer, espnow_link.start_error_detail, buffer_len);
}

static bool parse_on_off(const char *text, bool *value)
{
    if (text == NULL || value == NULL) {
        return false;
    }
    if (strcmp(text, "on") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "off") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool parse_channel(const char *text, uint8_t *channel)
{
    if (text == NULL || channel == NULL) {
        return false;
    }
    if (strcmp(text, "auto") == 0) {
        *channel = 0U;
        return true;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 1UL || parsed > 13UL) {
        return false;
    }
    *channel = (uint8_t)parsed;
    return true;
}

static bool parse_args(int argc,
                       char **argv,
                       const char **link,
                       uint8_t *channel,
                       solar_os_espnow_phy_t *phy,
                       bool *inbox_enabled,
                       bool *chat_enabled)
{
    int first = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_espnow_link_job.name) == 0) {
        first = 1;
    }
    if (argc - first < 1 || argc - first > 5) {
        return false;
    }

    *link = argv[first];
    *channel = 0U;
    *phy = SOLAR_OS_ESPNOW_PHY_NORMAL;
    *inbox_enabled = false;
    *chat_enabled = false;
    bool channel_seen = false;
    bool phy_seen = false;
    bool inbox_seen = false;
    bool chat_seen = false;
    for (int index = first + 1; index < argc; index++) {
        static const char channel_prefix[] = "channel=";
        static const char phy_prefix[] = "phy=";
        static const char inbox_prefix[] = "inbox=";
        static const char chat_prefix[] = "chat=";
        if (strncmp(argv[index], channel_prefix, sizeof(channel_prefix) - 1U) == 0) {
            if (channel_seen ||
                !parse_channel(argv[index] + sizeof(channel_prefix) - 1U, channel)) {
                return false;
            }
            channel_seen = true;
        } else if (strncmp(argv[index], phy_prefix, sizeof(phy_prefix) - 1U) == 0) {
            if (phy_seen ||
                !solar_os_espnow_parse_phy(
                    argv[index] + sizeof(phy_prefix) - 1U, phy)) {
                return false;
            }
            phy_seen = true;
        } else if (strncmp(argv[index], inbox_prefix, sizeof(inbox_prefix) - 1U) == 0) {
            if (inbox_seen ||
                !parse_on_off(argv[index] + sizeof(inbox_prefix) - 1U,
                              inbox_enabled)) {
                return false;
            }
            inbox_seen = true;
        } else if (strncmp(argv[index], chat_prefix, sizeof(chat_prefix) - 1U) == 0) {
            if (chat_seen ||
                !parse_on_off(argv[index] + sizeof(chat_prefix) - 1U,
                              chat_enabled)) {
                return false;
            }
            chat_seen = true;
        } else {
            return false;
        }
    }
    return !(*inbox_enabled && *chat_enabled);
}

static void publish_to_inbox(const solar_os_link_message_t *message)
{
    if (!espnow_link.status.inbox_enabled ||
        message->type != SOLAR_OS_LINK_MESSAGE_TEXT) {
        return;
    }

    char sender[SOLAR_OS_INBOX_SENDER_MAX];
    char title[SOLAR_OS_INBOX_TITLE_MAX];
    char body[SOLAR_OS_INBOX_BODY_MAX];
    char dedupe_key[64];
    snprintf(sender, sizeof(sender), "%08" PRIx32, message->source);
    snprintf(title, sizeof(title), "SolarOS Link %s", espnow_link.status.link);
    snprintf(dedupe_key,
             sizeof(dedupe_key),
             "%08" PRIx32 ":%04x",
             message->source,
             message->sequence);
    const size_t body_len = message->payload_len < sizeof(body) - 1U ?
        message->payload_len : sizeof(body) - 1U;
    memcpy(body, message->payload, body_len);
    body[body_len] = '\0';

    const solar_os_inbox_publish_t entry = {
        .source = "link",
        .topic = espnow_link.status.link,
        .sender = sender,
        .title = title,
        .body = body,
        .dedupe_key = dedupe_key,
        .source_id = message->source,
        .source_context = message->sequence,
        .priority = SOLAR_OS_INBOX_PRIORITY_NORMAL,
    };
    const esp_err_t ret = solar_os_inbox_publish(&entry, NULL);
    espnow_link.status.last_error = ret;
    if (ret == ESP_OK) {
        espnow_link.status.inbox_published++;
    } else {
        SOLAR_OS_LOGW(TAG, "inbox publish failed: %s", esp_err_to_name(ret));
    }
}

static void note_transmit(const solar_os_link_frame_t *frame,
                          esp_err_t result,
                          uint32_t now_ms)
{
    if (espnow_link.status.chat_enabled) {
        solar_os_link_messaging_note_transmit(frame, result, now_ms);
    }
    espnow_link.status.last_error = result;
    if (result == ESP_OK) {
        espnow_link.status.transmitted++;
    } else {
        espnow_link.status.transmit_errors++;
        SOLAR_OS_LOGW(TAG, "send failed: %s", esp_err_to_name(result));
    }
}

static void process_pending_send(uint32_t now_ms)
{
    if (!espnow_link.pending_send) {
        return;
    }
    esp_err_t result = ESP_OK;
    const esp_err_t take = solar_os_espnow_take_send_result(&result, 0U);
    if (take == ESP_OK) {
        if (!espnow_link.pending_reported) {
            note_transmit(&espnow_link.pending_frame, result, now_ms);
        }
        espnow_link.pending_send = false;
        espnow_link.pending_reported = false;
        return;
    }
    if (take != ESP_ERR_TIMEOUT) {
        if (!espnow_link.pending_reported) {
            note_transmit(&espnow_link.pending_frame, take, now_ms);
        }
        espnow_link.pending_send = false;
        espnow_link.pending_reported = false;
        return;
    }
    if (!espnow_link.pending_reported &&
        now_ms - espnow_link.pending_since_ms >= ESPNOW_LINK_SEND_TIMEOUT_MS) {
        note_transmit(&espnow_link.pending_frame, ESP_ERR_TIMEOUT, now_ms);
        espnow_link.pending_reported = true;
    }
}

static void start_next_send(uint32_t now_ms)
{
    if (espnow_link.pending_send) {
        return;
    }
    solar_os_link_frame_t frame;
    esp_err_t ret = solar_os_link_take_tx(espnow_link.status.link, &frame, 0U);
    if (ret == ESP_ERR_TIMEOUT) {
        return;
    }
    if (ret != ESP_OK) {
        espnow_link.status.last_error = ret;
        espnow_link.status.transmit_errors++;
        return;
    }

    solar_os_link_message_t message;
    ret = solar_os_link_decode(frame.data, frame.len, &message);
    if (ret == ESP_OK) {
        ret = solar_os_espnow_send(message.destination, frame.data, frame.len);
    }
    if (ret != ESP_OK) {
        note_transmit(&frame, ret, now_ms);
        return;
    }
    espnow_link.pending_frame = frame;
    espnow_link.pending_send = true;
    espnow_link.pending_reported = false;
    espnow_link.pending_since_ms = now_ms;
}

static void process_receive(uint32_t now_ms)
{
    solar_os_espnow_packet_t packet;
    const esp_err_t receive = solar_os_espnow_receive(
        &packet,
        espnow_link.pending_send ? 0U : ESPNOW_LINK_POLL_MS);
    if (receive == ESP_ERR_TIMEOUT) {
        return;
    }
    if (receive != ESP_OK) {
        espnow_link.status.last_error = receive;
        espnow_link.status.receive_errors++;
        return;
    }

    solar_os_link_ingest_result_t result;
    const esp_err_t ret = solar_os_link_ingest(
        espnow_link.status.link, packet.data, packet.len, &result);
    if (ret == ESP_ERR_NOT_FOUND) {
        return;
    }
    espnow_link.status.last_error = ret;
    if (ret != ESP_OK) {
        espnow_link.status.receive_errors++;
        return;
    }

    const esp_err_t learn = solar_os_espnow_peer_learn(result.message.source,
                                                       packet.source_mac,
                                                       packet.rssi,
                                                       now_ms);
    if (learn != ESP_OK && learn != ESP_ERR_INVALID_STATE) {
        SOLAR_OS_LOGW(TAG, "peer learn failed: %s", esp_err_to_name(learn));
    }
    if (result.accepted && result.message.type == SOLAR_OS_LINK_MESSAGE_STREAM) {
        const esp_err_t stream_error = solar_os_link_stream_ingest(
            espnow_link.status.link, &result.message, now_ms);
        if (stream_error != ESP_OK && stream_error != ESP_ERR_NOT_FOUND &&
            stream_error != ESP_ERR_INVALID_STATE) {
            SOLAR_OS_LOGW(TAG,
                          "Link stream ingest failed: %s",
                          esp_err_to_name(stream_error));
        }
    }
    if (espnow_link.status.chat_enabled) {
        const esp_err_t chat_error =
            solar_os_link_messaging_note_ingest(&result, now_ms);
        if (chat_error != ESP_OK) {
            espnow_link.status.chat_errors++;
            SOLAR_OS_LOGW(TAG,
                          "Chat projection failed: %s",
                          esp_err_to_name(chat_error));
        }
    }
    if (result.accepted) {
        espnow_link.status.received++;
        publish_to_inbox(&result.message);
    }
}

static void espnow_link_task(void *arg)
{
    (void)arg;
    while (!espnow_link.stop_requested) {
        const uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        if (espnow_link.status.chat_enabled) {
            solar_os_link_messaging_process(now_ms);
        }
        solar_os_link_stream_process(espnow_link.status.link, now_ms);
        process_pending_send(now_ms);
        start_next_send(now_ms);
        process_receive(now_ms);
        if (espnow_link.pending_send) {
            vTaskDelay(1);
        }
    }
    espnow_link.task = NULL;
    solar_os_task_delete_internal(NULL);
}

static void cleanup_started_services(bool chat_started,
                                     bool link_created,
                                     bool espnow_started)
{
    if (chat_started) {
        solar_os_link_messaging_stop();
    }
    if (link_created) {
        solar_os_link_stream_transport_stopped(espnow_link.status.link);
        (void)solar_os_link_destroy(espnow_link.status.link);
    }
    if (espnow_started) {
        (void)solar_os_espnow_stop(espnow_link.owner);
    }
}

static esp_err_t espnow_link_start(solar_os_context_t *ctx,
                                   int argc,
                                   char **argv)
{
    (void)ctx;
    espnow_link.start_error_detail[0] = '\0';
    const char *link = NULL;
    uint8_t requested_channel = 0U;
    solar_os_espnow_phy_t phy = SOLAR_OS_ESPNOW_PHY_NORMAL;
    bool inbox_enabled = false;
    bool chat_enabled = false;
    if (!parse_args(argc,
                    argv,
                    &link,
                    &requested_channel,
                    &phy,
                    &inbox_enabled,
                    &chat_enabled)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (chat_enabled && !solar_os_link_messaging_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (report_fixed_channel_conflict(requested_channel)) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&espnow_link, 0, sizeof(espnow_link));
    strlcpy(espnow_link.status.link, link, sizeof(espnow_link.status.link));
    espnow_link.status.inbox_enabled = inbox_enabled;
    espnow_link.status.chat_enabled = chat_enabled;
    espnow_link.status.channel_auto = requested_channel == 0U;
    espnow_link.status.phy = phy;
    espnow_link.status.last_error = ESP_OK;

    esp_err_t ret = solar_os_jobs_owner_name(solar_os_espnow_link_job.name,
                                             espnow_link.owner,
                                             sizeof(espnow_link.owner));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_espnow_start(espnow_link.owner, requested_channel, phy);
    if (ret != ESP_OK) {
        return ret;
    }
    bool espnow_started = true;

    solar_os_espnow_status_t espnow_status;
    solar_os_espnow_get_status(&espnow_status);
    espnow_link.status.channel = espnow_status.channel;

    ret = solar_os_link_create(link,
                               solar_os_link_default_local_id(),
                               SOLAR_OS_ESPNOW_FRAME_MTU);
    bool link_created = ret == ESP_OK;
    if (ret == ESP_OK) {
        ret = solar_os_link_stream_init();
    }
    bool chat_started = false;
    if (ret == ESP_OK && chat_enabled) {
        ret = solar_os_link_messaging_start(link);
        chat_started = ret == ESP_OK;
    }
    if (ret != ESP_OK) {
        cleanup_started_services(chat_started, link_created, espnow_started);
        return ret;
    }

    espnow_link.status.running = true;
    if (solar_os_task_create_pinned_internal(espnow_link_task,
                                             "espnow_link",
                                             ESPNOW_LINK_TASK_STACK,
                                             NULL,
                                             tskIDLE_PRIORITY + 2,
                                             &espnow_link.task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        espnow_link.status.running = false;
        cleanup_started_services(chat_started, link_created, espnow_started);
        return ESP_ERR_NO_MEM;
    }

    (void)solar_os_jobs_note_resource(solar_os_espnow_link_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      "wifi",
                                      "ESP-NOW radio");
    (void)solar_os_jobs_note_resource(solar_os_espnow_link_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      link,
                                      "SolarOS Link");
    SOLAR_OS_LOGI(TAG,
                  "started link=%s channel=%u mode=%s phy=%s inbox=%s chat=%s mtu=%u",
                  link,
                  (unsigned)espnow_link.status.channel,
                  requested_channel == 0U ? "auto" : "fixed",
                  solar_os_espnow_phy_name(phy),
                  inbox_enabled ? "on" : "off",
                  chat_enabled ? "on" : "off",
                  (unsigned)SOLAR_OS_ESPNOW_FRAME_MTU);
    return ESP_OK;
}

static void espnow_link_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    espnow_link.stop_requested = true;
    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(ESPNOW_LINK_STOP_WAIT_MS);
    while (espnow_link.task != NULL &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(1);
    }
    if (espnow_link.task != NULL) {
        solar_os_task_delete_internal(espnow_link.task);
        espnow_link.task = NULL;
        espnow_link.status.last_error = ESP_ERR_TIMEOUT;
    }

    espnow_link.status.running = false;
    if (espnow_link.status.chat_enabled) {
        solar_os_link_messaging_stop();
    }
    solar_os_link_stream_transport_stopped(espnow_link.status.link);
    (void)solar_os_link_destroy(espnow_link.status.link);
    const esp_err_t stop_error = solar_os_espnow_stop(espnow_link.owner);
    if (stop_error != ESP_OK) {
        espnow_link.status.last_error = stop_error;
    }
    SOLAR_OS_LOGI(TAG,
                  "stopped tx=%" PRIu32 " rx=%" PRIu32
                  " tx-errors=%" PRIu32 " rx-errors=%" PRIu32,
                  espnow_link.status.transmitted,
                  espnow_link.status.received,
                  espnow_link.status.transmit_errors,
                  espnow_link.status.receive_errors);
}

void solar_os_espnow_link_job_get_status(
    solar_os_espnow_link_job_status_t *status)
{
    if (status != NULL) {
        *status = espnow_link.status;
    }
}

const solar_os_job_t solar_os_espnow_link_job = {
    .name = "espnow-link",
    .summary = "SolarOS Link ESP-NOW transport",
    .start = espnow_link_start,
    .stop = espnow_link_stop,
    .worker_stack_bytes = ESPNOW_LINK_TASK_STACK,
    .error_detail = espnow_link_error_detail,
};
