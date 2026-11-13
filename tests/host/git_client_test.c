#include "solar_os_git.h"
#include "solar_os_git_transport.h"
#include "solar_os_memory.h"
#include "git2/sys/transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

/* Host tests exercise the production Git engine and Git's pack protocol with
 * a local git-daemon. Device SSH/TLS adapters are tested separately on HIL. */
int solar_os_git_transport_init(const solar_os_git_options_t *o) { (void)o; return 0; }
void solar_os_git_transport_shutdown(void) {}
int solar_os_git_transport_factory(git_transport **out, git_remote *remote, void *p)
{ (void)p; return git_transport_new(out, remote, git_remote_url(remote)); }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(n, size); }
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return malloc(size); }
void *solar_os_memory_realloc(void *p, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return realloc(p, size); }
void solar_os_memory_free(void *p) { free(p); }
static unsigned workspace_count;
void *solar_os_git_workspace_alloc(size_t size)
{
    if (getenv("SOLAR_OS_GIT_TEST_WORKSPACE_OOM")) return NULL;
    void *p = malloc(size);
    if (p) workspace_count++;
    return p;
}
void solar_os_git_workspace_release(uint32_t **workspace)
{
    if (*workspace) { assert(workspace_count); workspace_count--; free(*workspace); }
}
static void emit(void *p, const char *data, size_t size) { (void)p; fwrite(data, 1, size, stdout); }
static bool stop(void *p) { (void)p; return getenv("SOLAR_OS_GIT_TEST_CANCEL") != NULL; }
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    char error[512];
    solar_os_git_options_t options = { .output = emit, .cancelled = stop,
        .error = error, .error_size = sizeof(error) };
    if (!strcmp(argv[2], "__test_lifecycle")) {
        char *status[] = { "status" };
        for (int i = 0; i < 100; i++) {
            int rc = solar_os_git_run(argv[1], 1, status, &options);
            if (rc) { fprintf(stderr, "%s\n", error); return 1; }
            assert(workspace_count == 0);
        }
        return 0;
    }
    if (!strcmp(argv[2], "__test_regex")) {
        git_config *config = NULL;
        git_config_iterator *iter = NULL;
        assert(git_libgit2_init() > 0);
        assert(git_config_new(&config) == 0);
        int rc = git_config_iterator_glob_new(&iter, config, "^remote\\..*$");
        if (rc) fprintf(stderr, "%s\n", git_error_last()->message);
        git_config_iterator_free(iter);
        git_config_free(config);
        assert(workspace_count == 0);
        git_libgit2_shutdown();
        return rc ? 1 : 0;
    }
    int rc = solar_os_git_run(argv[1], argc - 2, argv + 2, &options);
    assert(workspace_count == 0);
    if (rc) fprintf(stderr, "%s\n", error);
    return rc ? 1 : 0;
}
