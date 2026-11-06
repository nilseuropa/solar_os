#include "solar_os_shell_commands.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_controls.h"
#include "solar_os_osc.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_stream.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_net_session.h"
#include "solar_os_port.h"
#include "solar_os_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OSC_DEFAULT_RATE_HZ 50.0f

static const char *const osc_usage =
    "osc bindings\n"
    "  osc send <host:port> <address> <value> [value ...] [--rate 1..100] [--duration seconds]\n"
    "  osc bind <name> stream <stream> <address> [rate=hz] [delta=value] "
    "[send=change|always]\n"
    "  osc bind <name> receive <scalar-sink> <address> [arg=0..7]\n"
    "  osc bind <name> stream <event-stream> <address> edge=rising|falling|both "
    "[rate=hz]\n"
    "  osc bind <name> control <control> <address> [rate=hz] "
    "[send=change|always]\n"
    "  osc unbind <name>\n"
    "  osc clear";

static bool osc_parse_float(const char *text, float *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const float parsed = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

#include "solar_os_shell_osc_send.inc"

static void osc_print_error(solar_os_shell_io_t *io,
                            const char *operation,
                            esp_err_t err)
{
    solar_os_shell_io_printf(io, "osc: %s failed: %s\n", operation,
                             solar_os_shell_error_text(err));
}

static void osc_list_bindings(solar_os_shell_io_t *io)
{
    const size_t count = solar_os_osc_binding_count();
    if (count == 0U) {
        solar_os_shell_io_table_writeln(io, "No OSC bindings configured");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        solar_os_osc_binding_info_t binding;
        if (!solar_os_osc_binding_get(i, &binding)) {
            continue;
        }
        if(binding.config.direction==SOLAR_OS_OSC_RECEIVE) {
            solar_os_shell_io_table_printf(io,
                "%s: receive %s arg=%u -> %s state=%s claimed=%u received=%u write-errors=%u last-receive=%llums error=%s\n",
                binding.config.name,binding.config.address,(unsigned)binding.config.argument,binding.config.source,
                binding.target_available?"ready":"missing",(unsigned)binding.target_claimed,
                (unsigned)binding.received,(unsigned)binding.write_errors,
                (unsigned long long)binding.last_receive_ms,solar_os_shell_error_text(binding.last_error));
            continue;
        }
        const float rate = 1000.0f / (float)binding.config.interval_ms;
        solar_os_shell_io_table_printf(
            io, "%s: %s %s (%s) -> %s rate=%.3gHz",
            binding.config.name,
            solar_os_osc_source_name(binding.config.source_type),
            binding.config.source,
            solar_os_osc_value_name(binding.config.value_type),
            binding.config.address,
            (double)rate);
        if (binding.config.value_type == SOLAR_OS_OSC_VALUE_EVENT) {
            solar_os_shell_io_table_printf(io, " edge=%s",
                                     solar_os_osc_edge_name(binding.config.edge));
        } else {
            solar_os_shell_io_table_printf(io, " delta=%.6g send=%s",
                                     (double)binding.config.delta,
                                     binding.config.send_always ? "always" : "change");
        }
        if (binding.has_value) {
            solar_os_shell_io_table_printf(
                io, " state=%s value=%.6g sent=%u last-send=%llums error=%s\n",
                binding.source_available ? "ready" : "missing",
                (double)binding.last_value, (unsigned)binding.sent,
                (unsigned long long)binding.last_send_ms,
                solar_os_shell_error_text(binding.last_error));
        } else {
            solar_os_shell_io_table_printf(
                io, " state=%s value=- sent=%u last-send=%llums error=%s\n",
                binding.source_available ? "ready" : "missing",
                (unsigned)binding.sent,
                (unsigned long long)binding.last_send_ms,
                solar_os_shell_error_text(binding.last_error));
        }
    }
}

static void osc_bind_command(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc < 6 ||
        (strcmp(argv[3], "stream") != 0 && strcmp(argv[3], "control") != 0 && strcmp(argv[3],"receive") != 0)) {
        solar_os_shell_diag_problem(io, "osc bind", "invalid binding", osc_usage,
                                    NULL);
        return;
    }
    solar_os_osc_binding_config_t config = {
        .source_type = strcmp(argv[3], "control") == 0 ?
            SOLAR_OS_OSC_SOURCE_CONTROL : SOLAR_OS_OSC_SOURCE_STREAM,
        .value_type = SOLAR_OS_OSC_VALUE_SCALAR,
        .interval_ms = (uint32_t)lroundf(1000.0f / OSC_DEFAULT_RATE_HZ),
        .edge = SOLAR_OS_OSC_EDGE_BOTH,
        .direction = !strcmp(argv[3],"receive")?SOLAR_OS_OSC_RECEIVE:SOLAR_OS_OSC_SEND,
    };
    if (strlen(argv[2]) >= sizeof(config.name) ||
        strlen(argv[4]) >= sizeof(config.source) ||
        strlen(argv[5]) >= sizeof(config.address)) {
        solar_os_shell_diag_problem(io, "osc bind",
                                    "name, source, or address is too long",
                                    osc_usage, NULL);
        return;
    }
    strlcpy(config.name, argv[2], sizeof(config.name));
    strlcpy(config.source, argv[4], sizeof(config.source));
    strlcpy(config.address, argv[5], sizeof(config.address));
    bool edge_option = false;
    bool argument_set = false;
    for (int i = 6; i < argc; i++) {
        if(config.direction==SOLAR_OS_OSC_RECEIVE) {
            size_t argument;
            if(argument_set || strncmp(argv[i],"arg=",4) ||
               !solar_os_shell_parse_size_arg(argv[i]+4,0,SOLAR_OS_OSC_ARGUMENT_MAX-1,&argument)) {
                solar_os_shell_diag_invalid(io,"osc bind","receive option",argv[i],"one arg=0..7",osc_usage,false);return;
            }
            config.argument=(uint32_t)argument;argument_set=true;
        } else if (strncmp(argv[i], "rate=", 5U) == 0) {
            float rate = 0.0f;
            if (!osc_parse_float(argv[i] + 5U, &rate) ||
                rate * 1000.0f < SOLAR_OS_OSC_RATE_MIN_MILLIHZ ||
                rate * 1000.0f > SOLAR_OS_OSC_RATE_MAX_MILLIHZ) {
                solar_os_shell_diag_invalid(io, "osc bind", "rate", argv[i],
                                            "rate=0.1..100", osc_usage, false);
                return;
            }
            config.interval_ms = (uint32_t)lroundf(1000.0f / rate);
        } else if (strncmp(argv[i], "delta=", 6U) == 0) {
            if (!osc_parse_float(argv[i] + 6U, &config.delta) ||
                config.delta < 0.0f) {
                solar_os_shell_diag_invalid(io, "osc bind", "delta", argv[i],
                                            "delta=non-negative-number",
                                            osc_usage, false);
                return;
            }
        } else if (strcmp(argv[i], "send=always") == 0) {
            config.send_always = true;
        } else if (strcmp(argv[i], "send=change") == 0) {
            config.send_always = false;
        } else if (strncmp(argv[i], "edge=", 5U) == 0) {
            edge_option = true;
            config.value_type = SOLAR_OS_OSC_VALUE_EVENT;
            if (strcmp(argv[i] + 5U, "rising") == 0) {
                config.edge = SOLAR_OS_OSC_EDGE_RISING;
            } else if (strcmp(argv[i] + 5U, "falling") == 0) {
                config.edge = SOLAR_OS_OSC_EDGE_FALLING;
            } else if (strcmp(argv[i] + 5U, "both") != 0) {
                solar_os_shell_diag_invalid(io, "osc bind", "edge", argv[i],
                                            "edge=rising|falling|both",
                                            osc_usage, false);
                return;
            }
        } else {
            solar_os_shell_diag_unexpected(io, "osc bind", argv[i], osc_usage);
            return;
        }
    }
    if (config.source_type == SOLAR_OS_OSC_SOURCE_CONTROL && edge_option) {
        solar_os_shell_diag_problem(io, "osc bind",
                                    "control sources cannot use edge=", osc_usage,
                                    NULL);
        return;
    }
    if (config.value_type == SOLAR_OS_OSC_VALUE_EVENT &&
        (config.delta != 0.0f || config.send_always)) {
        solar_os_shell_diag_problem(io, "osc bind",
                                    "event sources cannot use delta= or send=always",
                                    osc_usage, NULL);
        return;
    }

    if (config.source_type == SOLAR_OS_OSC_SOURCE_STREAM) {
        solar_os_stream_info_t stream;
        if (solar_os_stream_get_info(config.source, &stream) == ESP_OK) {
            const solar_os_stream_type_t expected = edge_option ?
                SOLAR_OS_STREAM_TYPE_EVENT : SOLAR_OS_STREAM_TYPE_SCALAR;
            if (stream.type != expected || (config.direction==SOLAR_OS_OSC_RECEIVE &&
                (stream.direction==SOLAR_OS_STREAM_DIRECTION_SOURCE || strcmp(stream.format,"f32")))) {
                solar_os_shell_diag_problem(
                    io, "osc bind",
                    config.direction==SOLAR_OS_OSC_RECEIVE?"target is not a writable float32 scalar stream":
                    edge_option ? "source is not an event stream" :
                                  "source is not a scalar stream",
                    osc_usage, NULL);
                return;
            }
        }
    }
    const esp_err_t err = solar_os_osc_bind(&config, NULL);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(io, "OSC binding %s created\n", config.name);
    } else {
        osc_print_error(io, "bind", err);
    }
}

void solar_os_shell_cmd_osc(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc == 2 && strcmp(argv[1], "bindings") == 0) {
        osc_list_bindings(io);
    } else if (argc >= 2 && strcmp(argv[1], "send") == 0) {
        osc_send_command(ctx, argc, argv);
    } else if (argc >= 2 && strcmp(argv[1], "bind") == 0) {
        osc_bind_command(io, argc, argv);
    } else if (argc == 3 && strcmp(argv[1], "unbind") == 0) {
        const esp_err_t err = solar_os_osc_unbind(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io, "OSC binding %s removed\n", argv[2]);
        } else {
            osc_print_error(io, "unbind", err);
        }
    } else if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        esp_err_t err=solar_os_osc_clear();
        if(err==ESP_OK)solar_os_shell_io_writeln(io, "OSC bindings cleared");
        else osc_print_error(io,"clear; close failed or binding busy; retry",err);
    } else {
        solar_os_shell_diag_problem(io, "osc", "invalid command", osc_usage,
                                    NULL);
    }
}
