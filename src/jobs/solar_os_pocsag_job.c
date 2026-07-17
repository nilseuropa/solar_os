#include "solar_os_pocsag_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_inbox.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_radio.h"

#define POCSAG_DEFAULT_DEVIATION_HZ 4500U
#define POCSAG_DEFAULT_BANDWIDTH_HZ 10500U
#define POCSAG_MESSAGE_FLUSH_MS 1500U
#define POCSAG_DUPLICATE_WINDOW_MS 30000U
#define POCSAG_RIC_MAX 2097151U

static const char *TAG = "solar_os_pocsag";

typedef struct {
    solar_os_pocsag_job_status_t status;
    solar_os_radio_status_t saved_radio;
    solar_os_pocsag_decoder_t decoder;
    uint32_t last_batch_ms;
    uint32_t last_message_ms;
    uint32_t last_message_ric;
    uint8_t last_message_function;
    char last_message_text[SOLAR_OS_INBOX_BODY_MAX];
} pocsag_job_state_t;

static pocsag_job_state_t pocsag;

static bool parse_u32(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_args(int argc,
                       char **argv,
                       const char **radio,
                       uint32_t *frequency_hz,
                       uint32_t *baud,
                       uint32_t *ric,
                       solar_os_pocsag_format_t *format,
                       bool *inverted)
{
    int first = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_pocsag_job.name) == 0) {
        first = 1;
    }
    if (argc - first < 4 || argc - first > 6) {
        return false;
    }

    *radio = argv[first];
    if (!parse_u32(argv[first + 1], 290000000U, 1020000000U, frequency_hz) ||
        !parse_u32(argv[first + 2], 512U, 2400U, baud) ||
        !parse_u32(argv[first + 3], 1U, POCSAG_RIC_MAX, ric)) {
        return false;
    }

    *format = SOLAR_OS_POCSAG_FORMAT_ALPHA;
    *inverted = false;
    if (argc - first >= 5) {
        if (strcmp(argv[first + 4], "alpha") == 0) {
            *format = SOLAR_OS_POCSAG_FORMAT_ALPHA;
        } else if (strcmp(argv[first + 4], "numeric") == 0) {
            *format = SOLAR_OS_POCSAG_FORMAT_NUMERIC;
        } else {
            return false;
        }
    }
    if (argc - first == 6) {
        if (strcmp(argv[first + 5], "inverted") == 0) {
            *inverted = true;
        } else if (strcmp(argv[first + 5], "normal") != 0) {
            return false;
        }
    }
    return true;
}

static void restore_radio(void)
{
    if (pocsag.status.radio[0] == '\0') {
        return;
    }
    (void)solar_os_radio_set_state(pocsag.status.radio, SOLAR_OS_RADIO_STATE_STANDBY);
    if (solar_os_radio_configure(pocsag.status.radio, &pocsag.saved_radio.config) == ESP_OK) {
        (void)solar_os_radio_set_state(pocsag.status.radio, pocsag.saved_radio.state);
    }
}

static void publish_message(const solar_os_pocsag_message_t *message, void *user)
{
    (void)user;
    const uint32_t now_ms = pocsag.last_batch_ms;
    if (message->ric == pocsag.last_message_ric &&
        message->function == pocsag.last_message_function &&
        strcmp(message->text, pocsag.last_message_text) == 0 &&
        (uint32_t)(now_ms - pocsag.last_message_ms) < POCSAG_DUPLICATE_WINDOW_MS) {
        pocsag.status.duplicates++;
        return;
    }

    char topic[SOLAR_OS_INBOX_TOPIC_MAX];
    char sender[SOLAR_OS_INBOX_SENDER_MAX];
    char title[SOLAR_OS_INBOX_TITLE_MAX];
    snprintf(topic, sizeof(topic), "%lu.%03luMHz",
             (unsigned long)(pocsag.status.frequency_hz / 1000000U),
             (unsigned long)((pocsag.status.frequency_hz / 1000U) % 1000U));
    snprintf(sender, sizeof(sender), "RIC %" PRIu32 "/F%u", message->ric, message->function);
    snprintf(title, sizeof(title), "POCSAG %" PRIu32, pocsag.status.baud);

    const solar_os_inbox_publish_t inbox_message = {
        .source = "pocsag",
        .topic = topic,
        .sender = sender,
        .title = title,
        .body = message->text,
        .priority = SOLAR_OS_INBOX_PRIORITY_NORMAL,
    };
    const esp_err_t err = solar_os_inbox_publish(&inbox_message, NULL);
    pocsag.status.last_error = err;
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "inbox publish failed: %s", esp_err_to_name(err));
        return;
    }

    pocsag.status.messages++;
    pocsag.status.corrected_codewords += message->corrected_codewords;
    pocsag.status.uncorrectable_codewords += message->uncorrectable_codewords;
    pocsag.last_message_ms = now_ms;
    pocsag.last_message_ric = message->ric;
    pocsag.last_message_function = message->function;
    strlcpy(pocsag.last_message_text, message->text, sizeof(pocsag.last_message_text));
    SOLAR_OS_LOGI(TAG,
                  "message ric=%" PRIu32 " function=%u rssi=%d text=\"%s\"",
                  message->ric,
                  message->function,
                  message->rssi_dbm,
                  message->text);
}

