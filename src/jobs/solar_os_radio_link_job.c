#include "solar_os_radio_link_job.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_inbox.h"
#include "solar_os_jobs.h"
#include "solar_os_link_messaging.h"
#include "solar_os_link_repeater.h"
#include "solar_os_link_stream.h"
#include "solar_os_log.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"

/* Chat projection enters contacts and messaging services from this worker. */
#define RADIO_LINK_TASK_STACK 6144U
#define RADIO_LINK_RECEIVE_TIMEOUT_MS 10U
#define RADIO_LINK_SEND_TIMEOUT_MS 3000U
#define RADIO_LINK_STOP_WAIT_MS 3500U

static const char *TAG = "radio-link";

typedef struct {
    solar_os_radio_link_job_status_t status;
    solar_os_link_repeater_t repeater;
    solar_os_radio_status_t saved_radio;
    solar_os_radio_handle_t radio_handle;
    volatile bool stop_requested;
    TaskHandle_t task;
} radio_link_state_t;

static EXT_RAM_BSS_ATTR radio_link_state_t radio_link;

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

static bool parse_args(int argc,
                       char **argv,
                       const char **link,
                       const char **radio,
                       const char **profile,
                       bool *inbox_enabled,
                       bool *chat_enabled,
                       bool *repeater_enabled)
{
    int first = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_radio_link_job.name) == 0) {
        first = 1;
    }
    if (argc - first < 3 || argc - first > 6) {
        return false;
    }

    *link = argv[first];
    *radio = argv[first + 1];
    *profile = argv[first + 2];
    *inbox_enabled = false;
    *chat_enabled = false;
    *repeater_enabled = false;
    bool inbox_seen = false;
    bool chat_seen = false;
    bool repeater_seen = false;
    for (int index = first + 3; index < argc; index++) {
        static const char inbox_prefix[] = "inbox=";
        static const char chat_prefix[] = "chat=";
        static const char repeater_prefix[] = "repeater=";
        if (strncmp(argv[index], inbox_prefix, sizeof(inbox_prefix) - 1U) == 0) {
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
        } else if (strncmp(argv[index], repeater_prefix,
                           sizeof(repeater_prefix) - 1U) == 0) {
            if (repeater_seen ||
                !parse_on_off(argv[index] + sizeof(repeater_prefix) - 1U,
                              repeater_enabled)) {
                return false;
            }
            repeater_seen = true;
        } else {
            return false;
        }
    }
    return !(*inbox_enabled && *chat_enabled);
}

static bool radio_link_send_repeater_frame(uint32_t now_ms)
{
    if (!radio_link.status.repeater_enabled) {
        return false;
    }

    solar_os_link_frame_t received;
    if (!solar_os_link_repeater_take_due(&radio_link.repeater,
                                         now_ms,
                                         &received)) {
        return false;
    }

    solar_os_link_message_t message;
    esp_err_t ret = solar_os_link_decode(received.data,
                                         received.len,
                                         &message);
    solar_os_link_frame_t relayed;
    if (ret == ESP_OK) {
        message.flags |= SOLAR_OS_LINK_FLAG_RELAYED;
        ret = solar_os_link_encode(&message, &relayed);
    }
    if (ret == ESP_OK) {
        solar_os_radio_packet_t packet = {.len = relayed.len};
        memcpy(packet.data, relayed.data, relayed.len);
        ret = solar_os_radio_handle_send(&radio_link.radio_handle,
                                         &packet,
                                         RADIO_LINK_SEND_TIMEOUT_MS);
    } else {
        solar_os_link_repeater_note_invalid(&radio_link.repeater);
    }

    radio_link.status.last_error = ret;
    solar_os_link_repeater_note_transmit(&radio_link.repeater, ret == ESP_OK);
    if (ret == ESP_OK) {
        radio_link.status.transmitted++;
    } else {
        radio_link.status.transmit_errors++;
        SOLAR_OS_LOGW(TAG, "repeater send failed: %s", esp_err_to_name(ret));
    }
    return true;
}

static void radio_link_observe_repeater_frame(
    const solar_os_radio_packet_t *packet,
    uint32_t now_ms)
{
    if (!radio_link.status.repeater_enabled || packet == NULL) {
        return;
    }

    solar_os_link_message_t message;
    if (solar_os_link_decode(packet->data, packet->len, &message) != ESP_OK) {
        solar_os_link_repeater_note_invalid(&radio_link.repeater);
        return;
    }
    solar_os_link_frame_t received = {
        .len = packet->len,
    };
    memcpy(received.data, packet->data, packet->len);
    (void)solar_os_link_repeater_observe(&radio_link.repeater,
                                         &message,
                                         &received,
                                         now_ms,
                                         esp_random());
}

