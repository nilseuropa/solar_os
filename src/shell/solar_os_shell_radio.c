#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell_tui_apps.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_radio.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void radio_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  radio");
    solar_os_shell_io_writeln(term, "  radio status|list");
    solar_os_shell_io_writeln(term, "  radio status <name>");
    solar_os_shell_io_writeln(term, "  radio config <name> [field value]");
    solar_os_shell_io_writeln(term, "  radio state <name> [sleep|standby|rx|tx]");
    solar_os_shell_io_writeln(term, "  radio send <name> <text|byte...>");
    solar_os_shell_io_writeln(term, "  radio recv <name> [timeout-ms]");
}

static bool parse_u32_arg(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_i32_arg(const char *text, int32_t min, int32_t max, int32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (int32_t)parsed;
    return true;
}

static bool token_equals_ci(const char *start, const char *end, const char *token)
{
    const char *p = start;
    const char *q = token;

    while (p < end && *q != '\0') {
        if (tolower((unsigned char)*p) != tolower((unsigned char)*q)) {
            return false;
        }
        p++;
        q++;
    }
    return p == end && *q == '\0';
}

static bool parse_frequency_arg(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    const char *start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (start == end) {
        return false;
    }

    const char *p = start;
    uint64_t whole = 0;
    bool has_digit = false;
    while (p < end && isdigit((unsigned char)*p)) {
        const uint64_t digit = (uint64_t)(*p - '0');
        if (whole > (UINT64_MAX - digit) / 10ULL) {
            return false;
        }
        whole = whole * 10ULL + digit;
        has_digit = true;
        p++;
    }
    if (!has_digit) {
        return false;
    }

    uint64_t fraction = 0;
    uint64_t fraction_scale = 1;
    bool has_fraction = false;
    if (p < end && *p == '.') {
        p++;
        while (p < end && isdigit((unsigned char)*p)) {
            if (fraction_scale > 100000000ULL) {
                return false;
            }
            fraction = fraction * 10ULL + (uint64_t)(*p - '0');
            fraction_scale *= 10ULL;
            has_fraction = true;
            p++;
        }
        if (!has_fraction) {
            return false;
        }
    }

    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }

    uint64_t multiplier = 1;
    if (p == end) {
        multiplier = 1;
    } else if (token_equals_ci(p, end, "hz")) {
        multiplier = 1;
    } else if (token_equals_ci(p, end, "k") || token_equals_ci(p, end, "khz")) {
        multiplier = 1000ULL;
    } else if (token_equals_ci(p, end, "m") || token_equals_ci(p, end, "mhz")) {
        multiplier = 1000000ULL;
    } else {
        return false;
    }

    if (has_fraction && multiplier == 1) {
        return false;
    }
    if (whole > UINT64_MAX / multiplier) {
        return false;
    }
    uint64_t hz = whole * multiplier;
    if (has_fraction) {
        if (fraction > UINT64_MAX / multiplier) {
            return false;
        }
        const uint64_t fractional_hz = (fraction * multiplier + fraction_scale / 2ULL) / fraction_scale;
        if (hz > UINT64_MAX - fractional_hz) {
            return false;
        }
        hz += fractional_hz;
    }
    if (hz < min || hz > max) {
        return false;
    }

    *value = (uint32_t)hz;
    return true;
}

static bool parse_bool_arg(const char *text, bool *value)
{
    if (text == NULL || value == NULL) {
        return false;
    }
    if (strcmp(text, "on") == 0 || strcmp(text, "true") == 0 || strcmp(text, "1") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "false") == 0 || strcmp(text, "0") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool token_has_hex_alpha(const char *text)
{
    for (const char *p = text; p != NULL && *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
            return true;
        }
    }
    return false;
}

static bool parse_byte_token(const char *text, uint8_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    int base = 0;
    if (strncmp(text, "0x", 2) != 0 && strncmp(text, "0X", 2) != 0 && token_has_hex_alpha(text)) {
        base = 16;
    }

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > 255) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static void radio_print_error(solar_os_shell_io_t *term, const char *prefix, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        solar_os_shell_io_printf(term, "%s: radio not found\n", prefix);
        break;
    case ESP_ERR_NOT_SUPPORTED:
        solar_os_shell_io_printf(term, "%s: operation not supported by driver\n", prefix);
        break;
    case ESP_ERR_INVALID_ARG:
        solar_os_shell_io_printf(term, "%s: invalid argument\n", prefix);
        break;
    case ESP_ERR_INVALID_SIZE:
        solar_os_shell_io_printf(term, "%s: packet too large\n", prefix);
        break;
    case ESP_ERR_TIMEOUT:
        solar_os_shell_io_printf(term, "%s: timeout\n", prefix);
        break;
    default:
        solar_os_shell_io_printf(term, "%s failed: %s\n", prefix, esp_err_to_name(err));
        break;
    }
}

