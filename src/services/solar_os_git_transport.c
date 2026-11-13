#include "solar_os_git_transport.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "git2/sys/stream.h"
#include "git2/sys/errors.h"
#include "git2/sys/transport.h"
#include "solar_os_ssh_transport.h"
#include "solar_os_memory.h"

#define GIT_IO_TIMEOUT_US (30LL * 1000000LL)

/* libgit2 is serialized by solar_os_git_run; stream constructors have no
 * payload slot. The pointer is valid only for the duration of that call. */
static const solar_os_git_options_t *active_options;

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

static void ssh_error(void *unused, const char *message)
{
    (void)unused;
    git_error_set_str(GIT_ERROR_SSH, message);
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
    solar_os_ssh_transport_t ssh;
    solar_os_ssh_transport_config_t config;
    LIBSSH2_CHANNEL *channel;
    solar_os_git_ssh_stream_t *stream;
    solar_os_git_ssh_url_t url;
    int64_t open_deadline;
};

static bool ssh_stopped(void *user)
{
    solar_os_git_ssh_t *t = user;
    return stopped(NULL) || (t->open_deadline && esp_timer_get_time() >= t->open_deadline);
}

static bool ssh_wait(solar_os_git_ssh_t *t, int64_t deadline)
{
    if (stopped(NULL)) { transport_error("cancelled"); return false; }
    if (esp_timer_get_time() >= deadline) { transport_error("SSH Git I/O timed out"); return false; }
    return solar_os_ssh_transport_wait_socket(&t->config, t->ssh.socket_fd, t->ssh.session) >= 0;
}

static int ssh_close(git_smart_subtransport *base)
{
    solar_os_git_ssh_t *t = (solar_os_git_ssh_t *)base;
    if (t->channel) {
        int64_t deadline = esp_timer_get_time() + 1000000;
        int rc;
        do {
            rc = libssh2_channel_free(t->channel);
        } while (rc == LIBSSH2_ERROR_EAGAIN && ssh_wait(t, deadline));
        t->channel = NULL;
    }
    solar_os_ssh_transport_close(&t->ssh, "Git finished");
    return 0;
}

static int ssh_read(git_smart_subtransport_stream *base, char *data, size_t len, size_t *read_len)
{
    solar_os_git_ssh_t *t = ((solar_os_git_ssh_stream_t *)base)->owner;
    int64_t deadline = esp_timer_get_time() + GIT_IO_TIMEOUT_US;
    *read_len = 0;
    while (!stopped(NULL)) {
        ssize_t n = libssh2_channel_read(t->channel, data, len);
        if (n > 0) { *read_len = (size_t)n; return 0; }
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) return transport_error("SSH Git read failed");
        char error[256];
        ssize_t error_len = libssh2_channel_read_stderr(t->channel, error, sizeof(error) - 1);
        if (error_len > 0) {
            error[error_len] = 0;
            if (active_options && active_options->output)
                active_options->output(active_options->user, error, (size_t)error_len);
        }
        if (libssh2_channel_eof(t->channel)) {
            if (libssh2_channel_get_exit_status(t->channel)) return transport_error("remote Git command failed");
            return 0;
        }
        if (!ssh_wait(t, deadline)) return -1;
    }
    return transport_error("cancelled");
}

