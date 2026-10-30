#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_memory.h"
#include "solar_os_messages_app.h"
#include "solar_os_messaging.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_shell.h"
#if SOLAR_OS_PACKAGE_SERVICE_SC2_PROVIDER
#include "solar_os_sc2.h"
#endif
#if SOLAR_OS_PACKAGE_JOB_SC2_SYNC
#include <stdio.h>

#include "sc2_codec.h"
#include "solar_os_sc2_discover.h"
#endif

static const char * const messages_commands[] = {
    "status", "conversations", "list", "send", "read", "delete",
    "clear", "outbox", "cancel", "join", "leave", "query", "connect", "disconnect", "remove", "classes", "instances", "add", "configure",
};

static void messages_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  messages (instance, connection, and history TUI)");
    solar_os_shell_io_writeln(io, "  messages status");
    solar_os_shell_io_writeln(io, "  messages classes|instances");
    solar_os_shell_io_writeln(io, "  messages add <class> <instance> <settings...>");
    solar_os_shell_io_writeln(io, "  messages configure <instance> <settings...>");
    solar_os_shell_io_writeln(io, "  messages remove <instance|id> (delete history)");
    solar_os_shell_io_writeln(io, "  messages join|query <instance> <target>");
#if SOLAR_OS_PACKAGE_SERVICE_SC2_PROVIDER
    solar_os_shell_io_writeln(io, "    sc2 query target: device id, unique prefix (>= 6 hex) or `sc2 alias` name");
#endif
    solar_os_shell_io_writeln(io, "  messages leave <conversation-id>");
    solar_os_shell_io_writeln(io, "  messages connect|disconnect <instance>");
    solar_os_shell_io_writeln(io, "  messages conversations");
    solar_os_shell_io_writeln(io, "  messages list <conversation-id>");
    solar_os_shell_io_writeln(
        io,
        "  messages send <conversation-id> <text> [--allow-untrusted]");
    solar_os_shell_io_writeln(io, "  messages read <conversation-id>");
    solar_os_shell_io_writeln(io, "  messages delete <message-id>");
    solar_os_shell_io_writeln(
        io,
        "  messages clear <instance|all>");
    solar_os_shell_io_writeln(io, "  messages outbox");
    solar_os_shell_io_writeln(io, "  messages cancel <message-id>");
}

static bool messages_parse_u32(const char *text, uint32_t *value);

#if SOLAR_OS_PACKAGE_JOB_SC2_SYNC
/* messages add sc2 NAME DOMAIN [ADMIT] [key=B64]: the relay settings come
 * from its signed HTTPS descriptor; its key is pinned in the profile. */
