#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_MQTT

#include "solar_os_shell_commands.h"

#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_MQTT

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_mqtt.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const mqtt_commands[] = {
    "status", "connect", "disconnect", "publish", "subscribe",
};

static void mqtt_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  mqtt status");
    solar_os_shell_io_writeln(term, "  mqtt connect [mqtt[s]://host[:port] [username [password]]]");
    solar_os_shell_io_writeln(term, "  mqtt disconnect");
    solar_os_shell_io_writeln(term, "  mqtt publish <topic> <payload> [qos] [retain]");
    solar_os_shell_io_writeln(term, "  mqtt subscribe <topic> [qos]");
}

static bool mqtt_parse_qos(const char *text, int *qos)
{
    size_t value = 0;

    if (qos == NULL || !solar_os_shell_parse_size_arg(text, 0, 2, &value)) {
        return false;
    }

    *qos = (int)value;
    return true;
}

static bool mqtt_parse_retain(const char *text, bool *retain)
{
    if (text == NULL || retain == NULL) {
        return false;
    }
    if (strcmp(text, "1") == 0 ||
        strcmp(text, "true") == 0 ||
        strcmp(text, "on") == 0 ||
        strcmp(text, "retain") == 0) {
        *retain = true;
        return true;
    }
    if (strcmp(text, "0") == 0 ||
        strcmp(text, "false") == 0 ||
        strcmp(text, "off") == 0) {
        *retain = false;
        return true;
    }
    return false;
}

static bool mqtt_read_stop_key(solar_os_shell_io_t *term)
{
    char chars[8];
    size_t count;

    while ((count = solar_os_input_read_chars(chars, sizeof(chars))) > 0) {
        for (size_t i = 0; i < count; i++) {
            if ((uint8_t)chars[i] == SOLAR_OS_KEY_APP_EXIT ||
                chars[i] == 'q') {
                return true;
            }
        }
    }

    if (term == NULL ||
        solar_os_shell_io_kind(term) != SOLAR_OS_SHELL_IO_KIND_PORT ||
        !solar_os_port_handle_valid(&term->port)) {
        return false;
    }

    uint8_t port_chars[8];
    do {
        count = 0;
        const esp_err_t err = solar_os_port_read(&term->port,
                                                 port_chars,
                                                 sizeof(port_chars),
                                                 0,
                                                 &count);
        if (err != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            if (port_chars[i] == SOLAR_OS_KEY_APP_EXIT ||
                port_chars[i] == 0x1dU ||
                port_chars[i] == 'q') {
                return true;
            }
        }
    } while (count > 0);

    return false;
}

static void mqtt_print_status(solar_os_shell_io_t *term)
{
    solar_os_mqtt_status_t status;
    const esp_err_t err = solar_os_mqtt_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt status failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "MQTT: %s%s\n",
                             status.running ? "on" : "off",
                             status.connected ? ", connected" : "");
    solar_os_shell_io_printf(term,
                             "Broker: %s\n",
                             status.configured ? status.url : "(not configured)");
    solar_os_shell_io_printf(term,
                             "Auth: user %s, password %s\n",
                             status.username_set ? status.username : "none",
                             status.password_set ? "set" : "none");
    solar_os_shell_io_printf(term, "Client: %s\n", status.client_id);
    solar_os_shell_io_printf(term,
                             "Messages: rx %" PRIu32 ", tx %" PRIu32 ", queued %u, dropped %" PRIu32 "\n",
                             status.rx_count,
                             status.tx_count,
                             (unsigned)status.queued_messages,
                             status.dropped_count);
    if (status.last_error[0] != '\0') {
        solar_os_shell_io_printf(term,
                                 "Last error: %s (%s)\n",
                                 status.last_error,
                                 solar_os_shell_error_text(status.error));
    }
}

static void mqtt_wait_and_print_connect(solar_os_shell_io_t *term)
{
    for (int i = 0; i < 50; i++) {
        solar_os_mqtt_status_t status;
        if (solar_os_mqtt_get_status(&status) == ESP_OK) {
            if (status.connected) {
                solar_os_shell_io_writeln(term, "mqtt: connected");
                return;
            }
            if (status.last_error[0] != '\0') {
                solar_os_shell_io_printf(term, "mqtt: %s\n", status.last_error);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    solar_os_shell_io_writeln(term, "mqtt: connecting");
}

static void mqtt_cmd_connect(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc > 5) {
        solar_os_shell_diag_problem(term, "mqtt connect", "too many connection arguments",
                                    "mqtt connect [url [username [password]]]", NULL);
        return;
    }

    const char *url = argc >= 3 ? argv[2] : NULL;
    const char *username = argc >= 4 ? argv[3] : NULL;
    const char *password = argc >= 5 ? argv[4] : NULL;
    const esp_err_t err = solar_os_mqtt_connect(url, username, password);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_diag_problem(
            term, "mqtt connect", "no saved broker URL",
            "mqtt connect mqtt[s]://host[:port] [username [password]]", NULL);
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_diag_problem(
            term, "mqtt connect", "invalid URL, username, or password",
            "mqtt connect mqtt[s]://host[:port] [username [password]]",
            "credential values are intentionally not repeated");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt connect failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    mqtt_wait_and_print_connect(term);
}

static void mqtt_cmd_publish(solar_os_shell_io_t *term, int argc, char **argv)
{
    int qos = 0;
    bool retain = false;

    if (argc < 3) {
        solar_os_shell_diag_missing(term, "mqtt publish", "topic",
                                    "mqtt publish <topic> <payload> [qos] [retain]");
        return;
    }
    if (argc < 4) {
        solar_os_shell_diag_missing(term, "mqtt publish", "payload",
                                    "mqtt publish <topic> <payload> [qos] [retain]");
        return;
    }
    if (argc > 6) {
        solar_os_shell_diag_unexpected(term, "mqtt publish", argv[6],
                                       "mqtt publish <topic> <payload> [qos] [retain]");
        return;
    }
    if (argc >= 5 && !mqtt_parse_qos(argv[4], &qos)) {
        solar_os_shell_diag_invalid(term, "mqtt publish", "QoS", argv[4], "0, 1, or 2",
                                    "mqtt publish <topic> <payload> [qos] [retain]", false);
        return;
    }
    if (argc >= 6 && !mqtt_parse_retain(argv[5], &retain)) {
        solar_os_shell_diag_invalid(term, "mqtt publish", "retain flag", argv[5],
                                    "0, 1, on, off, or retain",
                                    "mqtt publish <topic> <payload> [qos] [retain]", false);
        return;
    }

    int msg_id = 0;
    const char *payload = argv[3];
    const esp_err_t err = solar_os_mqtt_publish(argv[2],
                                                payload,
                                                strlen(payload),
                                                qos,
                                                retain,
                                                &msg_id);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "mqtt: not connected");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "mqtt: invalid topic, payload, qos, or retain flag");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt publish failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    solar_os_shell_io_printf(term, "mqtt publish: msg %d\n", msg_id);
}

