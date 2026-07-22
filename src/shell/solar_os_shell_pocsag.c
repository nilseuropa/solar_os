#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_pocsag_job.h"
#include "solar_os_memory.h"
#include "solar_os_shell_io.h"

#define POCSAG_DEFAULT_DEVIATION_HZ 4500U
#define POCSAG_DEFAULT_BANDWIDTH_HZ 10500U
#define POCSAG_PREAMBLE_BYTES 72U
#define POCSAG_RIC_MAX 2097151U

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

static void print_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage: pocsag status");
    solar_os_shell_io_writeln(
        io,
        "       pocsag send <radio> <frequency-hz> <baud> <ric> <message> [alpha|numeric] [normal|inverted] [function]");
    solar_os_shell_io_writeln(
        io,
        "start: job start pocsag <radio> <frequency-hz> <baud> <ric> [alpha|numeric] [normal|inverted]");
}

static esp_err_t restore_radio(const char *radio, const solar_os_radio_status_t *saved)
{
    (void)solar_os_radio_set_state(radio, SOLAR_OS_RADIO_STATE_STANDBY);
    esp_err_t err = solar_os_radio_configure(radio, &saved->config);
    if (err == ESP_OK) {
        err = solar_os_radio_set_state(radio, saved->state);
    }
    return err;
}

static void pocsag_send(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc < 7 || argc > 10) {
        print_usage(io);
        return;
    }

    const char *radio = argv[2];
    const char *message = argv[6];
    uint32_t frequency_hz = 0;
    uint32_t baud = 0;
    uint32_t ric = 0;
    uint32_t function_value = 0;
    if (!parse_u32(argv[3], 290000000U, 1020000000U, &frequency_hz) ||
        !parse_u32(argv[4], 512U, 2400U, &baud) ||
        (baud != 512U && baud != 1200U && baud != 2400U) ||
        !parse_u32(argv[5], 0, POCSAG_RIC_MAX, &ric)) {
        print_usage(io);
        return;
    }

    solar_os_pocsag_format_t format = SOLAR_OS_POCSAG_FORMAT_ALPHA;
    bool inverted = false;
    if (argc >= 8) {
        if (strcmp(argv[7], "numeric") == 0) {
            format = SOLAR_OS_POCSAG_FORMAT_NUMERIC;
        } else if (strcmp(argv[7], "alpha") != 0) {
            print_usage(io);
            return;
        }
    }
    if (argc >= 9) {
        if (strcmp(argv[8], "inverted") == 0) {
            inverted = true;
        } else if (strcmp(argv[8], "normal") != 0) {
            print_usage(io);
            return;
        }
    }
    uint8_t function = format == SOLAR_OS_POCSAG_FORMAT_ALPHA ? 3U : 0U;
    if (argc == 10) {
        if (!parse_u32(argv[9], 0, 3, &function_value)) {
            print_usage(io);
            return;
        }
        function = (uint8_t)function_value;
    }

    solar_os_radio_info_t info;
    esp_err_t err = solar_os_radio_get_info(radio, &info);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "pocsag send: %s\n", esp_err_to_name(err));
        return;
    }
    if ((info.modulations & SOLAR_OS_RADIO_MODULATION_FSK) == 0 ||
        (info.features & SOLAR_OS_RADIO_FEATURE_CONTINUOUS_TX) == 0) {
        solar_os_shell_io_writeln(io, "pocsag send: radio does not support continuous FSK transmit");
        return;
    }

    solar_os_pocsag_job_status_t receiver;
    solar_os_pocsag_job_get_status(&receiver);
    if (receiver.running && strcmp(receiver.radio, radio) == 0) {
        solar_os_shell_io_writeln(io, "pocsag send: radio is used by the receiver; stop the pocsag job first");
        return;
    }

    uint8_t *payload = solar_os_memory_alloc(SOLAR_OS_POCSAG_PAYLOAD_MAX,
                                             SOLAR_OS_MEMORY_TRANSIENT,
                                             "pocsag.payload");
    if (payload == NULL) {
        solar_os_shell_io_writeln(io, "pocsag send: ESP_ERR_NO_MEM");
        return;
    }
    size_t payload_len = 0;
    size_t batches = 0;
    err = solar_os_pocsag_encode_payload(ric,
                                         function,
                                         format,
                                         message,
                                         payload,
                                         SOLAR_OS_POCSAG_PAYLOAD_MAX,
                                         &payload_len,
                                         &batches);
    if (err != ESP_OK) {
        solar_os_memory_free(payload);
        solar_os_shell_io_printf(io, "pocsag send: invalid message: %s\n", esp_err_to_name(err));
        return;
    }
    if (inverted) {
        for (size_t i = 0; i < payload_len; i++) {
            payload[i] = (uint8_t)~payload[i];
        }
    }

    solar_os_radio_status_t saved;
    err = solar_os_radio_get_status(radio, &saved);
    if (err != ESP_OK) {
        solar_os_memory_free(payload);
        solar_os_shell_io_printf(io, "pocsag send: %s\n", esp_err_to_name(err));
        return;
    }

    static const uint8_t sync_normal[4] = {0x7C, 0xD2, 0x15, 0xD8};
    static const uint8_t sync_inverted[4] = {0x83, 0x2D, 0xEA, 0x27};
    solar_os_radio_config_t config = saved.config;
    config.frequency_hz = frequency_hz;
    config.modulation = SOLAR_OS_RADIO_MODULATION_FSK;
    config.bitrate_bps = baud;
    config.deviation_hz = POCSAG_DEFAULT_DEVIATION_HZ;
    config.rx_bandwidth_hz = POCSAG_DEFAULT_BANDWIDTH_HZ;
    config.preamble_len = POCSAG_PREAMBLE_BYTES;
    config.sync_word_len = 4;
    memcpy(config.sync_word, inverted ? sync_inverted : sync_normal, 4);
    config.crc_enabled = false;
    config.variable_length = false;
    config.payload_length = 0;
    config.has_node_id = false;
    config.has_network_id = false;

    err = solar_os_radio_configure(radio, &config);
    if (err == ESP_OK) {
        const uint64_t bits = (uint64_t)(POCSAG_PREAMBLE_BYTES + 4U + payload_len) * 8U;
        const uint32_t timeout_ms = (uint32_t)((bits * 1000U + baud - 1U) / baud) + 1500U;
        err = solar_os_radio_send_stream(radio, payload, payload_len, timeout_ms);
    }
    const esp_err_t restore_err = restore_radio(radio, &saved);
    solar_os_memory_free(payload);
    if (err == ESP_OK) {
        err = restore_err;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "pocsag send: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(io,
                             "POCSAG sent: RIC %" PRIu32 "/F%u, %s, %" PRIu32
                             " baud, %u batch%s\n",
                             ric,
                             function,
                             solar_os_pocsag_format_name(format),
                             baud,
                             (unsigned)batches,
                             batches == 1 ? "" : "es");
}