#define MESSAGES_KEYARG_MAX (4u + 44u + 1u)
static void messages_sc2_add_domain(solar_os_shell_io_t *io, const char *name, int argc, char **argv)
{
    const char *admit = NULL;
    uint8_t expect[SC2_KEY_LEN];
    bool have_expect = false;
    for (int i = 1; i < argc; i++) {
        size_t got = 0;
        if (strncmp(argv[i], "key=", 4) == 0 && !have_expect &&
            sc2_b64_decode(expect, sizeof(expect), &got, argv[i] + 4, strlen(argv[i] + 4)) == SC2_OK &&
            got == sizeof(expect)) {
            have_expect = true;
        } else if (strncmp(argv[i], "key=", 4) != 0 && admit == NULL) {
            admit = argv[i];
        } else {
            solar_os_shell_io_writeln(io, "messages: usage: messages add sc2 <name> <domain> [admit] [key=<base64>]");
            return;
        }
    }
    sc2_desc_t *d = solar_os_memory_alloc(sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "messages.sc2.desc");
    if (d == NULL) {
        solar_os_shell_io_printf(io, "messages: %s\n", solar_os_shell_error_text(ESP_ERR_NO_MEM));
        return;
    }
    char why[112] = "";
    solar_os_shell_io_printf(io, "messages: fetching https://%s/.well-known/solaros-relay\n", argv[0]);
    esp_err_t error = solar_os_sc2_discover(argv[0], have_expect ? expect : NULL, d, why, sizeof(why));
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io, "messages: refused: %s\n", why);
        solar_os_memory_free(d);
        return;
    }
    char url[128 + 6 + 72];
    char keyarg[MESSAGES_KEYARG_MAX];
    size_t kn = 0;
    int un = snprintf(url, sizeof(url), "chats://%s:%u%s%s", d->host[0] != '\0' ? d->host : argv[0],
                      (unsigned)d->port, d->pin[0] != '\0' ? "#pin=" : "", d->pin);
    memcpy(keyarg, "key=", 4);
    (void)sc2_b64_encode(keyarg + 4, sizeof(keyarg) - 4, &kn, d->key, SC2_KEY_LEN);
    const char *settings[3] = {url, admit != NULL ? admit : keyarg, keyarg};
    solar_os_messaging_provider_id_t id = 0;
    error = un > 0 && (size_t)un < sizeof(url) ?
        solar_os_messaging_instance_add("sc2", name, admit != NULL ? 3 : 2, settings, &id) : ESP_ERR_INVALID_SIZE;
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io, "messages: %s\n", solar_os_shell_error_text(error));
        solar_os_memory_free(d);
        return;
    }
    solar_os_shell_io_printf(io, "messages: saved instance %s (%" PRIu32 "): relay %s at %s, key %s pinned\n", name,
                             id, d->relay_id, url, keyarg + 4);
    if (d->fallback_count > 0) {
        /* "fallback URL key=B64 ..." from the signed list */
        char (*keys)[MESSAGES_KEYARG_MAX] = solar_os_memory_alloc(
            SC2_DESC_FALLBACK_MAX * MESSAGES_KEYARG_MAX, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "messages.sc2.fb");
        const char *fb[1 + 2 * SC2_DESC_FALLBACK_MAX];
        int n = 0;
        fb[n++] = "fallback";
        for (size_t i = 0; keys != NULL && i < d->fallback_count; i++) {
            memcpy(keys[i], "key=", 4);
            (void)sc2_b64_encode(keys[i] + 4, MESSAGES_KEYARG_MAX - 4, &kn, d->fallback[i].key, SC2_KEY_LEN);
            fb[n++] = d->fallback[i].url;
            fb[n++] = keys[i];
        }
        error = keys != NULL ? solar_os_messaging_instance_configure(id, n, fb) : ESP_ERR_NO_MEM;
        if (error == ESP_OK) {
            for (size_t i = 0; i < d->fallback_count; i++) {
                solar_os_shell_io_printf(io, "messages:   fallback %u: %s (key pinned)\n", (unsigned)(i + 1),
                                         d->fallback[i].url);
            }
        } else {
            solar_os_shell_io_printf(io, "messages: fallback list not saved: %s\n", solar_os_shell_error_text(error));
        }
        solar_os_memory_free(keys);
    }
    solar_os_memory_free(d);
}
#endif

static bool messages_parse_provider(
    const char *text,
    solar_os_messaging_provider_id_t *provider)
{
    if (text == NULL || provider == NULL) {
        return false;
    }
    if (strcmp(text, "all") == 0) {
        *provider = 0;
        return true;
    }
    solar_os_messaging_provider_id_t instance = solar_os_messaging_provider_find(text);
    if (instance) { *provider = instance; return true; }
    solar_os_messaging_provider_status_t status;
    if (messages_parse_u32(text, &instance) &&
        solar_os_messaging_provider_get_status(instance, &status) == ESP_OK) {
        *provider = instance; return true;
    }
    if (strcmp(text, "gateway") == 0) {
        *provider = SOLAR_OS_MESSAGING_PROVIDER_GATEWAY;
        return true;
    }
    if (strcmp(text, "meshcore") == 0) {
        *provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE;
        return true;
    }
    if (strcmp(text, "link") == 0) {
        *provider = SOLAR_OS_MESSAGING_PROVIDER_LINK;
        return true;
    }
    return false;
}

