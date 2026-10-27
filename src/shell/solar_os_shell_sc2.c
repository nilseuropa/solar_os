#include "solar_os_shell_commands.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_sc2.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#if SOLAR_OS_PACKAGE_JOB_SC2_SYNC
#include <stdio.h>
#include <sys/stat.h>
#include "solar_os_memory.h"
#include "solar_os_sc2_sync.h"
#include "solar_os_shell.h"
#include "solar_os_storage.h"
#endif

static void sc2_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "sc2 id                show this device's SC2 identity");
    solar_os_shell_io_writeln(io, "sc2 status            service, store and session counts");
    solar_os_shell_io_writeln(io, "sc2 safety <peer>     safety number with a stored peer");
    solar_os_shell_io_writeln(io, "sc2 verify|unverify <peer>  mark the safety number compared (or not)");
    solar_os_shell_io_writeln(io, "sc2 alias [<name> <peer>|<name> off]  list, save or delete a peer nickname");
    solar_os_shell_io_writeln(io, "<peer>: 32-hex device id, unique prefix (>= 6 hex) of a known peer, or alias");
    solar_os_shell_io_writeln(io, "sc2 selftest          run known-answer and session tests");
    solar_os_shell_io_writeln(io, "sc2 bench [n]         time the primitives (n iterations, default 10)");
#if SOLAR_OS_PACKAGE_JOB_SC2_SYNC
    solar_os_shell_io_writeln(io, "sc2 relay             relay connection (sc2-sync) status");
    solar_os_shell_io_writeln(io, "relay setup: messages add sc2 <name> chat://HOST:7777 | chats://HOST:7778[#pin=sha256/B64]");
    solar_os_shell_io_writeln(io, "add:         messages add sc2 <name> <domain> [admit] [key=<b64>] (signed HTTPS descriptor)");
    solar_os_shell_io_writeln(io, "fallbacks:   messages configure <name> fallback <url> [key=<b64>] [<url> ...] | fallback off");
    solar_os_shell_io_writeln(io, "peer: messages query <name> <peer>, then messages send <conv> <text>");
    solar_os_shell_io_writeln(io, "sc2 rooms             rooms of the relay instance and their state");
    solar_os_shell_io_writeln(io, "sc2 room create [name]  new private room; quote names containing spaces");
    solar_os_shell_io_writeln(io, "sc2 room invite <room>  print the invite again (ID, prefix or unique name)");
    solar_os_shell_io_writeln(io, "sc2 room join <sc2i:...|@file>  join a private room with an invite");
    solar_os_shell_io_writeln(io, "private room: messages send <conv> <text>; leave with messages leave <conv>");
#endif
}

static void sc2_fail(solar_os_shell_io_t *io, esp_err_t err)
{
    solar_os_shell_io_printf(io, "sc2: %s\n", solar_os_sc2_err_text(err));
}

static void sc2_print_identity(solar_os_shell_io_t *io)
{
    sc2e_identity_info_t info;
    esp_err_t err = solar_os_sc2_identity(&info);
    if (err != ESP_OK) {
        sc2_fail(io, err);
        return;
    }
    solar_os_shell_io_printf(io, "device:      %s\n", info.device_id);
    solar_os_shell_io_printf(io, "fingerprint: %s\n", info.fingerprint);
    solar_os_shell_io_printf(io, "identity:    %s\n", info.ik_b64);
    solar_os_shell_io_printf(io, "device key:  %s\n", info.dk_b64);
    solar_os_shell_io_printf(io, "prekeys:     spk %" PRIu32 ", %" PRIu32 " one-time\n", info.spk_id, info.opk_count);
    solar_os_shell_io_writeln(io, "Keys are stored unencrypted on internal flash unless flash encryption is on.");
}

static void sc2_print_status(solar_os_shell_io_t *io)
{
    solar_os_sc2_status_t st;
    esp_err_t err = solar_os_sc2_get_status(&st);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "core:    %s, hash %s\n", st.core_version ? st.core_version : SC2_VERSION_STRING,
                                 st.hash_backend ? st.hash_backend : sc2_hash_backend());
        sc2_fail(io, err);
        return;
    }
    solar_os_shell_io_printf(io, "core:    %s, hash %s\n", st.core_version, st.hash_backend);
    solar_os_shell_io_printf(io, "device:  %s%s\n", st.device_id, st.created ? " (created this boot)" : "");
    solar_os_shell_io_printf(io, "store:   %s\n", st.store_path);
    solar_os_shell_io_printf(io, "memory:  work buffers in %s\n", st.work_in_psram ? "PSRAM" : "internal RAM");
    solar_os_shell_io_printf(io, "prekeys: spk %" PRIu32 ", %" PRIu32 " one-time\n", st.spk_id, st.opk_count);
    solar_os_shell_io_printf(io, "state:   %u peers, %u sessions, %u rooms\n", (unsigned)st.peers,
                             (unsigned)st.sessions, (unsigned)st.rooms);
}