static void restore_radio(void)
{
    if (!solar_os_radio_handle_valid(&radio_link.radio_handle)) {
        return;
    }
    (void)solar_os_radio_handle_set_state(&radio_link.radio_handle, SOLAR_OS_RADIO_STATE_STANDBY);
    if (solar_os_radio_handle_configure(&radio_link.radio_handle, &radio_link.saved_radio.config) ==
            ESP_OK &&
        radio_link.saved_radio.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
        (void)solar_os_radio_handle_set_state(&radio_link.radio_handle,
                                              radio_link.saved_radio.state);
    }
}

static void publish_to_inbox(const solar_os_link_message_t *message)
{
    if (!radio_link.status.inbox_enabled || message->type != SOLAR_OS_LINK_MESSAGE_TEXT) {
        return;
    }

    char sender[SOLAR_OS_INBOX_SENDER_MAX];
    char title[SOLAR_OS_INBOX_TITLE_MAX];
    char body[SOLAR_OS_INBOX_BODY_MAX];
    char dedupe_key[64];
    snprintf(sender, sizeof(sender), "%08" PRIx32, message->source);
    snprintf(title, sizeof(title), "SolarOS Link %s", radio_link.status.link);
    snprintf(
        dedupe_key, sizeof(dedupe_key), "%08" PRIx32 ":%04x", message->source, message->sequence);
    const size_t body_len =
        message->payload_len < sizeof(body) - 1 ? message->payload_len : sizeof(body) - 1;
    memcpy(body, message->payload, body_len);
    body[body_len] = '\0';

    const solar_os_inbox_publish_t entry = {
        .source = "link",
        .topic = radio_link.status.link,
        .sender = sender,
        .title = title,
        .body = body,
        .dedupe_key = dedupe_key,
        .source_id = message->source,
        .source_context = message->sequence,
        .priority = SOLAR_OS_INBOX_PRIORITY_NORMAL,
    };
    const esp_err_t ret = solar_os_inbox_publish(&entry, NULL);
    radio_link.status.last_error = ret;
    if (ret == ESP_OK) {
        radio_link.status.inbox_published++;
    } else {
        SOLAR_OS_LOGW(TAG, "inbox publish failed: %s", esp_err_to_name(ret));
    }
}