static int ssh_write(git_smart_subtransport_stream *base, const char *data, size_t len)
{
    solar_os_git_ssh_t *t = ((solar_os_git_ssh_stream_t *)base)->owner;
    int64_t deadline = esp_timer_get_time() + GIT_IO_TIMEOUT_US;
    while (len && !stopped(NULL)) {
        ssize_t n = libssh2_channel_write(t->channel, data, len);
        if (n > 0) { data += n; len -= (size_t)n; deadline = esp_timer_get_time() + GIT_IO_TIMEOUT_US; }
        else if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) return transport_error("SSH Git write failed");
        else if (!ssh_wait(t, deadline)) return -1;
    }
    return len ? transport_error("cancelled") : 0;
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
    if (!solar_os_git_parse_ssh_url(url, &t->url)) return transport_error("invalid SSH repository URL");
    t->config = (solar_os_ssh_transport_config_t){
        .host = t->url.host, .port = t->url.port, .username = t->url.username,
        .password = "", .user = t, .should_stop = ssh_stopped, .error = ssh_error,
        .allow_unverified_host_key_without_storage = false, .include_error_code = true,
    };
    t->open_deadline = esp_timer_get_time() + GIT_IO_TIMEOUT_US;
    if (solar_os_ssh_transport_open(&t->config, &t->ssh) != ESP_OK) return -1;
    t->open_deadline = 0;
    int64_t deadline = esp_timer_get_time() + GIT_IO_TIMEOUT_US;
    do {
        t->channel = libssh2_channel_open_session(t->ssh.session);
        if (t->channel) break;
        if (libssh2_session_last_errno(t->ssh.session) != LIBSSH2_ERROR_EAGAIN || !ssh_wait(t, deadline))
            return transport_error("cannot open SSH Git channel");
    } while (true);
    char command[560];
    snprintf(command, sizeof(command), "%s '%s'", action == GIT_SERVICE_UPLOADPACK_LS ?
             "git-upload-pack" : "git-receive-pack", t->url.path);
    int rc;
    do {
        rc = libssh2_channel_exec(t->channel, command);
        if (rc == LIBSSH2_ERROR_EAGAIN && !ssh_wait(t, deadline)) return -1;
    } while (rc == LIBSSH2_ERROR_EAGAIN);
    if (rc) return transport_error("remote Git command rejected");
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
    ssh_close(base);
    free(t);
}

static int ssh_create(git_smart_subtransport **out, git_transport *owner, void *payload)
{
    (void)owner; (void)payload;
    solar_os_git_ssh_t *t = solar_os_memory_calloc(1, sizeof(*t), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "git.ssh");
    if (!t) return transport_error("no memory for SSH Git transport");
    t->ssh.socket_fd = -1;
    t->parent = (git_smart_subtransport){ .action = ssh_action, .close = ssh_close, .free = ssh_free };
    *out = &t->parent;
    return 0;
}

typedef struct {
    git_stream parent;
    esp_tls_t *tls;
    char host[256];
    int port;
} solar_os_git_tls_t;

static int tls_connect(git_stream *base)
{
    solar_os_git_tls_t *s = (solar_os_git_tls_t *)base;
    esp_tls_cfg_t cfg = { .crt_bundle_attach = esp_crt_bundle_attach,
        .non_block = true, .timeout_ms = 1000 };
    s->tls = esp_tls_init();
    if (!s->tls) return transport_error("no memory for TLS");
    int64_t deadline = esp_timer_get_time() + GIT_IO_TIMEOUT_US;
    while (!stopped(NULL) && esp_timer_get_time() < deadline) {
        int rc = esp_tls_conn_new_async(s->host, (int)strlen(s->host), s->port, &cfg, s->tls);
        if (rc == 1) return 0;
        if (rc < 0) return transport_error("HTTPS connection or certificate verification failed; check network and clock");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return transport_error(stopped(NULL) ? "cancelled" : "HTTPS connection timed out");
}

static ssize_t tls_io(git_stream *base, void *data, size_t len, bool write)
{
    solar_os_git_tls_t *s = (solar_os_git_tls_t *)base;
    int64_t deadline = esp_timer_get_time() + GIT_IO_TIMEOUT_US;
    while (!stopped(NULL) && esp_timer_get_time() < deadline) {
        ssize_t n = write ? esp_tls_conn_write(s->tls, data, len) : esp_tls_conn_read(s->tls, data, len);
        if (n >= 0) return n;
        if (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE)
            return transport_error("HTTPS Git I/O failed");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return transport_error(stopped(NULL) ? "cancelled" : "HTTPS Git I/O timed out");
}

static ssize_t tls_read(git_stream *s, void *data, size_t len) { return tls_io(s, data, len, false); }
static ssize_t tls_write(git_stream *s, const char *data, size_t len, int flags)
{ (void)flags; return tls_io(s, (void *)data, len, true); }
static int tls_close(git_stream *base)
{
    solar_os_git_tls_t *s = (solar_os_git_tls_t *)base;
    if (s->tls) esp_tls_conn_destroy(s->tls);
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
    strcpy(s->host, host); s->port = (int)parsed_port;
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
    active_options = NULL;
}