static esp_err_t pocsag_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    const char *radio = NULL;
    uint32_t frequency_hz = 0;
    uint32_t baud = 0;
    uint32_t ric = 0;
    solar_os_pocsag_format_t format = SOLAR_OS_POCSAG_FORMAT_ALPHA;
    bool inverted = false;
    if (!parse_args(argc, argv, &radio, &frequency_hz, &baud, &ric, &format, &inverted)) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_radio_info_t info;
    esp_err_t err = solar_os_radio_get_info(radio, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (info.max_packet_len < SOLAR_OS_POCSAG_BATCH_BYTES ||
        (info.modulations & SOLAR_OS_RADIO_MODULATION_FSK) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_radio_status_t saved;
    err = solar_os_radio_get_status(radio, &saved);
    if (err != ESP_OK) {
        return err;
    }

    solar_os_radio_config_t config = saved.config;
    static const uint8_t sync_normal[4] = {0x7C, 0xD2, 0x15, 0xD8};
    static const uint8_t sync_inverted[4] = {0x83, 0x2D, 0xEA, 0x27};
    config.frequency_hz = frequency_hz;
    config.modulation = SOLAR_OS_RADIO_MODULATION_FSK;
    config.bitrate_bps = baud;
    config.deviation_hz = POCSAG_DEFAULT_DEVIATION_HZ;
    config.rx_bandwidth_hz = POCSAG_DEFAULT_BANDWIDTH_HZ;
    config.preamble_len = 0;
    config.sync_word_len = 4;
    memcpy(config.sync_word, inverted ? sync_inverted : sync_normal, 4);
    config.crc_enabled = false;
    config.variable_length = false;
    config.payload_length = SOLAR_OS_POCSAG_BATCH_BYTES;
    config.has_node_id = false;
    config.has_network_id = false;

    err = solar_os_radio_configure(radio, &config);
    if (err == ESP_OK) {
        err = solar_os_radio_set_state(radio, SOLAR_OS_RADIO_STATE_RX);
    }
    if (err != ESP_OK) {
        (void)solar_os_radio_configure(radio, &saved.config);
        (void)solar_os_radio_set_state(radio, saved.state);
        return err;
    }

    memset(&pocsag, 0, sizeof(pocsag));
    pocsag.saved_radio = saved;
    pocsag.status.running = true;
    strlcpy(pocsag.status.radio, radio, sizeof(pocsag.status.radio));
    pocsag.status.frequency_hz = frequency_hz;
    pocsag.status.baud = baud;
    pocsag.status.ric = ric;
    pocsag.status.format = format;
    pocsag.status.inverted = inverted;
    pocsag.status.last_error = ESP_OK;
    solar_os_pocsag_decoder_init(&pocsag.decoder, ric, format);
    (void)solar_os_jobs_note_resource(solar_os_pocsag_job.name,
                                      SOLAR_OS_JOB_RESOURCE_CUSTOM,
                                      radio,
                                      "POCSAG RX");
    SOLAR_OS_LOGI(TAG,
                  "started: radio=%s frequency=%" PRIu32 " baud=%" PRIu32
                  " ric=%" PRIu32 " format=%s polarity=%s",
                  radio,
                  frequency_hz,
                  baud,
                  ric,
                  solar_os_pocsag_format_name(format),
                  inverted ? "inverted" : "normal");
    return ESP_OK;
}

static void pocsag_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    pocsag.status.running = false;
    (void)solar_os_pocsag_decoder_flush(&pocsag.decoder, publish_message, NULL);
    restore_radio();
    SOLAR_OS_LOGI(TAG,
                  "stopped: batches=%" PRIu32 " messages=%" PRIu32 " errors=%" PRIu32,
                  pocsag.status.batches,
                  pocsag.status.messages,
                  pocsag.status.receive_errors);
}

static bool pocsag_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (!pocsag.status.running || event == NULL || event->type != SOLAR_OS_EVENT_TICK) {
        return false;
    }

    solar_os_radio_packet_t packet;
    const esp_err_t err = solar_os_radio_receive(pocsag.status.radio, &packet, 0);
    if (err == ESP_ERR_TIMEOUT) {
        if (pocsag.decoder.active && pocsag.last_batch_ms != 0 &&
            (uint32_t)(event->data.tick_ms - pocsag.last_batch_ms) >= POCSAG_MESSAGE_FLUSH_MS) {
            (void)solar_os_pocsag_decoder_flush(&pocsag.decoder, publish_message, NULL);
        }
        return false;
    }
    pocsag.status.last_error = err;
    if (err != ESP_OK) {
        pocsag.status.receive_errors++;
        return false;
    }

    pocsag.status.batches++;
    pocsag.status.last_rssi_dbm = packet.has_rssi ? packet.rssi_dbm : 0;
    pocsag.last_batch_ms = event->data.tick_ms;
    if (pocsag.status.inverted) {
        for (size_t i = 0; i < packet.len; i++) {
            packet.data[i] = (uint8_t)~packet.data[i];
        }
    }
    (void)solar_os_pocsag_decode_batch(&pocsag.decoder,
                                       packet.data,
                                       packet.len,
                                       pocsag.status.last_rssi_dbm,
                                       publish_message,
                                       NULL);
    return true;
}

void solar_os_pocsag_job_get_status(solar_os_pocsag_job_status_t *status)
{
    if (status != NULL) {
        *status = pocsag.status;
    }
}

const solar_os_job_t solar_os_pocsag_job = {
    .name = "pocsag",
    .summary = "POCSAG pager receiver",
    .start = pocsag_start,
    .stop = pocsag_stop,
    .event = pocsag_event,
};