static bool sc2_peer_arg(solar_os_shell_io_t *io, const char *ref, char out[SC2_DEVICE_ID_HEX_LEN + 1])
{
    esp_err_t err = solar_os_sc2_resolve_peer(ref, out);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "sc2: %s: %s\n", ref, solar_os_sc2_peer_ref_error(err));
        return false;
    }
    return true;
}

static void sc2_alias(solar_os_shell_io_t *io, int argc, char **argv)
{
    char name[SOLAR_OS_SC2_ALIAS_NAME_MAX + 1];
    char dev[SC2_DEVICE_ID_HEX_LEN + 1];
    if (argc == 2) {
        size_t i = 0;
        for (; i < SOLAR_OS_SC2_ALIAS_MAX && solar_os_sc2_alias_get(i, name, dev) == ESP_OK; i++) {
            solar_os_shell_io_printf(io, "%-16s %s\n", name, dev);
        }
        if (i == 0) {
            solar_os_shell_io_writeln(io, "sc2: no aliases (sc2 alias <name> <device-id>)");
        }
        return;
    }
    if (!solar_os_sc2_alias_name_valid(argv[2])) {
        solar_os_shell_io_writeln(io, "sc2: alias names are 1-16 of A-Z a-z 0-9 . _ - and not all hex digits");
        return;
    }
    bool off = strcmp(argv[3], "off") == 0;
    if (!off && !sc2_peer_arg(io, argv[3], dev)) {
        return;
    }
    esp_err_t err = solar_os_sc2_alias_set(argv[2], off ? NULL : dev);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(io, off ? "sc2: alias %s deleted\n" : "sc2: alias %s = %s\n", argv[2], dev);
    } else {
        solar_os_shell_io_printf(io, "sc2: alias: %s\n",
                                 err == ESP_ERR_NO_MEM ? "alias list is full (16)" :
                                 err == ESP_ERR_NOT_FOUND ? "no such alias" : solar_os_sc2_err_text(err));
    }
}

static void sc2_print_safety(solar_os_shell_io_t *io, const char *ref)
{
    sc2e_safety_t safety;
    char peer[SC2_DEVICE_ID_HEX_LEN + 1];
    if (!sc2_peer_arg(io, ref, peer)) {
        return;
    }
    esp_err_t err = solar_os_sc2_safety(peer, &safety);
    if (err != ESP_OK) {
        sc2_fail(io, err);
        return;
    }
    bool verified = false;
    (void)solar_os_sc2_verified_get(peer, &verified);
    solar_os_shell_io_printf(io, "peer:  %s (%s)\n", peer, verified ? "verified" : "NOT verified");
    solar_os_shell_io_printf(io, "short: %s\n", safety.short_form);
    solar_os_shell_io_printf(io, "words: %s\n", safety.words);
    solar_os_shell_io_printf(io, "full:  %s\n", safety.full_form);
    if (!verified) {
        solar_os_shell_io_printf(io, "Compare with the other device, then: sc2 verify %s\n", ref);
    }
}

static void sc2_verify(solar_os_shell_io_t *io, const char *ref, bool verified)
{
    char peer[SC2_DEVICE_ID_HEX_LEN + 1];
    if (!sc2_peer_arg(io, ref, peer)) {
        return;
    }
    esp_err_t err = solar_os_sc2_verified_set(peer, verified);
    if (err == ESP_OK) {
        solar_os_shell_io_printf(io, "sc2: %s %s\n", peer, verified ? "marked verified" : "marked NOT verified");
    } else {
        solar_os_shell_io_printf(io, "sc2: %s: %s\n", verified ? "verify" : "unverify",
                                 err == ESP_ERR_NOT_FOUND ? "no keys for this peer yet (message it first)" :
                                 err == ESP_ERR_NO_MEM ? "verified list is full (64)" : solar_os_sc2_err_text(err));
    }
}

static void sc2_selftest_line(void *ctx, const char *name, int rc)
{
    solar_os_shell_io_t *io = ctx;
    solar_os_shell_io_printf(io, "  %-20s %s\n", name, rc == SC2_OK ? "ok" : sc2_strerror(rc));
}

