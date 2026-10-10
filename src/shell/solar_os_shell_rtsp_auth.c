#include "solar_os_shell_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_rtsp_auth.h"
#include "solar_os_shell_io.h"

static void rtsp_auth_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  rtsp-auth");
    solar_os_shell_io_writeln(io, "  rtsp-auth set <host[:port]> <user> <password>");
    solar_os_shell_io_writeln(io, "  rtsp-auth clear [host[:port]]");
}

static bool split_host(const char *text, char *host, size_t host_cap, uint16_t *port)
{
    if (!text || !*text || !host || !port) return false;
    const char *colon = strrchr(text, ':');
    *port = 554;
    if (colon && colon != text && strchr(colon + 1, '.') == NULL) {
        char *end = NULL;
        unsigned long n = strtoul(colon + 1, &end, 10);
        if (!end || *end || n == 0 || n > 65535) return false;
        *port = (uint16_t)n;
        if ((size_t)(colon - text) >= host_cap) return false;
        memcpy(host, text, (size_t)(colon - text));
        host[colon - text] = '\0';
        return host[0] != '\0';
    }
    return snprintf(host, host_cap, "%s", text) > 0 && strlen(text) < host_cap;
}

void solar_os_shell_cmd_rtsp_auth(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (!io) return;
    if (argc == 1) {
        solar_os_rtsp_auth_account_t records[SOLAR_OS_RTSP_AUTH_CAPACITY];
        size_t count = solar_os_rtsp_auth_list(records, SOLAR_OS_RTSP_AUTH_CAPACITY);
        if (!count) {
            solar_os_shell_io_writeln(io, "No stored RTSP accounts.");
            return;
        }
        for (size_t i = 0; i < count; i++)
            solar_os_shell_io_printf(io, "%s:%u  %s\n", records[i].host, records[i].port, records[i].user);
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        esp_err_t err = solar_os_rtsp_auth_clear(NULL, 0);
        solar_os_shell_io_writeln(io, err == ESP_OK ? "Cleared stored RTSP accounts." : "rtsp-auth: clear failed");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "clear")) {
        char host[SOLAR_OS_RTSP_AUTH_HOST_MAX + 1U];
        uint16_t port = 554;
        if (!split_host(argv[2], host, sizeof(host), &port)) {
            rtsp_auth_usage(io);
            return;
        }
        esp_err_t err = solar_os_rtsp_auth_clear(host, port);
        solar_os_shell_io_printf(io, err == ESP_OK ? "Cleared %s:%u\n" : "rtsp-auth: no account for %s:%u\n", host, port);
        return;
    }
    if (argc == 5 && !strcmp(argv[1], "set")) {
        char host[SOLAR_OS_RTSP_AUTH_HOST_MAX + 1U];
        uint16_t port = 554;
        if (!split_host(argv[2], host, sizeof(host), &port)) {
            rtsp_auth_usage(io);
            return;
        }
        esp_err_t err = solar_os_rtsp_auth_set(host, port, argv[3], argv[4]);
        if (err == ESP_OK) solar_os_shell_io_printf(io, "Stored RTSP account for %s:%u\n", host, port);
        else if (err == ESP_ERR_NO_MEM) solar_os_shell_io_writeln(io, "rtsp-auth: account table full (4)");
        else solar_os_shell_io_writeln(io, "rtsp-auth: could not store account");
        return;
    }
    rtsp_auth_usage(io);
}
