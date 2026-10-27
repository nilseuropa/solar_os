#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_daq_job.h"
#include "solar_os_jobs.h"
#include "solar_os_port.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const daq_commands[] = {"help", "status", "streams", "start", "stop"};
#include "solar_os_stream.h"

static void daq_print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  daq help");
    solar_os_shell_io_writeln(term, "  daq status");
    solar_os_shell_io_writeln(term, "  daq streams");
    solar_os_shell_io_writeln(term, "  daq start <file.csv> <stream...> [--rate seconds|--rate-ms ms]");
    solar_os_shell_io_writeln(term, "  daq start <stream...> <file.csv> [--rate seconds|--rate-ms ms]");
    solar_os_shell_io_writeln(term, "  daq start <file.csv> <stream> --changes [--append|--replace]");
    solar_os_shell_io_writeln(term, "  daq start <file.bin> <byte-stream> --raw [--rate-ms ms]");
    solar_os_shell_io_writeln(term, "  daq stop");
    solar_os_shell_io_writeln(term, "");
    solar_os_shell_io_writeln(term, "examples:");
    solar_os_shell_io_writeln(term, "  daq start /logs/env.csv temperature humidity battery --rate 60");
    solar_os_shell_io_writeln(term, "  daq start /logs/key.csv gpio17 --changes");
    solar_os_shell_io_writeln(term, "  daq start /logs/uart0.bin uart0 --raw --rate-ms 25");
}

static void daq_stream_print_list(solar_os_shell_io_t *term)
{
    const size_t count = solar_os_stream_count();
    if (count == 0) {
        solar_os_shell_io_table_writeln(term, "streams: none");
        return;
    }

    solar_os_shell_io_table_writeln(term, "ID           TYPE    FORMAT UNIT      SUMMARY");
    for (size_t i = 0; i < count; i++) {
        solar_os_stream_info_t info;
        if (!solar_os_stream_get(i, &info)) {
            continue;
        }
        solar_os_shell_io_table_printf(term,
                                 "%-12s %-7s %-6s %-9s %s\n",
                                 info.id,
                                 solar_os_stream_type_name(info.type),
                                 info.format,
                                 info.unit[0] != '\0' ? info.unit : "-",
                                 info.summary);
    }
}

static void daq_print_status(solar_os_shell_io_t *term)
{
    solar_os_daq_status_t status;
    solar_os_daq_job_get_status(&status);

    if (!status.running) {
        solar_os_shell_io_printf(term,
                                 "DAQ: stopped%s%s\n",
                                 status.last_error == ESP_OK ? "" : ", last error ",
                                 status.last_error == ESP_OK ? "" : solar_os_shell_error_text(status.last_error));
        return;
    }

    solar_os_shell_io_printf(term, "DAQ: running %s -> %s\n", status.stream_ids, status.path);
    solar_os_shell_io_printf(term,
                             "Streams: %u, type %s, mode %s, interval %" PRIu32 " ms, %s, %s\n",
                             (unsigned)status.stream_count,
                             solar_os_stream_type_name(status.stream_type),
                             status.raw ? "raw" : "csv",
                             status.interval_ms,
                             status.change_only ? "changes" : "samples",
                             status.append ? "append" : "replace");
    if (status.raw) {
        solar_os_shell_io_printf(term,
                                 "Chunks: written %" PRIu32 ", bytes %" PRIu64
                                 ", skipped %" PRIu32 ", failed %" PRIu32 "\n",
                                 status.written_records,
                                 status.written_bytes,
                                 status.skipped_records,
                                 status.failed_records);
    } else {
        solar_os_shell_io_printf(term,
                                 "Records: written %" PRIu32
                                 ", skipped %" PRIu32 ", failed %" PRIu32 "\n",
                                 status.written_records,
                                 status.skipped_records,
                                 status.failed_records);
    }
    if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(term, "Last error: %s\n", solar_os_shell_error_text(status.last_error));
    }
}

static void daq_print_start_error(solar_os_shell_io_t *term,
                                  const char *stream_id,
                                  esp_err_t err)
{
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "daq: stream not found: %s\n", stream_id);
        return;
    }
    if (err == ESP_ERR_INVALID_STATE && stream_id != NULL) {
        solar_os_stream_info_t stream;
        solar_os_port_info_t port;
        if (solar_os_stream_get_info(stream_id, &stream) == ESP_OK &&
            stream.type == SOLAR_OS_STREAM_TYPE_BYTES &&
            solar_os_port_get_info(stream.id, &port) == ESP_OK &&
            port.claimed) {
            solar_os_shell_io_printf(term, "daq: %s is owned by %s\n", stream.id, port.owner);
            return;
        }
    }
    if (err == ESP_ERR_INVALID_ARG) {
        solar_os_shell_diag_problem(term, "daq start", "invalid capture arguments",
                                    "daq start <file> <stream...> [options]", NULL);
        return;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        solar_os_shell_io_writeln(term, "daq: unsupported stream/mode combination");
        return;
    }

    solar_os_shell_io_printf(term, "daq start failed: %s\n", solar_os_shell_error_text(err));
}

void solar_os_shell_cmd_daq(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = solar_os_shell_command_io(ctx);

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "help") == 0)) {
        daq_print_usage(term);
        return;
    }

    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        daq_print_status(term);
        return;
    }

    if (argc == 2 && strcmp(argv[1], "streams") == 0) {
        daq_stream_print_list(term);
        return;
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (argc != 2) {
            solar_os_shell_diag_unexpected(term, "daq stop", argv[2], "daq stop");
            return;
        }
        const esp_err_t err = solar_os_jobs_stop(ctx, "daq");
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "daq: stopped");
        } else {
            solar_os_shell_io_printf(term, "daq stop failed: %s\n", solar_os_shell_error_text(err));
        }
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 4 || argc > SOLAR_OS_APP_ARG_MAX + 1) {
            solar_os_shell_diag_problem(term, "daq start",
                                        argc < 4 ? "missing file or stream" : "too many arguments",
                                        "daq start <file> <stream...> [options]", NULL);
            return;
        }

        char *job_argv[SOLAR_OS_APP_ARG_MAX];
        job_argv[0] = "daq";
        for (int i = 2; i < argc; i++) {
            job_argv[i - 1] = argv[i];
        }

        const esp_err_t err = solar_os_jobs_start(ctx, "daq", argc - 1, job_argv);
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(term, "daq: started");
        } else {
            daq_print_start_error(term, argv[2], err);
        }
        return;
    }

    solar_os_shell_diag_subcommand(term,
                                   "daq",
                                   argc,
                                   argv,
                                   "daq help|status|streams|start|stop",
                                   daq_commands,
                                   sizeof(daq_commands) / sizeof(daq_commands[0]));
}
