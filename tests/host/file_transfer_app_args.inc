#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_ftp.h"
#include "solar_os_sftp.h"

#define SOLAR_OS_PACKAGE_APP_FTP 1
#define SOLAR_OS_PACKAGE_APP_SFTP 1
#define SOLAR_OS_NET_HOST_MAX 128U
#define FTP_APP_INPUT_MAX 80U
#define FTP_APP_MESSAGE_MAX 128U

typedef int solar_os_tui_t;
typedef int ftp_app_pane_t;
typedef int ftp_app_input_t;
typedef int solar_os_text_search_state_t;
typedef struct { char password[64]; } ftp_app_connection_form_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef int ftp_app_request_t;
typedef int ftp_app_progress_t;
typedef struct { void **state_slot; } solar_os_app_t;
typedef struct { int argc; const char *const *argv; } solar_os_context_t;

static const solar_os_app_t solar_os_sftp_app;
static const solar_os_app_t *active_app;

static const solar_os_app_t *solar_os_application_descriptor(void)
{
    return active_app;
}

static int solar_os_context_argc(const solar_os_context_t *ctx) { return ctx->argc; }
static const char *solar_os_context_argv(const solar_os_context_t *ctx, int index) { return ctx->argv[index]; }

static size_t strlcpy(char *out, const char *value, size_t capacity)
{
    snprintf(out, capacity, "%s", value);
    return strlen(value);
}

static void solar_os_identity_get_user(char *out, size_t capacity)
{
    strlcpy(out, "local-user", capacity);
}

/* STATE */
/* PARSER */

static const solar_os_app_t solar_os_ftp_app = {.state_slot = &ftp_app_state};
static const solar_os_app_t solar_os_sftp_app = {.state_slot = &sftp_app_state};

int main(void)
{
    ftp_app_state_t ftp_state = {0}, sftp_state = {0};
    ftp_app_state = &ftp_state;
    sftp_app_state = &sftp_state;
    char local[SOLAR_OS_STORAGE_PATH_MAX], remote[SOLAR_OS_STORAGE_PATH_MAX];
    bool connect;

    const char *const ftp_arguments[] = {
        "ftp", "ftp.example", "2121", "--user", "guest", "--password", "password", "--remote", "/shared",
    };
    solar_os_context_t ctx = {.argc = 9, .argv = ftp_arguments};
    active_app = &solar_os_ftp_app;
    ftp_app.transport = ftp_app_select_transport();
    assert(ftp_app.transport->protocol == SOLAR_OS_REMOTE_FILE_FTP);
    assert(ftp_app_parse_args(&ctx, local, remote, &connect) == ESP_OK);
    assert(connect && !strcmp(remote, "/shared") && !strcmp(local, "."));
    assert(ftp_app.port == 2121 && !strcmp(ftp_app.username, "guest"));

    const char *const sftp_arguments[] = {"sftp", "operator@sftp.example:/archive", "--local", "/sd"};
    ctx = (solar_os_context_t){.argc = 4, .argv = sftp_arguments};
    active_app = &solar_os_sftp_app;
    ftp_app.transport = ftp_app_select_transport();
    assert(ftp_app.transport->protocol == SOLAR_OS_REMOTE_FILE_SFTP);
    assert(ftp_app_parse_args(&ctx, local, remote, &connect) == ESP_OK);
    assert(connect && !strcmp(remote, "/archive") && !strcmp(local, "/sd"));
    assert(ftp_app.port == 22 && !strcmp(ftp_app.username, "operator") && !ftp_app.password[0]);

    assert(!strcmp(ftp_state.host, "ftp.example") && !strcmp(ftp_state.password, "password"));
    assert(!strcmp(sftp_state.host, "sftp.example"));
    active_app = &solar_os_ftp_app;
    assert(ftp_app_current() == &ftp_state);
    assert(ftp_app.transport->protocol == SOLAR_OS_REMOTE_FILE_FTP && ftp_app.port == 2121);
    active_app = NULL;
    assert(!ftp_app_current());
}
