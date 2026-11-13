#include "git2_util.h"
#include "alloc.h"
#include "process.h"
#include "str.h"
#include "util.h"
#include "esp_random.h"
#include "git2/config.h"
#include <pwd.h>
#include <utime.h>

#undef rename

void *solar_os_git_workspace_alloc(size_t size) { return git__malloc(size); }
void solar_os_git_workspace_release(uint32_t **workspace) { git__free(*workspace); }

int solar_os_git_link(const char *from, const char *to)
{
    (void)from; (void)to;
    /* ESP-IDF emulates FAT link() by copying. Git needs real hard-link
     * semantics; let it use the safe rename/copy fallback instead. */
    errno = ENOTSUP;
    return -1;
}

int solar_os_git_parse_int(int *out, const char *value)
{
    int32_t parsed;
    int rc = git_config_parse_int32(&parsed, value);
    if (!rc) *out = (int)parsed;
    return rc;
}

int solar_os_git_config_int(int *out, const git_config *config, const char *key)
{
    int32_t parsed;
    int rc = git_config_get_int32(&parsed, config, key);
    if (!rc) *out = (int)parsed;
    return rc;
}

int solar_os_git_strntoint(int *out, const char *str, size_t len, const char **end, int base)
{
    int32_t parsed;
    int rc = git__strntol32(&parsed, str, len, end, base);
    if (!rc) *out = (int)parsed;
    return rc;
}

const char *solar_os_git_gai_strerror(int code)
{
    (void)code;
    return "DNS resolution failed";
}

ssize_t solar_os_git_readlink(const char *path, char *out, size_t size)
{
    (void)path; (void)out; (void)size;
    errno = ENOTSUP;
    return -1;
}

int solar_os_git_symlink(const char *target, const char *path)
{
    (void)target; (void)path;
    errno = ENOTSUP;
    return -1;
}

int solar_os_git_utimes(const char *path, const struct timeval times[2])
{
    if (!times) return utime(path, NULL);
    struct utimbuf when = { .actime = times[0].tv_sec, .modtime = times[1].tv_sec };
    return utime(path, &when);
}

int solar_os_git_chmod(const char *path, mode_t mode)
{
    struct stat st;
    (void)mode;
    return stat(path, &st);
}

/* FatFs cannot replace an existing destination with rename(). Keep the old
 * value until the new value is installed, and restore it on ordinary errors.
 * A leftover backup after power loss is deliberately never overwritten. */
int solar_os_git_rename(const char *from, const char *to)
{
    struct stat st;
    if (stat(to, &st) != 0) {
        if (errno != ENOENT) return -1;
        return rename(from, to);
    }
    if (strcmp(from, to) == 0) return 0;
    if (S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }
    size_t size = strlen(to) + sizeof(".solaros-backup");
    char *backup = malloc(size);
    if (!backup) { errno = ENOMEM; return -1; }
    snprintf(backup, size, "%s.solaros-backup", to);
    int result = -1;
    if (stat(backup, &st) == 0) { errno = EEXIST; goto out; }
    if (errno != ENOENT || rename(to, backup) != 0) goto out;
    if (rename(from, to) != 0) {
        int saved = errno;
        (void)rename(backup, to);
        errno = saved;
        goto out;
    }
    result = unlink(backup);
out:
    free(backup);
    return result;
}

int solar_os_git_futimes(int fd, const struct timeval times[2])
{
    (void)fd; (void)times;
    /* File times are hints; Git validates content when the stat cache differs. */
    return 0;
}

int solar_os_git_getentropy(void *data, size_t len)
{
    esp_fill_random(data, len);
    return 0;
}

int solar_os_git_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
                          size_t size, struct passwd **result)
{
    (void)uid; (void)buf; (void)size;
    memset(pwd, 0, sizeof(*pwd));
    pwd->pw_dir = "/";
    pwd->pw_name = "solaros";
    *result = pwd;
    return 0;
}

/* ESP VFS has no symlinks. Canonicalize paths without a process-wide cwd. */
char *p_realpath(const char *path, char *resolved)
{
    char *out = resolved ? resolved : git__malloc(PATH_MAX);
    if (!out) return NULL;
    if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) {
        errno = EINVAL;
        goto fail;
    }
    size_t used = 1;
    out[0] = '/';
    while (*path) {
        while (*path == '/') path++;
        const char *part = path;
        while (*path && *path != '/') path++;
        size_t len = (size_t)(path - part);
        if (!len || (len == 1 && part[0] == '.')) continue;
        if (len == 2 && part[0] == '.' && part[1] == '.') {
            while (used > 1 && out[used - 1] != '/') used--;
            if (used > 1) used--;
            continue;
        }
        if (used > 1) out[used++] = '/';
        memcpy(out + used, part, len);
        used += len;
    }
    out[used] = '\0';
    struct stat st;
    if (stat(out, &st) == 0) return out;
fail:
    if (!resolved) git__free(out);
    return NULL;
}

/* Git attributes may name host executables. Never run them on the device. */
static int process_unsupported(void)
{
    git_error_set(GIT_ERROR_OS, "external Git filters are unavailable on SolarOS");
    return -1;
}
int git_process_new(git_process **out, const char **args, size_t argc,
    const char **env, size_t envc, git_process_options *opts)
{ (void)out; (void)args; (void)argc; (void)env; (void)envc; (void)opts; return process_unsupported(); }
int git_process_new_from_cmdline(git_process **out, const char *cmd,
    const char **env, size_t envc, git_process_options *opts)
{ (void)out; (void)cmd; (void)env; (void)envc; (void)opts; return process_unsupported(); }
int git_process_start(git_process *p) { (void)p; return process_unsupported(); }
int git_process_id(p_pid_t *id, git_process *p) { (void)id; (void)p; return process_unsupported(); }
ssize_t git_process_read(git_process *p, void *b, size_t n) { (void)p; (void)b; (void)n; return process_unsupported(); }
ssize_t git_process_read_err(git_process *p, void *b, size_t n) { return git_process_read(p, b, n); }
ssize_t git_process_write(git_process *p, const void *b, size_t n) { (void)p; (void)b; (void)n; return process_unsupported(); }
int git_process_wait(git_process_result *r, git_process *p) { (void)r; (void)p; return process_unsupported(); }
int git_process_close_in(git_process *p) { (void)p; return 0; }
int git_process_close_out(git_process *p) { (void)p; return 0; }
int git_process_close_err(git_process *p) { (void)p; return 0; }
int git_process_close(git_process *p) { (void)p; return 0; }
int git_process_result_msg(git_str *out, git_process_result *r) { (void)r; return git_str_sets(out, "external filters unavailable"); }
void git_process_free(git_process *p) { (void)p; }
