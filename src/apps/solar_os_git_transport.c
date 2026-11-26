#include "solar_os_git_transport.h"
#include "solar_os_git_state.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "solar_os_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "git2/sys/stream.h"
#include "git2/sys/errors.h"
#include "git2/sys/transport.h"
#include "solar_os_native_ssh_exec.h"
#include "solar_os_native_ssh_exec_abi.h"
#include "solar_os_memory.h"

#define GIT_IO_TIMEOUT_MS 30000U

/* libgit2 is serialized by solar_os_git_run; stream constructors have no
 * payload slot. The pointer is valid only for the duration of that call. */
static SOLAR_OS_GIT_STATE_BSS const solar_os_git_options_t *active_options;

static bool stopped(void *unused)
{
    (void)unused;
    return active_options && active_options->cancelled &&
        active_options->cancelled(active_options->user);
}

static int transport_error(const char *message)
{
    git_error_set_str(GIT_ERROR_NET, message);
    return -1;
}

static void ssh_error(void *user, const char *message)
{
    *(bool *)user = true;
    git_error_set_str(GIT_ERROR_SSH, message);
}

static void ssh_output(void *unused, const void *bytes, size_t size)
{
    (void)unused;
    if (active_options && active_options->output) {
        active_options->output(active_options->user, bytes, size);
    }
}

static solar_os_native_ssh_exec_io_v1_t ssh_io(bool *reported)
{
    return (solar_os_native_ssh_exec_io_v1_t){ .cancelled = stopped, .diagnostic = ssh_error,
        .stderr_data = ssh_output, .user = reported };
}

static int ssh_failure(int result, bool reported, const char *message)
{
    if (reported) return -1;
    if (result == SOLAR_OS_NATIVE_ERROR_CANCELLED) return transport_error("cancelled");
    if (result == SOLAR_OS_NATIVE_ERROR_TIMEOUT) return transport_error("SSH Git I/O timed out");
    return transport_error(message);
}

static bool copy_part(char *out, size_t size, const char *start, size_t len)
{
    if (!len || len >= size) return false;
    memcpy(out, start, len); out[len] = 0;
    return true;
}