static void radio_print_sync_word(solar_os_shell_io_t *term, const solar_os_radio_config_t *config)
{
    solar_os_shell_io_write(term, "sync=");
    if (config->sync_word_len == 0) {
        solar_os_shell_io_write(term, "none");
        return;
    }
    for (uint8_t i = 0; i < config->sync_word_len; i++) {
        solar_os_shell_io_printf(term, "%s%02x", i == 0 ? "" : ":", config->sync_word[i]);
    }
}

static void radio_print_config(solar_os_shell_io_t *term, const solar_os_radio_config_t *config)
{
    solar_os_shell_io_printf(term,
                             "frequency=%" PRIu32 " modulation=%s bitrate=%" PRIu32
                             " deviation=%" PRIu32 " bandwidth=%" PRIu32
                             " power=%d crc=%s preamble=%u variable=%s ",
                             config->frequency_hz,
                             solar_os_radio_modulation_name(config->modulation),
                             config->bitrate_bps,
                             config->deviation_hz,
                             config->rx_bandwidth_hz,
                             config->tx_power_dbm,
                             config->crc_enabled ? "on" : "off",
                             config->preamble_len,
                             config->variable_length ? "on" : "off");
    radio_print_sync_word(term, config);
    if (config->has_node_id) {
        solar_os_shell_io_printf(term, " node=%u", config->node_id);
    }
    if (config->has_network_id) {
        solar_os_shell_io_printf(term, " network=%u", config->network_id);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void radio_print_device(solar_os_shell_io_t *term, const solar_os_radio_info_t *info, bool verbose)
{
    solar_os_radio_status_t status;
    char modulations[64];
    char features[128];

    solar_os_radio_modulations_format(info->modulations, modulations, sizeof(modulations));
    solar_os_radio_features_format(info->features, features, sizeof(features));

    const esp_err_t status_ret = solar_os_radio_get_status(info->name, &status);
    solar_os_shell_io_printf(term,
                             "%s driver=%s state=%s max=%u",
                             info->name,
                             info->driver[0] != '\0' ? info->driver : "-",
                             status_ret == ESP_OK ? solar_os_radio_state_name(status.state) : "unknown",
                             (unsigned)info->max_packet_len);
    if (status_ret == ESP_OK && status.has_rssi) {
        solar_os_shell_io_printf(term, " rssi=%d", status.rssi_dbm);
    }
    if (status_ret == ESP_OK && status.has_snr) {
        solar_os_shell_io_printf(term, " snr=%d", status.snr_db);
    }
    solar_os_shell_io_printf(term, " modulations=%s\n", modulations);

    if (!verbose) {
        return;
    }
    solar_os_shell_io_printf(term, "  features: %s\n", features);
    if (info->summary[0] != '\0') {
        solar_os_shell_io_printf(term, "  summary: %s\n", info->summary);
    }
    if (status_ret == ESP_OK) {
        solar_os_shell_io_write(term, "  config: ");
        radio_print_config(term, &status.config);
    }
}

static void radio_cmd_list(solar_os_shell_io_t *term, bool verbose)
{
    const size_t count = solar_os_radio_count();
    if (count == 0) {
        solar_os_shell_io_writeln(term, "no radios registered");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_radio_info_t info;
        if (solar_os_radio_get(i, &info)) {
            radio_print_device(term, &info, verbose);
        }
    }
}

static void radio_cmd_status(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2) {
        radio_cmd_list(term, true);
        return;
    }
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: radio status <name>");
        return;
    }

    solar_os_radio_info_t info;
    const esp_err_t err = solar_os_radio_get_info(argv[2], &info);
    if (err != ESP_OK) {
        radio_print_error(term, "radio status", err);
        return;
    }
    radio_print_device(term, &info, true);
}

static bool parse_sync_args(int argc, char **argv, int first, solar_os_radio_config_t *config)
{
    if (argc <= first || config == NULL) {
        return false;
    }

    uint8_t bytes[SOLAR_OS_RADIO_SYNC_WORD_MAX];
    size_t len = 0;
    bool all_bytes = true;
    for (int i = first; i < argc; i++) {
        if (len >= SOLAR_OS_RADIO_SYNC_WORD_MAX || !parse_byte_token(argv[i], &bytes[len])) {
            all_bytes = false;
            break;
        }
        len++;
    }

    if (all_bytes && len > 0) {
        config->sync_word_len = (uint8_t)len;
        memcpy(config->sync_word, bytes, len);
        return true;
    }

    if (argc != first + 1) {
        return false;
    }
    const size_t text_len = strlen(argv[first]);
    if (text_len == 0 || text_len > SOLAR_OS_RADIO_SYNC_WORD_MAX) {
        return false;
    }
    config->sync_word_len = (uint8_t)text_len;
    memcpy(config->sync_word, argv[first], text_len);
    return true;
}