static bool messages_parse_u32(const char *text, uint32_t *value)
{
    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 ||
        parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool messages_parse_u64(const char *text, uint64_t *value)
{
    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 16);
    if (end == text || *end != '\0' || parsed == 0) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static void messages_status(solar_os_shell_io_t *io)
{
    solar_os_messaging_status_t status;
    const esp_err_t error = solar_os_messaging_get_status(&status);
    if (error != ESP_OK) {
        solar_os_shell_io_table_printf(io,
                                 "messages: unavailable: %s\n",
                                 solar_os_shell_error_text(error));
        return;
    }
    solar_os_shell_io_table_printf(
        io,
        "Conversations: %u/%u\n"
        "Messages: %u (%u unread)\n"
        "RAM cache: %u/%u\n"
        "Pending outbox: %u/%u (volatile)\n"
        "History: %s\n"
        "Dropped: messages %" PRIu32 ", outbox %" PRIu32 "\n",
        (unsigned)status.conversations,
        (unsigned)SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY,
        (unsigned)status.messages,
        (unsigned)status.unread,
        (unsigned)status.cached_messages,
        (unsigned)SOLAR_OS_MESSAGING_MESSAGE_CAPACITY,
        (unsigned)status.queued_outbox,
        (unsigned)SOLAR_OS_MESSAGING_OUTBOX_CAPACITY,
        status.persistent ? "disk archive (available disk space)" : "unavailable",
        status.dropped_messages,
        status.dropped_outbox);
    if (status.storage_error != ESP_OK) {
        solar_os_shell_io_table_printf(io,
                                 "Storage error: %s\n",
                                 solar_os_shell_error_text(status.storage_error));
    }
    solar_os_messaging_provider_id_t provider = 0;
    solar_os_messaging_provider_status_t provider_status;
    while (solar_os_messaging_provider_next(&provider, &provider_status)) {
        {
            solar_os_shell_io_table_printf(
                io,
                "Provider %-8s: %s%s%s%s\n",
                provider_status.name,
                provider_status.running ? "running" : "stopped",
                provider_status.connected ? ", connected" : "",
                provider_status.detail[0] != '\0' ? ", " : "",
                provider_status.detail[0] != '\0' ?
                    provider_status.detail : "");
        }
    }
}

static void messages_outbox(solar_os_shell_io_t *io)
{
    solar_os_messaging_outbound_t *requests = solar_os_memory_calloc(
        SOLAR_OS_MESSAGING_OUTBOX_CAPACITY,
        sizeof(*requests),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "messages.shell.outbox");
    if (requests == NULL) {
        solar_os_shell_io_table_writeln(io, "outbox: no PSRAM for snapshot");
        return;
    }
    const size_t count = solar_os_messaging_outbox_snapshot(
        requests,
        SOLAR_OS_MESSAGING_OUTBOX_CAPACITY);
    for (size_t i = 0; i < count; i++) {
        const solar_os_messaging_outbound_t *request = &requests[i];
        solar_os_shell_io_table_printf(
            io,
            "%016" PRIx64 "  %-8s conversation=%" PRIu32
            " attempts=%u  %s\n",
            request->message_key,
            solar_os_messaging_provider_name(request->provider),
            request->conversation_id,
            (unsigned)request->attempts,
            request->body);
    }
    if (count == 0U) {
        solar_os_shell_io_table_writeln(io, "Outbox is empty");
    }
    solar_os_memory_free(requests);
}

static void messages_conversations(solar_os_shell_io_t *io)
{
    solar_os_messaging_conversation_t *conversations =
        solar_os_memory_calloc(SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY,
                               sizeof(*conversations),
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "messages.shell.conversations");
    if (conversations == NULL) {
        solar_os_shell_io_table_writeln(io, "messages: no PSRAM for snapshot");
        return;
    }
    const size_t count = solar_os_messaging_conversation_snapshot(
        conversations,
        SOLAR_OS_MESSAGING_CONVERSATION_CAPACITY);
    for (size_t i = 0; i < count; i++) {
        const solar_os_messaging_conversation_t *conversation =
            &conversations[i];
        char label[SOLAR_OS_MESSAGING_TITLE_MAX];
        solar_os_messaging_conversation_label(conversation, label, sizeof(label));
        solar_os_shell_io_table_printf(
            io,
            "%" PRIu32 "  %-8s %-9s unread=%" PRIu32
            " security=0x%02" PRIx32 "  %s\n",
            conversation->id,
            solar_os_messaging_provider_name(conversation->provider),
            solar_os_conversation_kind_name(conversation->kind),
            conversation->unread_count,
            conversation->security_flags,
            label);
    }
    if (count == 0) {
        solar_os_shell_io_table_writeln(io, "No conversations");
    }
    solar_os_memory_free(conversations);
}

typedef struct {
    solar_os_shell_io_t *io;
    size_t count;
} messages_list_context_t;

static bool messages_list_visit(
    const solar_os_messaging_message_t *message,
    void *user)
{
    messages_list_context_t *context = user;
    solar_os_shell_io_table_printf(
        context->io,
        "%016" PRIx64 "  %-3s %-9s sec=0x%02" PRIx32 "  %s%s%s\n",
        message->key,
        message->direction == SOLAR_OS_MESSAGE_INBOUND ? "in" : "out",
        solar_os_delivery_state_name(message->delivery),
        message->security_flags,
        message->sender[0] != '\0' ? message->sender : "",
        message->sender[0] != '\0' ? ": " : "",
        message->body);
    context->count++;
    return true;
}

static void messages_list(solar_os_shell_io_t *io,
                          solar_os_conversation_id_t conversation_id)
{
    messages_list_context_t context = {
        .io = io,
    };
    (void)solar_os_messaging_message_visit(conversation_id,
                                           0,
                                           messages_list_visit,
                                           &context,
                                           NULL);
    if (context.count == 0) {
        solar_os_shell_io_writeln(io, "No messages");
    }
}

static void messages_send(solar_os_shell_io_t *io,
                          int argc,
                          char **argv,
                          solar_os_conversation_id_t conversation_id)
{
    bool allow_untrusted = false;
    char *body = solar_os_memory_calloc(1,
                                        SOLAR_OS_MESSAGING_BODY_MAX,
                                        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                        "messages.shell.body");
    if (body == NULL) {
        solar_os_shell_io_writeln(io, "messages: no PSRAM for message");
        return;
    }
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--allow-untrusted") == 0) {
            allow_untrusted = true;
            continue;
        }
        if (body[0] != '\0' &&
            strlcat(body, " ", SOLAR_OS_MESSAGING_BODY_MAX) >=
                SOLAR_OS_MESSAGING_BODY_MAX) {
            solar_os_memory_free(body);
            solar_os_shell_io_writeln(io, "messages: body too long");
            return;
        }
        if (strlcat(body, argv[i], SOLAR_OS_MESSAGING_BODY_MAX) >=
            SOLAR_OS_MESSAGING_BODY_MAX) {
            solar_os_memory_free(body);
            solar_os_shell_io_writeln(io, "messages: body too long");
            return;
        }
    }
    if (body[0] == '\0') {
        solar_os_memory_free(body);
        messages_usage(io);
        return;
    }
    solar_os_message_key_t message_key = 0;
    const esp_err_t error =
        solar_os_messaging_send(conversation_id,
                                body,
                                allow_untrusted,
                                &message_key);
    solar_os_memory_free(body);
    if (error == ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "messages: queued %016" PRIx64 "\n",
                                 message_key);
    } else {
        solar_os_shell_io_printf(io,
                                 "messages: send failed: %s\n",
                                 solar_os_shell_error_text(error));
    }
}