void solar_os_shell_cmd_pocsag(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (argc >= 2 && strcmp(argv[1], "send") == 0) {
        pocsag_send(io, argc, argv);
        return;
    }
    if (argc != 2 || strcmp(argv[1], "status") != 0) {
        print_usage(io);
        return;
    }

    solar_os_pocsag_job_status_t status;
    solar_os_pocsag_job_get_status(&status);
    if (!status.running) {
        solar_os_shell_io_writeln(io, "POCSAG receiver: stopped");
        return;
    }

    solar_os_shell_io_printf(io,
                             "POCSAG receiver: running on %s\n"
                             "Frequency: %" PRIu32 " Hz, baud: %" PRIu32
                             ", RIC: %" PRIu32 ", format: %s, polarity: %s\n"
                             "Batches: %" PRIu32 ", messages: %" PRIu32
                             ", duplicates: %" PRIu32 ", receive errors: %" PRIu32 "\n"
                             "Corrected codewords: %" PRIu32
                             ", uncorrectable codewords: %" PRIu32
                             ", last RSSI: %d dBm, last error: %s\n",
                             status.radio,
                             status.frequency_hz,
                             status.baud,
                             status.ric,
                             solar_os_pocsag_format_name(status.format),
                             status.inverted ? "inverted" : "normal",
                             status.batches,
                             status.messages,
                             status.duplicates,
                             status.receive_errors,
                             status.corrected_codewords,
                             status.uncorrectable_codewords,
                             status.last_rssi_dbm,
                             esp_err_to_name(status.last_error));
}