static void sc2_bench_line(void *ctx, const sc2e_bench_result_t *r)
{
    solar_os_shell_io_t *io = ctx;
    if (r->rc != SC2_OK || r->iterations == 0) {
        solar_os_shell_io_printf(io, "  %-24s failed: %s\n", r->name, sc2_strerror(r->rc));
        return;
    }
    uint64_t per = r->total_us / r->iterations;
    solar_os_shell_io_printf(io, "  %-24s %6" PRIu32 " x %8" PRIu64 " us  (total %" PRIu64 " ms)\n", r->name,
                             r->iterations, per, r->total_us / 1000u);
}

#if SOLAR_OS_PACKAGE_JOB_SC2_SYNC
#define SC2_ROOM_WAIT_MS 20000u

/* An invite from the command line or, with @path, from a file (invites are
 * longer than some terminals' input line). */
static bool sc2_invite_arg(solar_os_context_t *ctx, solar_os_shell_io_t *io, const char *arg, char *out, size_t cap)
{
    if (arg[0] != '@') {
        snprintf(out, cap, "%s", arg);
        return true;
    }
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx, io, "sc2", arg + 1, path, sizeof(path))) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        solar_os_shell_io_printf(io, "sc2: cannot read %s\n", arg + 1);
        return false;
    }
    size_t n = fread(out, 1, cap - 1u, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    if (more || n == 0) {
        solar_os_shell_io_printf(io, "sc2: %s does not hold one invite\n", arg + 1);
        return false;
    }
    return true;
}

static void sc2_room_cmd(solar_os_context_t *ctx, solar_os_shell_io_t *io, solar_os_sc2_room_op_t op,
                         const char *arg)
{
    char *buf = solar_os_memory_calloc(1, SOLAR_OS_SC2_ROOM_OUT_MAX, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.room");
    char *in = NULL;
    char room[SC2_DEVICE_ID_HEX_LEN + 1] = "";
    char why[96] = "";
    esp_err_t err;
    if (buf == NULL) {
        sc2_fail(io, ESP_ERR_NO_MEM);
        return;
    }
    if (op == SOLAR_OS_SC2_ROOM_JOIN) {
        in = solar_os_memory_calloc(1, SC2_INVITE_TEXT_MAX + 2u, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.room");
        if (in == NULL || !sc2_invite_arg(ctx, io, arg, in, SC2_INVITE_TEXT_MAX + 2u)) {
            if (in == NULL) {
                sc2_fail(io, ESP_ERR_NO_MEM);
            }
            goto out;
        }
        sc2_invite_t inv;
        if (sc2_invite_from_text(&inv, in, strlen(in)) != SC2_OK) {
            solar_os_shell_io_writeln(io, "sc2: not a valid invite (it starts with sc2i:)");
            goto out;
        }
        (void)sc2_hex_encode(room, sizeof(room), inv.room, 4u);
        solar_os_shell_io_printf(io, "invite for room %s... on %s\n", room, inv.url);
        sc2_memzero(&inv, sizeof(inv));
        arg = in;
    }
    err = solar_os_sc2_sync_room(op, arg, room, buf, SOLAR_OS_SC2_ROOM_OUT_MAX, why, sizeof(why),
                                           op == SOLAR_OS_SC2_ROOM_LIST || op == SOLAR_OS_SC2_ROOM_INVITE
                                               ? 3000u
                                               : SC2_ROOM_WAIT_MS);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "sc2: %s\n", why[0] != '\0' ? why : esp_err_to_name(err));
        if (err == ESP_ERR_TIMEOUT && op == SOLAR_OS_SC2_ROOM_CREATE) {
            solar_os_shell_io_writeln(io, "the room is kept if the relay confirms it: see `sc2 rooms`");
        }
        goto out;
    }
    switch (op) {
    case SOLAR_OS_SC2_ROOM_LIST:
        solar_os_shell_io_printf(io, "%s", buf[0] != '\0' ? buf : "no rooms\n");
        break;
    case SOLAR_OS_SC2_ROOM_CREATE:
    case SOLAR_OS_SC2_ROOM_INVITE:
        if (op == SOLAR_OS_SC2_ROOM_CREATE) {
            solar_os_shell_io_printf(io, "created private room %s (you are its admin)\n", room);
        }
        solar_os_shell_io_writeln(io, "invite (secret: whoever holds it can join; share it privately):");
        solar_os_shell_io_writeln(io, buf);
        solar_os_shell_io_writeln(io, "members are admitted while this device is online");
        break;
    case SOLAR_OS_SC2_ROOM_JOIN:
        solar_os_shell_io_printf(io, "joining private room %s; the admin's device admits you when online\n", room);
        solar_os_shell_io_writeln(io, "state: sc2 rooms; then messages send <conv> <text>");
        break;
    default:
        break;
    }
out:
    sc2_memzero(buf, SOLAR_OS_SC2_ROOM_OUT_MAX);
    solar_os_memory_free(buf);
    if (in != NULL) {
        sc2_memzero(in, SC2_INVITE_TEXT_MAX + 2u);
        solar_os_memory_free(in);
    }
}
#endif

