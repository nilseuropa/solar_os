#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_inbox_app.h"
#include "solar_os_inbox.h"
#include "solar_os_memory.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"

#define INBOX_LIST_MAX 16

static solar_os_shell_io_t *inbox_terminal(solar_os_context_t *ctx)
{
    return solar_os_context_shell_io(ctx);
}

static void inbox_print_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  inbox");
    solar_os_shell_io_writeln(io, "  inbox status");
    solar_os_shell_io_writeln(io, "  inbox list [all|unread]");
    solar_os_shell_io_writeln(io, "  inbox read <id>");
    solar_os_shell_io_writeln(io, "  inbox clear");
    solar_os_shell_io_writeln(io, "  inbox post <source> <message>");
}

static bool inbox_parse_id(const char *text, uint32_t *id)
{
    if (text == NULL || text[0] == '\0' || id == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT32_MAX) {
        return false;
    }
    *id = (uint32_t)value;
    return true;
}

static void inbox_cmd_status(solar_os_shell_io_t *io)
{
    solar_os_inbox_status_t status;
    const esp_err_t err = solar_os_inbox_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "inbox: unavailable: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(io,
                             "Messages: %u/%u (%u unread)\n",
                             (unsigned)status.count,
                             (unsigned)status.capacity,
                             (unsigned)status.unread);
    solar_os_shell_io_printf(io,
                             "Storage: %s, %u bytes\n",
                             status.ring_in_psram ? "PSRAM" : "internal RAM",
                             (unsigned)status.bytes);
    solar_os_shell_io_printf(io, "Dropped: %lu\n", (unsigned long)status.dropped);
}

static char inbox_priority_marker(solar_os_inbox_priority_t priority)
{
    return priority >= SOLAR_OS_INBOX_PRIORITY_HIGH ? '!' : ' ';
}

static void inbox_summary(const solar_os_inbox_entry_t *entry, char *summary, size_t summary_len)
{
    if (summary == NULL || summary_len == 0) {
        return;
    }

    const char *source = entry->title[0] != '\0' ? entry->title : entry->body;
    size_t copied = 0;
    while (source[copied] != '\0' && copied + 1 < summary_len) {
        const char ch = source[copied];
        summary[copied] = ch == '\r' || ch == '\n' || ch == '\t' ? ' ' : ch;
        copied++;
    }
    summary[copied] = '\0';
}

static void inbox_cmd_list(solar_os_shell_io_t *io, bool unread_only)
{
    solar_os_inbox_entry_t *entries =
        solar_os_memory_calloc(INBOX_LIST_MAX,
                               sizeof(*entries),
                               SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                               "shell.inbox");
    if (entries == NULL) {
        solar_os_shell_io_writeln(io, "inbox: out of memory");
        return;
    }

    size_t total = 0;
    const size_t count =
        solar_os_inbox_snapshot(entries, INBOX_LIST_MAX, unread_only, &total);
    if (total == 0) {
        solar_os_shell_io_writeln(io, unread_only ? "no unread messages" : "inbox is empty");
        solar_os_memory_free(entries);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        char summary[81];
        inbox_summary(&entries[i], summary, sizeof(summary));
        solar_os_shell_io_printf(io,
                                 "%c%c %lu %s%s%s: %s%s\n",
                                 entries[i].unread ? '*' : ' ',
                                 inbox_priority_marker(entries[i].priority),
                                 (unsigned long)entries[i].id,
                                 entries[i].source,
                                 entries[i].topic[0] != '\0' ? "/" : "",
                                 entries[i].topic,
                                 summary,
                                 entries[i].truncated ? " [truncated]" : "");
    }
    if (total > count) {
        solar_os_shell_io_printf(io, "... %u older messages\n", (unsigned)(total - count));
    }
    solar_os_memory_free(entries);
}