static void radio_cmd_config(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 3) {
        solar_os_shell_io_writeln(term, "usage: radio config <name> [field value]");
        return;
    }

    solar_os_radio_status_t status;
    esp_err_t err = solar_os_radio_get_status(argv[2], &status);
    if (err != ESP_OK) {
        radio_print_error(term, "radio config", err);
        return;
    }

    if (argc == 3) {
        radio_print_config(term, &status.config);
        return;
    }
    if (argc < 5) {
        solar_os_shell_io_writeln(term, "usage: radio config <name> <field> <value>");
        return;
    }

    solar_os_radio_config_t config = status.config;
    const char *field = argv[3];
    uint32_t u32 = 0;
    int32_t i32 = 0;
    bool bit = false;

    if (strcmp(field, "frequency") == 0) {
        if (!parse_frequency_arg(argv[4], 1, UINT32_MAX, &u32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.frequency_hz = u32;
    } else if (strcmp(field, "modulation") == 0) {
        const solar_os_radio_modulation_t modulation = solar_os_radio_modulation_from_name(argv[4]);
        if (modulation == SOLAR_OS_RADIO_MODULATION_NONE) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.modulation = modulation;
    } else if (strcmp(field, "bitrate") == 0) {
        if (!parse_u32_arg(argv[4], 1, UINT32_MAX, &u32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.bitrate_bps = u32;
    } else if (strcmp(field, "deviation") == 0) {
        if (!parse_u32_arg(argv[4], 0, UINT32_MAX, &u32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.deviation_hz = u32;
    } else if (strcmp(field, "bandwidth") == 0) {
        if (!parse_u32_arg(argv[4], 0, UINT32_MAX, &u32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.rx_bandwidth_hz = u32;
    } else if (strcmp(field, "power") == 0) {
        if (!parse_i32_arg(argv[4], -128, 127, &i32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.tx_power_dbm = (int8_t)i32;
    } else if (strcmp(field, "crc") == 0) {
        if (!parse_bool_arg(argv[4], &bit)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.crc_enabled = bit;
    } else if (strcmp(field, "variable") == 0) {
        if (!parse_bool_arg(argv[4], &bit)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.variable_length = bit;
    } else if (strcmp(field, "preamble") == 0) {
        if (!parse_u32_arg(argv[4], 0, UINT16_MAX, &u32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.preamble_len = (uint16_t)u32;
    } else if (strcmp(field, "node") == 0) {
        if (!parse_u32_arg(argv[4], 0, UINT8_MAX, &u32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.has_node_id = true;
        config.node_id = (uint8_t)u32;
    } else if (strcmp(field, "network") == 0) {
        if (!parse_u32_arg(argv[4], 0, UINT8_MAX, &u32)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
        config.has_network_id = true;
        config.network_id = (uint8_t)u32;
    } else if (strcmp(field, "sync") == 0) {
        if (!parse_sync_args(argc, argv, 4, &config)) {
            radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
            return;
        }
    } else {
        radio_print_error(term, "radio config", ESP_ERR_INVALID_ARG);
        return;
    }

    err = solar_os_radio_configure(argv[2], &config);
    if (err != ESP_OK) {
        radio_print_error(term, "radio config", err);
        return;
    }
    solar_os_shell_io_printf(term, "configured %s\n", argv[2]);
}

static void radio_cmd_state(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        solar_os_shell_io_writeln(term, "usage: radio state <name> [sleep|standby|rx|tx]");
        return;
    }

    if (argc == 3) {
        solar_os_radio_status_t status;
        const esp_err_t err = solar_os_radio_get_status(argv[2], &status);
        if (err != ESP_OK) {
            radio_print_error(term, "radio state", err);
            return;
        }
        solar_os_shell_io_printf(term, "%s\n", solar_os_radio_state_name(status.state));
        return;
    }

    const solar_os_radio_state_t state = solar_os_radio_state_from_name(argv[3]);
    if (state == SOLAR_OS_RADIO_STATE_UNKNOWN) {
        radio_print_error(term, "radio state", ESP_ERR_INVALID_ARG);
        return;
    }

    const esp_err_t err = solar_os_radio_set_state(argv[2], state);
    if (err != ESP_OK) {
        radio_print_error(term, "radio state", err);
        return;
    }
    solar_os_shell_io_printf(term, "%s state=%s\n", argv[2], solar_os_radio_state_name(state));
}

static bool packet_from_text_args(int argc, char **argv, int first, solar_os_radio_packet_t *packet)
{
    size_t used = 0;

    for (int i = first; i < argc; i++) {
        const size_t arg_len = strlen(argv[i]);
        if (used + arg_len + (i > first ? 1U : 0U) > SOLAR_OS_RADIO_PACKET_MAX) {
            return false;
        }
        if (i > first) {
            packet->data[used++] = ' ';
        }
        memcpy(&packet->data[used], argv[i], arg_len);
        used += arg_len;
    }
    packet->len = used;
    return used > 0;
}

static bool packet_from_args(int argc, char **argv, int first, solar_os_radio_packet_t *packet)
{
    if (argc <= first || packet == NULL) {
        return false;
    }

    size_t len = 0;
    bool all_bytes = true;
    for (int i = first; i < argc; i++) {
        if (len >= SOLAR_OS_RADIO_PACKET_MAX || !parse_byte_token(argv[i], &packet->data[len])) {
            all_bytes = false;
            break;
        }
        len++;
    }
    if (all_bytes && len > 0) {
        packet->len = len;
        return true;
    }
    return packet_from_text_args(argc, argv, first, packet);
}

static void radio_cmd_send(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 4) {
        solar_os_shell_io_writeln(term, "usage: radio send <name> <text|byte...>");
        return;
    }

    solar_os_radio_packet_t packet = {0};
    if (!packet_from_args(argc, argv, 3, &packet)) {
        radio_print_error(term, "radio send", ESP_ERR_INVALID_ARG);
        return;
    }

    const esp_err_t err = solar_os_radio_send(argv[2], &packet, 1000);
    if (err != ESP_OK) {
        radio_print_error(term, "radio send", err);
        return;
    }
    solar_os_shell_io_printf(term, "sent %u bytes on %s\n", (unsigned)packet.len, argv[2]);
}

static void radio_print_packet(solar_os_shell_io_t *term, const solar_os_radio_packet_t *packet)
{
    solar_os_shell_io_printf(term, "received %u bytes", (unsigned)packet->len);
    if (packet->has_source) {
        solar_os_shell_io_printf(term, " src=%u", packet->source);
    }
    if (packet->has_destination) {
        solar_os_shell_io_printf(term, " dst=%u", packet->destination);
    }
    if (packet->has_rssi) {
        solar_os_shell_io_printf(term, " rssi=%d", packet->rssi_dbm);
    }
    if (packet->has_snr) {
        solar_os_shell_io_printf(term, " snr=%d", packet->snr_db);
    }
    solar_os_shell_io_put_char(term, '\n');

    solar_os_shell_io_write(term, "hex:");
    for (size_t i = 0; i < packet->len; i++) {
        solar_os_shell_io_printf(term, " %02x", packet->data[i]);
    }
    solar_os_shell_io_put_char(term, '\n');

    solar_os_shell_io_write(term, "text: ");
    for (size_t i = 0; i < packet->len; i++) {
        const uint8_t ch = packet->data[i];
        solar_os_shell_io_put_char(term, isprint(ch) ? (char)ch : '.');
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void radio_cmd_recv(solar_os_shell_io_t *term, int argc, char **argv)
{
    uint32_t timeout_ms = 1000;

    if (argc < 3 || argc > 4) {
        solar_os_shell_io_writeln(term, "usage: radio recv <name> [timeout-ms]");
        return;
    }
    if (argc == 4 && !parse_u32_arg(argv[3], 0, UINT32_MAX, &timeout_ms)) {
        radio_print_error(term, "radio recv", ESP_ERR_INVALID_ARG);
        return;
    }

    solar_os_radio_packet_t packet;
    const esp_err_t err = solar_os_radio_receive(argv[2], &packet, timeout_ms);
    if (err != ESP_OK) {
        radio_print_error(term, "radio recv", err);
        return;
    }
    radio_print_packet(term, &packet);
}

void solar_os_shell_cmd_radio(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc == 1) {
        const esp_err_t err = solar_os_shell_launch_radio_tui(ctx);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "radio: launch failed: %s\n", esp_err_to_name(err));
        } else {
            solar_os_shell_session_prepare_foreground_launch(ctx, true);
        }
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        radio_cmd_status(term, argc, argv);
        return;
    }
    if (strcmp(argv[1], "list") == 0) {
        radio_cmd_list(term, false);
        return;
    }
    if (strcmp(argv[1], "config") == 0) {
        radio_cmd_config(term, argc, argv);
        return;
    }
    if (strcmp(argv[1], "state") == 0) {
        radio_cmd_state(term, argc, argv);
        return;
    }
    if (strcmp(argv[1], "send") == 0) {
        radio_cmd_send(term, argc, argv);
        return;
    }
    if (strcmp(argv[1], "recv") == 0) {
        radio_cmd_recv(term, argc, argv);
        return;
    }

    radio_print_usage(term);
}