void solar_os_shell_cmd_sc2(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (!io) {
        return;
    }
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        sc2_usage(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "id") == 0) {
        sc2_print_identity(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        sc2_print_status(io);
        return;
    }
#if SOLAR_OS_PACKAGE_JOB_SC2_SYNC
    if (argc == 2 && strcmp(argv[1], "relay") == 0) {
        solar_os_sc2_sync_status_t st;
        solar_os_sc2_sync_get_status(&st);
        if (!st.running) {
            solar_os_shell_io_writeln(io, "sc2-sync: not running (use `job start sc2-sync`)");
            return;
        }
        if (st.instance == 0) {
            solar_os_shell_io_writeln(io, "sc2-sync: running, no enabled sc2 instance");
            return;
        }
        solar_os_shell_io_printf(io, "sc2-sync: instance %u %s://%s:%u %s relay_id=%s outbox=%u retries=%u\n",
                                 st.instance, st.tls ? "chats" : "chat", st.host, st.port,
                                 st.ready ? "ready" : (st.connected ? "logging in" : "offline"),
                                 st.relay_id[0] ? st.relay_id : "-", st.outbox, st.attempt);
        if (st.relays > 1) {
            if (st.relay == 0) {
                solar_os_shell_io_printf(io, "relay:    primary (%u fallbacks)%s\n", st.relays - 1u,
                                         st.visiting ? ", probing" : "");
            } else {
                solar_os_shell_io_printf(io, "relay:    fallback %u of %u%s\n", st.relay, st.relays - 1u,
                                         st.visiting ? ", visiting (keys and mailbox)" : "");
            }
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "rooms") == 0) {
        sc2_room_cmd(ctx, io, SOLAR_OS_SC2_ROOM_LIST, NULL);
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "room") == 0) {
        if ((argc == 3 || argc == 4) && strcmp(argv[2], "create") == 0) {
            if (argc == 4 && !sc2e_label_valid(argv[3], SC2E_ROOM_TITLE_MAX)) {
                solar_os_shell_io_writeln(io, "sc2: room name must be 1-48 bytes of printable UTF-8");
                return;
            }
            sc2_room_cmd(ctx, io, SOLAR_OS_SC2_ROOM_CREATE, argc == 4 ? argv[3] : NULL);
            return;
        }
        if (argc == 4 && strcmp(argv[2], "invite") == 0) {
            sc2_room_cmd(ctx, io, SOLAR_OS_SC2_ROOM_INVITE, argv[3]);
            return;
        }
        if (argc == 4 && strcmp(argv[2], "join") == 0) {
            sc2_room_cmd(ctx, io, SOLAR_OS_SC2_ROOM_JOIN, argv[3]);
            return;
        }
    }
#endif
    if (argc == 3 && strcmp(argv[1], "safety") == 0) {
        sc2_print_safety(io, argv[2]);
        return;
    }
    if (argc == 3 && (strcmp(argv[1], "verify") == 0 || strcmp(argv[1], "unverify") == 0)) {
        sc2_verify(io, argv[2], argv[1][0] == 'v');
        return;
    }
    if ((argc == 2 || argc == 4) && strcmp(argv[1], "alias") == 0) {
        sc2_alias(io, argc, argv);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
        unsigned passed = 0, failed = 0;
        solar_os_shell_io_printf(io, "%s, hash %s\n", SC2_VERSION_STRING, sc2_hash_backend());
        esp_err_t err = solar_os_sc2_selftest(sc2_selftest_line, io, &passed, &failed);
        solar_os_shell_io_printf(io, "selftest: %u passed, %u failed\n", passed, failed);
        if (err != ESP_OK && failed == 0) {
            sc2_fail(io, err);
        }
        return;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "bench") == 0) {
        uint32_t n = 10;
        if (argc == 3) {
            char *end = NULL;
            unsigned long v = strtoul(argv[2], &end, 10);
            if (argv[2][0] == '\0' || *end != '\0' || v == 0 || v > 1000) {
                solar_os_shell_io_writeln(io, "sc2: iterations must be 1..1000");
                return;
            }
            n = (uint32_t)v;
        }
        solar_os_shell_io_printf(io, "%s, hash %s, %" PRIu32 " iterations\n", SC2_VERSION_STRING,
                                 sc2_hash_backend(), n);
        esp_err_t err = solar_os_sc2_bench(n, sc2_bench_line, io);
        if (err != ESP_OK) {
            sc2_fail(io, err);
        }
        return;
    }
    sc2_usage(io);
}