void solar_os_shell_cmd_messages(solar_os_context_t *ctx,
                                 int argc,
                                 char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 1) {
        esp_err_t error = solar_os_context_request_launch(ctx, &solar_os_messages_app, 0, NULL);
        if (error == ESP_OK) solar_os_shell_session_prepare_foreground_launch(ctx, false);
        else solar_os_shell_io_printf(io, "messages: launch failed: %s\n", solar_os_shell_error_text(error));
        return;
    }
    if (argc == 2 && strcmp(argv[1], "classes") == 0) {
        char cursor[16] = ""; solar_os_messaging_class_status_t status;
        while (solar_os_messaging_class_next(cursor, &status)) {
            solar_os_shell_io_printf(io, "%s%s\n", status.name, status.creatable ? " (configurable)" : "");
            strlcpy(cursor, status.name, sizeof(cursor));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "instances") == 0) {
        solar_os_messaging_provider_id_t cursor = 0; solar_os_messaging_provider_status_t status;
        while (solar_os_messaging_provider_next(&cursor, &status))
            solar_os_shell_io_table_printf(io, "%" PRIu32 " %s %s: %s, %s\n", cursor, status.class_name,
                status.name, status.connected ? "connected" : status.running ? "running" : "stopped", status.detail);
        return;
    }
    if (argc >= 5 && strcmp(argv[1], "add") == 0 && strcmp(argv[2], "sc2") == 0 && strstr(argv[4], "://") == NULL) {
#if SOLAR_OS_PACKAGE_JOB_SC2_SYNC
        messages_sc2_add_domain(io, argv[3], argc - 4, argv + 4);
#else
        solar_os_shell_io_writeln(io, "messages: adding an sc2 relay by domain needs sc2-sync in this build");
#endif
        return;
    }
    if (argc >= 4 && strcmp(argv[1], "add") == 0) {
        solar_os_messaging_provider_id_t id;
        esp_err_t error = solar_os_messaging_instance_add(argv[2], argv[3], argc - 4, (const char *const *)(argv + 4), &id);
        if (error == ESP_OK) solar_os_shell_io_printf(io, "messages: saved instance %s (%" PRIu32 ")\n", argv[3], id);
        else solar_os_shell_io_printf(io, "messages: %s\n", solar_os_shell_error_text(error));
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "configure") == 0) {
        solar_os_messaging_provider_id_t id;
        esp_err_t error = messages_parse_provider(argv[2], &id) && id ?
            solar_os_messaging_instance_configure(id, argc - 3, (const char *const *)(argv + 3)) : ESP_ERR_NOT_FOUND;
        solar_os_shell_io_printf(io, "messages: %s\n", error == ESP_OK ? "configuration saved" : solar_os_shell_error_text(error));
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        messages_status(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "conversations") == 0) {
        messages_conversations(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "outbox") == 0) {
        messages_outbox(io);
        return;
    }
    uint32_t conversation_id = 0;
    if (argc == 3 && strcmp(argv[1], "remove") == 0) {
        solar_os_messaging_provider_id_t instance;
        esp_err_t error = messages_parse_provider(argv[2], &instance) && instance ?
            solar_os_messaging_instance_remove(instance) : ESP_ERR_NOT_FOUND;
        solar_os_shell_io_printf(io, "messages: %s\n", error == ESP_OK ?
            "instance and history removed" : solar_os_shell_error_text(error)); return;
    }
    if (argc == 3 && strcmp(argv[1], "list") == 0 &&
        messages_parse_u32(argv[2], &conversation_id)) {
        messages_list(io, conversation_id);
        return;
    }
    if (argc >= 4 && strcmp(argv[1], "send") == 0 &&
        messages_parse_u32(argv[2], &conversation_id)) {
        messages_send(io, argc, argv, conversation_id);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "read") == 0 &&
        messages_parse_u32(argv[2], &conversation_id)) {
        const esp_err_t error =
            solar_os_messaging_mark_read(conversation_id);
        solar_os_shell_io_printf(io,
                                 "messages: %s\n",
                                 error == ESP_OK ? "read" :
                                     solar_os_shell_error_text(error));
        return;
    }
    uint64_t message_id = 0;
    if (argc == 3 && strcmp(argv[1], "delete") == 0 &&
        messages_parse_u64(argv[2], &message_id)) {
        const esp_err_t error =
            solar_os_messaging_message_delete(message_id);
        solar_os_shell_io_printf(io,
                                 "messages: %s\n",
                                 error == ESP_OK ? "deleted" :
                                     solar_os_shell_error_text(error));
        return;
    }
    if (argc == 3 && strcmp(argv[1], "leave") == 0 && messages_parse_u32(argv[2], &conversation_id)) {
        esp_err_t error = solar_os_messaging_leave(conversation_id);
        solar_os_shell_io_printf(io, "messages: %s\n", error == ESP_OK ? "leave requested" : solar_os_shell_error_text(error)); return;
    }
    if (argc == 3 && (strcmp(argv[1], "connect") == 0 || strcmp(argv[1], "disconnect") == 0)) {
        solar_os_messaging_provider_id_t instance = 0;
        esp_err_t error = messages_parse_provider(argv[2], &instance) && instance ?
            solar_os_messaging_connect(instance, strcmp(argv[1], "connect") == 0) : ESP_ERR_NOT_FOUND;
        solar_os_shell_io_printf(io, "messages: %s\n", error == ESP_OK ? "connection setting saved" : solar_os_shell_error_text(error)); return;
    }
    if (argc == 4 && (strcmp(argv[1], "join") == 0 || strcmp(argv[1], "query") == 0)) {
        solar_os_messaging_provider_id_t instance = 0;
        (void)messages_parse_provider(argv[2], &instance);
        const char *target = argv[3];
#if SOLAR_OS_PACKAGE_SERVICE_SC2_PROVIDER
        /* sc2 peers: full device id, unique prefix of a known peer, or alias */
        char peer[SC2_DEVICE_ID_HEX_LEN + 1];
        if (strcmp(argv[1], "query") == 0 && solar_os_messaging_provider_is_class(instance, "sc2")) {
            esp_err_t resolved = solar_os_sc2_resolve_peer(target, peer);
            if (resolved != ESP_OK) {
                solar_os_shell_io_printf(io, "messages: %s: %s\n", target, solar_os_sc2_peer_ref_error(resolved));
                return;
            }
            target = peer;
        }
#endif
        esp_err_t error = strcmp(argv[1], "join") == 0 ? solar_os_messaging_join(instance, target, &conversation_id) :
            solar_os_messaging_open_direct(instance, target, &conversation_id);
        if (error == ESP_OK) solar_os_shell_io_printf(io, "messages: conversation %" PRIu32 "%s%s\n", conversation_id,
                                                      target != argv[3] ? " with " : "", target != argv[3] ? target : "");
        else solar_os_shell_io_printf(io, "messages: %s\n", solar_os_shell_error_text(error));
#if SOLAR_OS_PACKAGE_SERVICE_SC2_PROVIDER
        solar_os_messaging_conversation_t *joined = error == ESP_OK && strcmp(argv[1], "join") == 0 &&
                solar_os_messaging_provider_is_class(instance, "sc2") ?
            solar_os_memory_alloc(sizeof(*joined), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "messages.shell.join") : NULL;
        if (joined != NULL) {
            if (solar_os_messaging_conversation_get(conversation_id, joined) == ESP_OK &&
                (joined->security_flags & SOLAR_OS_SECURITY_PUBLIC_SIGNED) != 0u &&
                !solar_os_sc2_notice_seen(joined->provider_key)) {
                solar_os_shell_io_printf(io, "messages: %s\n", SOLAR_OS_SC2_PUBLIC_NOTICE);
                (void)solar_os_sc2_notice_mark(joined->provider_key);
            }
            solar_os_memory_free(joined);
        }
#endif
        return;
    }
    solar_os_messaging_provider_id_t provider = 0;
    if (argc == 3 && strcmp(argv[1], "clear") == 0 &&
        messages_parse_provider(argv[2], &provider)) {
        size_t removed = 0;
        const esp_err_t error = solar_os_messaging_clear(provider, &removed);
        if (error == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "messages: cleared %u %s messages\n",
                                     (unsigned)removed,
                                     argv[2]);
        } else {
            solar_os_shell_io_printf(io,
                                     "messages: clear failed: %s\n",
                                     solar_os_shell_error_text(error));
        }
        return;
    }
    if (argc == 3 && strcmp(argv[1], "cancel") == 0 &&
        messages_parse_u64(argv[2], &message_id)) {
        const esp_err_t error = solar_os_messaging_cancel(message_id);
        solar_os_shell_io_printf(io,
                                 "messages: %s\n",
                                 error == ESP_OK ? "cancelled" :
                                     solar_os_shell_error_text(error));
        return;
    }
    solar_os_shell_diag_subcommand(io,
                                   "messages",
                                   argc,
                                   argv,
                                   "messages classes|instances|add|configure|status|conversations|list|send|read|delete|clear|outbox|cancel|join|leave|query|connect|disconnect|remove",
                                   messages_commands,
                                   sizeof(messages_commands) / sizeof(messages_commands[0]));
}

void solar_os_shell_cmd_outbox(solar_os_context_t *ctx,
                               int argc,
                               char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 1 ||
        (argc == 2 && strcmp(argv[1], "list") == 0)) {
        messages_outbox(io);
        return;
    }
    uint64_t message_id = 0;
    if (argc == 3 && strcmp(argv[1], "cancel") == 0 &&
        messages_parse_u64(argv[2], &message_id)) {
        const esp_err_t error = solar_os_messaging_cancel(message_id);
        solar_os_shell_io_printf(io,
                                 "outbox: %s\n",
                                 error == ESP_OK ? "cancelled" :
                                     solar_os_shell_error_text(error));
        return;
    }
    solar_os_shell_io_writeln(io, "usage: outbox [list]");
    solar_os_shell_io_writeln(io, "       outbox cancel <message-id>");
}
