#include "solar_os_shell_commands.h"

#include <string.h>

#include "solar_os_email.h"
#include "solar_os_email_app.h"
#include "solar_os_jobs.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"

static void email_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  email");
    solar_os_shell_io_writeln(io, "  email status");
    solar_os_shell_io_writeln(io, "  email configure <imaps://host[:port]> <user> <password> [mailbox]");
    solar_os_shell_io_writeln(io, "  email sync");
    solar_os_shell_io_writeln(io, "  email forget");
}

static void email_status(solar_os_shell_io_t *io)
{
    solar_os_email_status_t status;
    const esp_err_t err = solar_os_email_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "email: unavailable: %s\n", esp_err_to_name(err));
        return;
    }
    solar_os_shell_io_printf(io, "Configured: %s\n", status.configured ? "yes" : "no");
    if (status.configured) {
        solar_os_shell_io_printf(io, "Server: %s\n", status.url);
        solar_os_shell_io_printf(io, "User: %s\n", status.user);
        solar_os_shell_io_printf(io, "Mailbox: %s\n", status.mailbox);
    }
    solar_os_shell_io_printf(io,
                             "Messages: %u/%u (%u unread)\n",
                             (unsigned)status.count,
                             (unsigned)status.capacity,
                             (unsigned)status.unread);
    solar_os_shell_io_printf(io,
                             "Sync: %s, %lu completed, %lu received\n",
                             status.syncing ? "running" : "idle",
                             (unsigned long)status.sync_count,
                             (unsigned long)status.received_count);
    if (status.last_error != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "Last error: %s%s%s\n",
                                 esp_err_to_name(status.last_error),
                                 status.last_error_text[0] != '\0' ? ": " : "",
                                 status.last_error_text);
    }
}

void solar_os_shell_cmd_email(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 1) {
        const esp_err_t err = solar_os_context_request_launch(ctx, &solar_os_email_app, 0, NULL);
        if (err == ESP_OK) {
            solar_os_shell_session_prepare_foreground_launch(ctx, false);
        } else {
            solar_os_shell_io_printf(io, "email: launch failed: %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        email_status(io);
        return;
    }
    if ((argc == 5 || argc == 6) && strcmp(argv[1], "configure") == 0) {
        const esp_err_t err = solar_os_email_configure(argv[2],
                                                       argv[3],
                                                       argv[4],
                                                       argc == 6 ? argv[5] : NULL);
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "email account saved");
        } else {
            solar_os_shell_io_printf(io, "email: configure failed: %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "sync") == 0) {
#if SOLAR_OS_PACKAGE_JOB_EMAIL_SYNC
        char *job_argv[] = {"email-sync", "once"};
        const esp_err_t err = solar_os_jobs_start(ctx, "email-sync", 2, job_argv);
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "email sync started");
        } else {
            solar_os_shell_io_printf(io, "email: sync failed: %s\n", esp_err_to_name(err));
        }
#else
        solar_os_shell_io_writeln(io, "email: sync job is not compiled in");
#endif
        return;
    }
    if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        const esp_err_t err = solar_os_email_forget();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "email account forgotten");
        } else {
            solar_os_shell_io_printf(io, "email: forget failed: %s\n", esp_err_to_name(err));
        }
        return;
    }
    email_usage(io);
}