static void mqtt_print_message(solar_os_shell_io_t *term, const solar_os_mqtt_message_t *message)
{
    solar_os_shell_io_printf(term,
                             "%s %.*s%s\n",
                             message->topic,
                             (int)message->payload_len,
                             message->payload,
                             message->truncated ? " ..." : "");
}

static void mqtt_cmd_subscribe(solar_os_shell_io_t *term, int argc, char **argv)
{
    int qos = 0;

    if (argc < 3) {
        solar_os_shell_diag_missing(term, "mqtt subscribe", "topic",
                                    "mqtt subscribe <topic> [qos]");
        return;
    }
    if (argc > 4) {
        solar_os_shell_diag_unexpected(term, "mqtt subscribe", argv[4],
                                       "mqtt subscribe <topic> [qos]");
        return;
    }
    if (argc == 4 && !mqtt_parse_qos(argv[3], &qos)) {
        solar_os_shell_diag_invalid(term, "mqtt subscribe", "QoS", argv[3], "0, 1, or 2",
                                    "mqtt subscribe <topic> [qos]", false);
        return;
    }

    int msg_id = 0;
    const esp_err_t err = solar_os_mqtt_subscribe(argv[2], qos, &msg_id);
    if (err == ESP_ERR_INVALID_STATE) {
        solar_os_shell_io_writeln(term, "mqtt: not connected");
        return;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_io_writeln(term, "mqtt: invalid topic or qos");
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term, "mqtt subscribe failed: %s\n", solar_os_shell_error_text(err));
        return;
    }

    solar_os_shell_io_printf(term,
                             "mqtt subscribe: %s qos %d msg %d, %s/q to stop display\n",
                             argv[2],
                             qos,
                             msg_id,
                             solar_os_shell_io_app_exit_key(term));
    solar_os_shell_io_flush(term);

    bool stopped = false;
    while (!stopped) {
        if (mqtt_read_stop_key(term)) {
            stopped = true;
            break;
        }

        solar_os_mqtt_message_t message;
        const esp_err_t read_err = solar_os_mqtt_read_message(&message, 250);
        if (read_err == ESP_OK) {
            mqtt_print_message(term, &message);
            solar_os_shell_io_flush(term);
        } else if (read_err != ESP_ERR_TIMEOUT) {
            solar_os_shell_io_printf(term, "mqtt read failed: %s\n", solar_os_shell_error_text(read_err));
            break;
        }
    }

    solar_os_shell_io_writeln(term, "mqtt subscribe: display stopped");
}

void solar_os_shell_cmd_mqtt(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = solar_os_shell_command_io(ctx);

    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        if (argc > 2) {
            solar_os_shell_diag_unexpected(term, "mqtt status", argv[2], "mqtt status");
            return;
        }
        mqtt_print_status(term);
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        mqtt_cmd_connect(term, argc, argv);
    } else if (strcmp(argv[1], "disconnect") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "mqtt disconnect", argv[2], "mqtt disconnect");
            return;
        }
        const esp_err_t err = solar_os_mqtt_disconnect();
        if (err != ESP_OK) {
            solar_os_shell_io_printf(term, "mqtt disconnect failed: %s\n", solar_os_shell_error_text(err));
            return;
        }
        solar_os_shell_io_writeln(term, "mqtt: disconnected");
    } else if (strcmp(argv[1], "publish") == 0) {
        mqtt_cmd_publish(term, argc, argv);
    } else if (strcmp(argv[1], "subscribe") == 0) {
        mqtt_cmd_subscribe(term, argc, argv);
    } else {
        solar_os_shell_diag_subcommand(term,
                                       "mqtt",
                                       argc,
                                       argv,
                                       "mqtt status|connect|disconnect|publish|subscribe",
                                       mqtt_commands,
                                       sizeof(mqtt_commands) / sizeof(mqtt_commands[0]));
    }
}

#endif

#endif /* SOLAR_OS_PACKAGE_SERVICE_MQTT */