bool solar_os_git_parse_ssh_url(const char *url, solar_os_git_ssh_url_t *out)
{
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));
    out->port = 22;
    strcpy(out->username, "git");
    bool uri = strncmp(url, "ssh://", 6) == 0;
    const char *authority = uri ? url + 6 : url;
    if (!uri && strstr(url, "://")) return false;
    const char *end = uri ? strchr(authority, '/') : strchr(authority, ':');
    if (!end || end == authority) return false;
    const char *at = memchr(authority, '@', (size_t)(end - authority));
    const char *host = authority;
    if (at) {
        if (!copy_part(out->username, sizeof(out->username), authority, (size_t)(at - authority))) return false;
        host = at + 1;
    }
    const char *host_end = end;
    if (uri) {
        const char *colon = memchr(host, ':', (size_t)(end - host));
        if (colon) {
            unsigned port = 0;
            if (colon + 1 == end) return false;
            for (const char *p = colon + 1; p < end; p++) {
                if (!isdigit((unsigned char)*p)) return false;
                port = port * 10 + (unsigned)(*p - '0');
                if (!port || port > 65535) return false;
            }
            out->port = (unsigned short)port;
            host_end = colon;
        }
    }
    if (!copy_part(out->host, sizeof(out->host), host, (size_t)(host_end - host))) return false;
    const char *path = uri ? end : end + 1;
    if (!copy_part(out->path, sizeof(out->path), path, strlen(path))) return false;
    if (out->host[0] == '-' || out->username[0] == '-' || out->path[0] == '-') return false;
    for (const char *p = out->host; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.' && *p != '_') return false;
    for (const char *p = out->username; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return false;
    /* This argument is shell-quoted in the remote exec request. Reject quote,
     * controls and URL escapes so the parsed and executed paths stay identical. */
    for (const char *p = out->path; *p; p++)
        if ((unsigned char)*p < 32 || *p == 127 || *p == '\'' || *p == '%' || *p == '\\') return false;
    return strcmp(out->path, "/") != 0;
}

typedef struct solar_os_git_ssh solar_os_git_ssh_t;
typedef struct {
    git_smart_subtransport_stream parent;
    solar_os_git_ssh_t *owner;
} solar_os_git_ssh_stream_t;

struct solar_os_git_ssh {
    git_smart_subtransport parent;
    const solar_os_native_ssh_exec_api_v1_t *api;
    solar_os_native_handle_t channel;
    solar_os_git_ssh_stream_t *stream;
    solar_os_git_ssh_t *retired_next;
};

/* Git operations are serialized. Retain a channel if cleanup ever has to be
 * retried after libgit2 frees its subtransport. */
static SOLAR_OS_GIT_STATE_BSS solar_os_git_ssh_t *retired_ssh;

static int ssh_close(git_smart_subtransport *base)
{
    solar_os_git_ssh_t *t = (solar_os_git_ssh_t *)base;
    if (t->channel) {
        const int result = t->api->close(t->channel);
        if (result && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return -1;
        t->channel = 0;
    }
    return 0;
}

static int ssh_read(git_smart_subtransport_stream *base, char *data, size_t len, size_t *read_len)
{
    solar_os_git_ssh_t *t = ((solar_os_git_ssh_stream_t *)base)->owner;
    *read_len = 0;
    if (!len) return 0;
    bool reported = false;
    const solar_os_native_ssh_exec_io_v1_t io = ssh_io(&reported);
    const int result = t->api->read(t->channel, data, len, read_len, &io);
    if (result) return ssh_failure(result, reported, "SSH Git read failed");
    if (!*read_len) {
        solar_os_native_ssh_exec_status_v1_t status;
        if (t->api->status(t->channel, &status) || !status.completed || status.exit_status || status.exited_by_signal) {
            return transport_error("remote Git command failed");
        }
    }
    return 0;
}

static int ssh_write(git_smart_subtransport_stream *base, const char *data, size_t len)
{
    solar_os_git_ssh_t *t = ((solar_os_git_ssh_stream_t *)base)->owner;
    bool reported = false;
    const solar_os_native_ssh_exec_io_v1_t io = ssh_io(&reported);
    size_t written;
    const int result = t->api->write(t->channel, data, len, &written, &io);
    return result ? ssh_failure(result, reported, "SSH Git write failed") : 0;
}

static void ssh_stream_free(git_smart_subtransport_stream *base)
{
    solar_os_git_ssh_stream_t *s = (solar_os_git_ssh_stream_t *)base;
    ssh_close(&s->owner->parent);
    s->owner->stream = NULL;
    free(s);
}

static int ssh_action(git_smart_subtransport_stream **out, git_smart_subtransport *base,
                       const char *url, git_smart_service_t action)
{
    solar_os_git_ssh_t *t = (solar_os_git_ssh_t *)base;
    if (t->stream) { *out = &t->stream->parent; return 0; }
    if (action != GIT_SERVICE_UPLOADPACK_LS && action != GIT_SERVICE_RECEIVEPACK_LS)
        return transport_error("invalid Git SSH action");
    solar_os_git_ssh_url_t parsed;
    if (!solar_os_git_parse_ssh_url(url, &parsed)) return transport_error("invalid SSH repository URL");
    if (ssh_close(base)) return transport_error("SSH channel cleanup is pending");
    int result = t->api->create(&t->channel);
    if (result) return ssh_failure(result, false, "no memory for SSH Git channel");
    char command[560];
    snprintf(command, sizeof(command), "%s '%s'", action == GIT_SERVICE_UPLOADPACK_LS ?
             "git-upload-pack" : "git-receive-pack", parsed.path);
    const solar_os_native_ssh_exec_options_v1_t options = {
        .host = parsed.host, .port = parsed.port, .username = parsed.username, .command = command,
    };
    bool reported = false;
    const solar_os_native_ssh_exec_io_v1_t io = ssh_io(&reported);
    result = t->api->start(t->channel, &options, &io);
    if (result) return ssh_failure(result, reported, "cannot start SSH Git command");
    t->stream = solar_os_memory_calloc(1, sizeof(*t->stream), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git.stream");
    if (!t->stream) return transport_error("no memory for SSH Git stream");
    t->stream->owner = t;
    t->stream->parent = (git_smart_subtransport_stream){
        .subtransport = base, .read = ssh_read, .write = ssh_write, .free = ssh_stream_free,
    };
    *out = &t->stream->parent;
    return 0;
}

static void ssh_free(git_smart_subtransport *base)
{
    solar_os_git_ssh_t *t = (solar_os_git_ssh_t *)base;
    if (t->stream) ssh_stream_free(&t->stream->parent);
    if (ssh_close(base)) {
        t->retired_next = retired_ssh;
        retired_ssh = t;
    } else {
        free(t);
    }
}

static const solar_os_native_ssh_exec_api_v1_t *ssh_service(void)
{
    return solar_os_native_ssh_exec_get_service(SOLAR_OS_NATIVE_SSH_EXEC_SERVICE,
        SOLAR_OS_NATIVE_SSH_EXEC_ABI, sizeof(solar_os_native_ssh_exec_api_v1_t));
}

static bool ssh_reap(void)
{
    while (retired_ssh) {
        solar_os_git_ssh_t *t = retired_ssh;
        if (ssh_close(&t->parent)) return false;
        retired_ssh = t->retired_next;
        free(t);
    }
    return true;
}

static int ssh_create(git_smart_subtransport **out, git_transport *owner, void *payload)
{
    (void)owner; (void)payload;
    const solar_os_native_ssh_exec_api_v1_t *api = ssh_service();
    if (!api) return transport_error("SSH command service unavailable");
    if (!ssh_reap()) return transport_error("SSH channel cleanup is pending");
    solar_os_git_ssh_t *t = solar_os_memory_calloc(1, sizeof(*t), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git.ssh");
    if (!t) return transport_error("no memory for SSH Git transport");
    t->api = api;
    t->parent = (git_smart_subtransport){ .action = ssh_action, .close = ssh_close, .free = ssh_free };
    *out = &t->parent;
    return 0;
}

typedef struct {
    git_stream parent;
    solar_os_tls_connection_t *tls;
    char host[256];
    uint16_t port;
} solar_os_git_tls_t;

static int tls_connect(git_stream *base)
{
    solar_os_git_tls_t *stream = (solar_os_git_tls_t *)base;
    if (stream->tls) return transport_error("HTTPS stream is already connected");
    const esp_err_t error = solar_os_tls_connect(stream->host, stream->port, GIT_IO_TIMEOUT_MS,
        stopped, NULL, &stream->tls);
    if (error == ESP_OK) return 0;
    if (error == ESP_ERR_NO_MEM) return transport_error("no memory for TLS");
    if (error == ESP_ERR_TIMEOUT) return transport_error("HTTPS connection timed out");
    if (stopped(NULL)) return transport_error("cancelled");
    return transport_error("HTTPS connection or certificate verification failed; check network and clock");
}

static ssize_t tls_io(git_stream *base, void *data, size_t length, bool write)
{
    solar_os_git_tls_t *stream = (solar_os_git_tls_t *)base;
    size_t transferred;
    const esp_err_t error = write ?
        solar_os_tls_write(stream->tls, data, length, GIT_IO_TIMEOUT_MS, stopped, NULL, &transferred) :
        solar_os_tls_read(stream->tls, data, length, GIT_IO_TIMEOUT_MS, stopped, NULL, &transferred);
    if (error == ESP_OK) return (ssize_t)transferred;
    if (error == ESP_ERR_TIMEOUT) return transport_error("HTTPS Git I/O timed out");
    return transport_error(stopped(NULL) ? "cancelled" : "HTTPS Git I/O failed");
}

static ssize_t tls_read(git_stream *s, void *data, size_t len) { return tls_io(s, data, len, false); }
static ssize_t tls_write(git_stream *s, const char *data, size_t len, int flags)
{ (void)flags; return tls_io(s, (void *)data, len, true); }
static int tls_close(git_stream *base)
{
    solar_os_git_tls_t *s = (solar_os_git_tls_t *)base;
    solar_os_tls_close(s->tls);
    s->tls = NULL;
    return 0;
}
static void tls_free(git_stream *s) { tls_close(s); free(s); }

static int tls_create(git_stream **out, const char *host, const char *port)
{
    solar_os_git_tls_t *s = solar_os_memory_calloc(1, sizeof(*s), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git.tls");
    if (!s) return transport_error("no memory for HTTPS Git stream");
    char *end;
    long parsed_port = strtol(port, &end, 10);
    if (strlen(host) >= sizeof(s->host) || *end || parsed_port < 1 || parsed_port > 65535) {
        free(s); return transport_error("invalid HTTPS host or port");
    }
    strcpy(s->host, host); s->port = (uint16_t)parsed_port;
    s->parent = (git_stream){ .version = GIT_STREAM_VERSION, .encrypted = 1,
        .connect = tls_connect, .read = tls_read, .write = tls_write,
        .close = tls_close, .free = tls_free };
    *out = &s->parent;
    return 0;
}

int solar_os_git_transport_factory(git_transport **out, git_remote *owner, void *payload)
{
    (void)payload;
    const char *url = git_remote_url(owner);
    const char *push_url = git_remote_pushurl(owner);
    /* libgit2 calls the transport factory before passing its actual URL.
     * Both URLs must use an allowed transport; SSH is selected by action(). */
    if (!url) return transport_error("remote URL missing");
    if (push_url && strcmp(push_url, url)) return transport_error("separate push URLs are unsupported; use git remote set-url");
    solar_os_git_ssh_url_t parsed;
    if (!strncmp(url, "https://", 8)) return git_transport_new(out, owner, url);
    if (!solar_os_git_parse_ssh_url(url, &parsed)) return transport_error("use SSH or HTTPS repository URLs");
    git_smart_subtransport_definition def = { .callback = ssh_create, .rpc = 0 };
    return git_transport_smart(out, owner, &def);
}

int solar_os_git_transport_init(const solar_os_git_options_t *options)
{
    active_options = options;
    git_stream_registration tls = { .version = GIT_STREAM_VERSION, .init = tls_create };
    return git_stream_register(GIT_STREAM_TLS, &tls);
}

void solar_os_git_transport_shutdown(void)
{
    git_stream_register(GIT_STREAM_TLS, NULL);
    (void)ssh_reap();
    active_options = NULL;
}