static void inbox_cmd_read(solar_os_shell_io_t *io, uint32_t id)
{
    solar_os_inbox_entry_t entry;
    const esp_err_t err = solar_os_inbox_get(id, &entry, true);
    if (err == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(io, "inbox: message %lu not found\n", (unsigned long)id);
        return;
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "inbox: read failed: %s\n", esp_err_to_name(err));
        return;
    }

    solar_os_shell_io_printf(io, "Message: %lu\n", (unsigned long)entry.id);
    solar_os_shell_io_printf(io, "Source: %s\n", entry.source);
    if (entry.topic[0] != '\0') {
        solar_os_shell_io_printf(io, "Topic: %s\n", entry.topic);
    }
    if (entry.sender[0] != '\0') {
        solar_os_shell_io_printf(io, "Sender: %s\n", entry.sender);
    }
    solar_os_shell_io_printf(io,
                             "Priority: %s\nReceived: %lu ms\n",
                             solar_os_inbox_priority_name(entry.priority),
                             (unsigned long)entry.received_ms);
    if (entry.timestamp_ms != 0) {
        solar_os_shell_io_printf(io, "Timestamp: %" PRIu64 " ms\n", entry.timestamp_ms);
    }
    if (entry.title[0] != '\0') {
        solar_os_shell_io_printf(io, "Title: %s\n", entry.title);
    }
    if (entry.body[0] != '\0') {
        solar_os_shell_io_writeln(io, entry.body);
    }
    if (entry.truncated) {
        solar_os_shell_io_writeln(io, "[message fields truncated]");
    }
}

static void inbox_cmd_post(solar_os_shell_io_t *io, int argc, char **argv)
{
    char body[SOLAR_OS_INBOX_BODY_MAX] = {0};
    size_t used = 0;
    for (int i = 3; i < argc; i++) {
        const int written = snprintf(body + used,
                                     sizeof(body) - used,
                                     "%s%s",
                                     used > 0 ? " " : "",
                                     argv[i]);
        if (written < 0 || (size_t)written >= sizeof(body) - used) {
            solar_os_shell_io_writeln(io, "inbox: message too long");
            return;
        }
        used += (size_t)written;
    }

    const solar_os_inbox_publish_t message = {
        .source = argv[2],
        .body = body,
        .priority = SOLAR_OS_INBOX_PRIORITY_NORMAL,
    };
    uint32_t id = 0;
    const esp_err_t err = solar_os_inbox_publish(&message, &id);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "inbox: post failed: %s\n", esp_err_to_name(err));
        return;
    }
    solar_os_shell_io_printf(io, "posted message %lu\n", (unsigned long)id);
}

void solar_os_shell_cmd_inbox(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = inbox_terminal(ctx);
    if (io == NULL) {
        return;
    }

    if (argc == 1) {
        const esp_err_t err = solar_os_context_request_launch(ctx, &solar_os_inbox_app, 0, NULL);
        if (err != ESP_OK) {
            solar_os_shell_io_printf(io, "inbox: launch failed: %s\n", esp_err_to_name(err));
        } else {
            solar_os_shell_session_prepare_foreground_launch(ctx, false);
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        inbox_cmd_status(io);
        return;
    }
    if (strcmp(argv[1], "list") == 0 && argc <= 3) {
        bool unread_only = false;
        if (argc == 3) {
            if (strcmp(argv[2], "unread") == 0) {
                unread_only = true;
            } else if (strcmp(argv[2], "all") != 0) {
                inbox_print_usage(io);
                return;
            }
        }
        inbox_cmd_list(io, unread_only);
        return;
    }
    if (strcmp(argv[1], "read") == 0 && argc == 3) {
        uint32_t id = 0;
        if (!inbox_parse_id(argv[2], &id)) {
            inbox_print_usage(io);
            return;
        }
        inbox_cmd_read(io, id);
        return;
    }
    if (strcmp(argv[1], "clear") == 0 && argc == 2) {
        solar_os_inbox_status_t status = {0};
        (void)solar_os_inbox_get_status(&status);
        const esp_err_t err = solar_os_inbox_clear();
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io, "cleared %u messages\n", (unsigned)status.count);
        } else {
            solar_os_shell_io_printf(io, "inbox: clear failed: %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (strcmp(argv[1], "post") == 0 && argc >= 4) {
        inbox_cmd_post(io, argc, argv);
        return;
    }

    inbox_print_usage(io);
}
