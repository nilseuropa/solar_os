#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_app_registry.h"
#include "solar_os_dlp_app.h"
#include "solar_os_port.h"
#include "solar_os_sessions.h"
#include "solar_os_shell_common.h"

void solar_os_shell_cmd_dlp(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    const char *usage = "dlp serve <display> stream=<byte-stream> | dlp status | dlp stop <display>";
    solar_os_dlp_app_status_t status;
    solar_os_dlp_app_get_status(&status);
    if (argc == 2 && !strcmp(argv[1], "status")) {
        if (!status.running) solar_os_shell_io_writeln(io, "DLP: stopped");
        else solar_os_shell_io_printf(io,
            "DLP: %s %s <- %s, frames=%" PRIu32 " RX=%" PRIu32 " TX=%" PRIu32 "\n",
            status.suspended ? "suspended" : status.waiting ? "waiting" : "serving", status.display, status.stream,
            status.presented_frames, status.received_bytes, status.sent_bytes);
        const solar_os_dlp_presentation_stats_t *p = &status.presentation;
        if (p->frames) {
            solar_os_shell_io_printf(io,
                "Presentation timing: n=%" PRIu32 " (DLP frames only; last/avg/max us)\n"
                "  render:  %" PRIu32 "/%" PRIu64 "/%" PRIu32 "\n"
                "  display: %" PRIu32 "/%" PRIu64 "/%" PRIu32 "\n"
                "  total:   %" PRIu32 "/%" PRIu64 "/%" PRIu32 "\n",
                p->frames, p->render_last_us, p->render_total_us / p->frames, p->render_max_us,
                p->display_last_us, p->display_total_us / p->frames, p->display_max_us,
                p->total_last_us, p->total_us / p->frames, p->total_max_us);
            if (p->frames > 1 && p->last_completed_us > p->first_completed_us) {
                uint64_t fps_milli = (uint64_t)(p->frames - 1) * 1000000000ULL /
                    (p->last_completed_us - p->first_completed_us);
                solar_os_shell_io_printf(io, "  completion rate: %" PRIu64 ".%03" PRIu64 " fps\n",
                    fps_milli / 1000, fps_milli % 1000);
            }
        }
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "stop")) {
        char owner[SOLAR_OS_APP_OWNER_MAX];
        unsigned session = 0;
        if (!status.running || strcmp(argv[2], status.display) ||
            !solar_os_app_registry_owner(&solar_os_dlp_app, owner, sizeof(owner)) ||
            sscanf(owner, "session %u", &session) != 1 || session > UINT8_MAX) {
            solar_os_shell_diag_problem(io, "dlp", "no receiver on that display", usage, NULL);
            return;
        }
        esp_err_t err = solar_os_sessions_close_session((uint8_t)session, io);
        if (err != ESP_OK) solar_os_shell_diag_esp(io, "dlp stop", err, NULL, NULL);
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "serve") && !strncmp(argv[3], "stream=", 7) && argv[3][7]) {
        solar_os_stream_info_t stream;
        esp_err_t err = solar_os_stream_get_info(argv[3] + 7, &stream);
        if (err != ESP_OK) {
            solar_os_shell_diag_esp(io, "dlp stream", err, argv[3] + 7, "Use stream list");
            return;
        }
        if (stream.type != SOLAR_OS_STREAM_TYPE_BYTES || stream.direction != SOLAR_OS_STREAM_DIRECTION_DUPLEX ||
            stream.sharing != SOLAR_OS_STREAM_SHARING_EXCLUSIVE) {
            solar_os_shell_diag_problem(io, "dlp", "an exclusive duplex byte stream is required", usage, NULL);
            return;
        }
        solar_os_port_info_t port = {0};
        if (stream.active_handles ||
            (solar_os_port_get_info(stream.id, &port) == ESP_OK && port.claimed)) {
            solar_os_shell_io_printf(io, "dlp: %s owns %s\n",
                stream.owner[0] ? stream.owner : port.owner, stream.id);
            return;
        }
        char *args[] = {"dlp", argv[3]};
        char busy[SOLAR_OS_APP_OWNER_MAX];
        uint8_t session = 0;
        err = solar_os_sessions_create_display_app(&solar_os_dlp_app, argv[2], 2, args,
            &session, busy, sizeof(busy));
        if (err != ESP_OK) solar_os_shell_diag_esp(io, "dlp serve", err, busy[0] ? busy : NULL, NULL);
        else solar_os_shell_io_printf(io, "DLP session %u: %s <- %s\n", session, argv[2], stream.id);
        return;
    }
    static const char * const subcommands[] = {"serve", "status", "stop"};
    solar_os_shell_diag_subcommand(io, "dlp", argc, argv, usage, subcommands, 3);
}