static void radio_link_task(void *arg)
{
    (void)arg;

    while (!radio_link.stop_requested) {
        const uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        if (radio_link.status.chat_enabled) {
            solar_os_link_messaging_process(now_ms);
        }
        solar_os_link_stream_process(radio_link.status.link, now_ms);

        solar_os_link_frame_t frame;
        esp_err_t ret = solar_os_link_take_tx(radio_link.status.link, &frame, 0);
        if (ret == ESP_OK) {
            solar_os_radio_packet_t packet = {
                .len = frame.len,
            };
            memcpy(packet.data, frame.data, frame.len);
            ret = solar_os_radio_handle_send(
                &radio_link.radio_handle, &packet, RADIO_LINK_SEND_TIMEOUT_MS);
            if (radio_link.status.chat_enabled) {
                solar_os_link_messaging_note_transmit(&frame, ret, now_ms);
            }
            radio_link.status.last_error = ret;
            if (ret == ESP_OK) {
                radio_link.status.transmitted++;
            } else {
                radio_link.status.transmit_errors++;
                SOLAR_OS_LOGW(TAG, "send failed: %s", esp_err_to_name(ret));
            }
            continue;
        }
        if (ret != ESP_ERR_TIMEOUT) {
            radio_link.status.last_error = ret;
            radio_link.status.transmit_errors++;
        }

        if (radio_link_send_repeater_frame(now_ms)) {
            continue;
        }

        solar_os_radio_packet_t packet;
        ret = solar_os_radio_handle_receive(
            &radio_link.radio_handle, &packet, RADIO_LINK_RECEIVE_TIMEOUT_MS);
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (ret != ESP_OK) {
            radio_link.status.last_error = ret;
            radio_link.status.receive_errors++;
            continue;
        }
        if (!packet.crc_ok) {
            radio_link.status.last_error = ESP_ERR_INVALID_CRC;
            radio_link.status.receive_errors++;
            if (radio_link.status.repeater_enabled) {
                solar_os_link_repeater_note_invalid(&radio_link.repeater);
            }
            continue;
        }
        const uint32_t received_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        radio_link_observe_repeater_frame(&packet, received_ms);

        solar_os_link_ingest_result_t result;
        ret = solar_os_link_ingest(radio_link.status.link, packet.data, packet.len, &result);
        if (ret == ESP_ERR_NOT_FOUND) {
            continue;
        }
        radio_link.status.last_error = ret;
        if (ret != ESP_OK) {
            radio_link.status.receive_errors++;
            continue;
        }
        if (result.accepted &&
            result.message.type == SOLAR_OS_LINK_MESSAGE_STREAM) {
            const esp_err_t stream_error = solar_os_link_stream_ingest(
                radio_link.status.link, &result.message, received_ms);
            if (stream_error != ESP_OK && stream_error != ESP_ERR_NOT_FOUND &&
                stream_error != ESP_ERR_INVALID_STATE) {
                SOLAR_OS_LOGW(TAG,
                              "Link stream ingest failed: %s",
                              esp_err_to_name(stream_error));
            }
        }
        if (radio_link.status.chat_enabled) {
            const esp_err_t chat_error =
                solar_os_link_messaging_note_ingest(&result, received_ms);
            if (chat_error != ESP_OK) {
                radio_link.status.chat_errors++;
                SOLAR_OS_LOGW(TAG,
                              "Chat projection failed: %s",
                              esp_err_to_name(chat_error));
            }
        }
        if (result.accepted) {
            radio_link.status.received++;
            publish_to_inbox(&result.message);
        }
    }

    radio_link.task = NULL;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t radio_link_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    const char *link = NULL;
    const char *radio = NULL;
    const char *profile = NULL;
    bool inbox_enabled = false;
    bool chat_enabled = false;
    bool repeater_enabled = false;
    if (!parse_args(argc,
                    argv,
                    &link,
                    &radio,
                    &profile,
                    &inbox_enabled,
                    &chat_enabled,
                    &repeater_enabled)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (chat_enabled && !solar_os_link_messaging_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_radio_info_t info;
    esp_err_t ret = solar_os_radio_get_info(radio, &info);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((info.features & SOLAR_OS_RADIO_FEATURE_PACKET) == 0 ||
        (info.features & SOLAR_OS_RADIO_FEATURE_CONTINUOUS_RX) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char owner[SOLAR_OS_JOB_OWNER_MAX];
    ret = solar_os_jobs_owner_name(solar_os_radio_link_job.name, owner, sizeof(owner));
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_radio_handle_t handle = SOLAR_OS_RADIO_HANDLE_INIT;
    ret = solar_os_radio_claim(radio, owner, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_radio_status_t saved;
    ret = solar_os_radio_handle_get_status(&handle, &saved);
    if (ret == ESP_OK) {
        ret = solar_os_radio_handle_profile_apply(&handle, profile);
    }
    solar_os_radio_status_t configured;
    if (ret == ESP_OK) {
        ret = solar_os_radio_handle_get_status(&handle, &configured);
    }
    if (ret != ESP_OK) {
        (void)solar_os_radio_release(&handle);
        return ret;
    }

    size_t frame_mtu = info.max_packet_len;
    if (configured.config.payload_length > 0 && configured.config.payload_length < frame_mtu) {
        frame_mtu = configured.config.payload_length;
    }
    ret = solar_os_link_create(link, solar_os_link_default_local_id(), frame_mtu);
    if (ret != ESP_OK) {
        (void)solar_os_radio_handle_configure(&handle, &saved.config);
        if (saved.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
            (void)solar_os_radio_handle_set_state(&handle, saved.state);
        }
        (void)solar_os_radio_release(&handle);
        return ret;
    }
    ret = solar_os_link_stream_init();
    if (ret != ESP_OK) {
        (void)solar_os_link_destroy(link);
        (void)solar_os_radio_handle_configure(&handle, &saved.config);
        if (saved.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
            (void)solar_os_radio_handle_set_state(&handle, saved.state);
        }
        (void)solar_os_radio_release(&handle);
        return ret;
    }
    if (chat_enabled) {
        ret = solar_os_link_messaging_start(link);
        if (ret != ESP_OK) {
            (void)solar_os_link_destroy(link);
            (void)solar_os_radio_handle_configure(&handle, &saved.config);
            if (saved.state != SOLAR_OS_RADIO_STATE_UNKNOWN) {
                (void)solar_os_radio_handle_set_state(&handle, saved.state);
            }
            (void)solar_os_radio_release(&handle);
            return ret;
        }
    }

    memset(&radio_link, 0, sizeof(radio_link));
    radio_link.radio_handle = handle;
    radio_link.saved_radio = saved;
    radio_link.status.running = true;
    radio_link.status.inbox_enabled = inbox_enabled;
    radio_link.status.chat_enabled = chat_enabled;
    radio_link.status.repeater_enabled = repeater_enabled;
    radio_link.status.last_error = ESP_OK;
    strlcpy(radio_link.status.link, link, sizeof(radio_link.status.link));
    strlcpy(radio_link.status.radio, radio, sizeof(radio_link.status.radio));
    strlcpy(radio_link.status.profile, profile, sizeof(radio_link.status.profile));
    solar_os_link_repeater_reset(&radio_link.repeater,
                                 solar_os_link_default_local_id());

    if (solar_os_task_create_pinned_internal(radio_link_task,
                                             "radio_link",
                                             RADIO_LINK_TASK_STACK,
                                             NULL,
                                             tskIDLE_PRIORITY + 2,
                                             &radio_link.task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        radio_link.status.running = false;
        radio_link.status.last_error = ESP_ERR_NO_MEM;
        if (chat_enabled) {
            solar_os_link_messaging_stop();
        }
        solar_os_link_stream_transport_stopped(link);
        (void)solar_os_link_destroy(link);
        restore_radio();
        (void)solar_os_radio_release(&radio_link.radio_handle);
        return ESP_ERR_NO_MEM;
    }

    (void)solar_os_jobs_note_resource(
        solar_os_radio_link_job.name, SOLAR_OS_JOB_RESOURCE_CUSTOM, radio, "packet radio");
    (void)solar_os_jobs_note_resource(
        solar_os_radio_link_job.name, SOLAR_OS_JOB_RESOURCE_CUSTOM, link, "SolarOS Link");
    SOLAR_OS_LOGI(TAG,
                  "started link=%s radio=%s profile=%s inbox=%s chat=%s repeater=%s mtu=%u",
                  link,
                  radio,
                  profile,
                  inbox_enabled ? "on" : "off",
                  chat_enabled ? "on" : "off",
                  repeater_enabled ? "on" : "off",
                  (unsigned)frame_mtu);
    return ESP_OK;
}

static void radio_link_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    radio_link.stop_requested = true;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RADIO_LINK_STOP_WAIT_MS);
    while (radio_link.task != NULL && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(1);
    }
    if (radio_link.task != NULL) {
        solar_os_task_delete_internal(radio_link.task);
        radio_link.task = NULL;
        radio_link.status.last_error = ESP_ERR_TIMEOUT;
    }

    radio_link.status.running = false;
    if (radio_link.status.chat_enabled) {
        solar_os_link_messaging_stop();
    }
    solar_os_link_stream_transport_stopped(radio_link.status.link);
    (void)solar_os_link_destroy(radio_link.status.link);
    restore_radio();
    (void)solar_os_radio_release(&radio_link.radio_handle);
    SOLAR_OS_LOGI(TAG,
                  "stopped tx=%" PRIu32 " rx=%" PRIu32 " repeated=%" PRIu32
                  " tx-errors=%" PRIu32 " rx-errors=%" PRIu32,
                  radio_link.status.transmitted,
                  radio_link.status.received,
                  radio_link.repeater.repeated,
                  radio_link.status.transmit_errors,
                  radio_link.status.receive_errors);
}

void solar_os_radio_link_job_get_status(solar_os_radio_link_job_status_t *status)
{
    if (status != NULL) {
        *status = radio_link.status;
        solar_os_link_repeater_status_t repeater_status;
        solar_os_link_repeater_get_status(&radio_link.repeater,
                                          &repeater_status);
        status->repeater_queued = repeater_status.queued;
        status->repeated = repeater_status.repeated;
        status->repeater_suppressed = repeater_status.suppressed;
        status->repeater_queue_drops = repeater_status.queue_drops;
        status->repeater_invalid_frames = repeater_status.invalid_frames;
    }
}

static void radio_link_print_status(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    solar_os_radio_link_job_status_t status;
    solar_os_radio_link_job_get_status(&status);
    solar_os_shell_io_printf(
        io,
        "  radio-link: link=%s radio=%s profile=%s inbox=%s chat=%s repeater=%s\n",
        status.link[0] != '\0' ? status.link : "-",
        status.radio[0] != '\0' ? status.radio : "-",
        status.profile[0] != '\0' ? status.profile : "-",
        status.inbox_enabled ? "on" : "off",
        status.chat_enabled ? "on" : "off",
        status.repeater_enabled ? "on" : "off");
    solar_os_shell_io_printf(
        io,
        "  packets: tx=%" PRIu32 " rx=%" PRIu32
        " tx-errors=%" PRIu32 " rx-errors=%" PRIu32 "\n",
        status.transmitted,
        status.received,
        status.transmit_errors,
        status.receive_errors);
    solar_os_shell_io_printf(
        io,
        "  repeater: forwarded=%" PRIu32 " suppressed=%" PRIu32
        " queued=%u queue-drops=%" PRIu32 " invalid=%" PRIu32 "\n",
        status.repeated,
        status.repeater_suppressed,
        (unsigned)status.repeater_queued,
        status.repeater_queue_drops,
        status.repeater_invalid_frames);
}

const solar_os_job_t solar_os_radio_link_job = {
    .name = "radio-link",
    .summary = "SolarOS Link packet radio transport",
    .start = radio_link_start,
    .stop = radio_link_stop,
    .detail = radio_link_print_status,
    .worker_stack_bytes = RADIO_LINK_TASK_STACK,
};
