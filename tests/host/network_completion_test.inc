#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SHELL_ARG_MAX 16
#define SHELL_INPUT_MAX 192
#define SHELL_NOINLINE
#define SOLAR_OS_PACKAGE_APP_SSH SOLAR_OS_PACKAGE_SERVICE_SSH
#define SOLAR_OS_PACKAGE_APP_SCP SOLAR_OS_PACKAGE_SERVICE_SSH
#define SOLAR_OS_PACKAGE_APP_SFTP SOLAR_OS_PACKAGE_SERVICE_SSH
#define SOLAR_OS_PACKAGE_APP_SFTPSYNC SOLAR_OS_PACKAGE_SERVICE_SSH
#define SOLAR_OS_PACKAGE_APP_FTP 1
#define SOLAR_OS_PACKAGE_APP_TELNET 1
typedef int esp_err_t;
#define ESP_OK 0
typedef struct {
    char input[SHELL_INPUT_MAX];
    bool history_browsing;
    int history_index;
} solar_os_context_t;
typedef struct {
    const char *prefix;
    char match[SHELL_INPUT_MAX];
    unsigned count;
    bool print;
} shell_completion_match_t;
static size_t strlcpy(char *dst, const char *src, size_t len)
{
    size_t size = strlen(src);
    if (len) snprintf(dst, len, "%s", src);
    return size;
}
static solar_os_context_t *shell_session(solar_os_context_t *ctx) { return ctx; }
static void *shell_io(solar_os_context_t *ctx) { return ctx; }
static void solar_os_shell_io_newline(void *io) { (void)io; }
static void shell_prompt(solar_os_context_t *ctx) { (void)ctx; }
static void shell_replace_input(solar_os_context_t *ctx, const char *input)
{ strlcpy(ctx->input, input, sizeof(ctx->input)); }
static void shell_completion_init_state(solar_os_context_t *ctx, const char *prefix,
                                         bool print, shell_completion_match_t *matches)
{
    (void)ctx;
    *matches = (shell_completion_match_t){.prefix = prefix, .print = print};
}
static unsigned printed;
static void shell_completion_emit(shell_completion_match_t *matches, const char *candidate)
{
    if (strncmp(candidate, matches->prefix, strlen(matches->prefix))) return;
    if (matches->print) printed++;
    if (!matches->count++) {
        strlcpy(matches->match, candidate, sizeof(matches->match));
    } else {
        size_t i = 0;
        while (matches->match[i] && matches->match[i] == candidate[i]) i++;
        matches->match[i] = 0;
    }
}
static int visit_hosts(bool (*visitor)(const char *, const char *, void *), void *user)
{
    const char *aliases[] = {"remote", "relay", "server"};
    for (unsigned i = 0; i < 3; i++) {
        if (!visitor("192.0.2.1", aliases[i], user)) break;
    }
    return ESP_OK;
}
#if SOLAR_OS_PACKAGE_SERVICE_SSH
#define solar_os_ssh_hosts_visit visit_hosts
#else
#define solar_os_net_hosts_visit visit_hosts
#endif

/* COMPLETION_SOURCE */

static void complete(const char *command, const char *prefix, const char *expected)
{
    solar_os_context_t ctx = {0};
    snprintf(ctx.input, sizeof(ctx.input), "%s %s", command, prefix);
    const char *tokens[] = {command};
    assert(shell_complete_ssh_host_argument(&ctx, command, tokens, 1, prefix,
                                            strlen(command) + 1, false));
    assert(!strcmp(ctx.input, expected));
}
int main(void)
{
    complete("ftp", "ser", "ftp server ");
    complete("telnet", "ser", "telnet server ");
    complete("ftp", "r", "ftp re");
#if SOLAR_OS_PACKAGE_SERVICE_SSH
    complete("ssh", "user@ser", "ssh user@server ");
    complete("sftp", "user@ser", "sftp user@server ");
    complete("scp", "user@ser", "scp user@server:");
    complete("sftpsync", "user@ser", "sftpsync user@server ");
#endif
    solar_os_context_t ctx = {.input = "telnet -r ser"};
    const char *raw[] = {"telnet", "-r"};
    assert(shell_complete_ssh_host_argument(&ctx, "telnet", raw, 2, "ser", 10, false));
    assert(!strcmp(ctx.input, "telnet -r server "));
    const char *later[] = {"ftp", "server"};
    assert(!shell_complete_ssh_host_argument(&ctx, "ftp", later, 2, "ser", 0, false));
    const char *tokens[] = {"ftp"};
    const char *invalid[] = {"--user", "server:/path", "/local", ".local", "user@ser", "unknown"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        assert(!shell_complete_ssh_host_argument(&ctx, "ftp", tokens, 1, invalid[i], 0, false));
    }
    strcpy(ctx.input, "ftp re");
    assert(shell_complete_ssh_host_argument(&ctx, "ftp", tokens, 1, "re", 4, true));
    assert(printed == 2);
    assert(!strcmp(ctx.input, "ftp re"));
    return 0;
}
