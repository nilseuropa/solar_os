#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    void *user;
    void (*output)(void *user, const char *data, size_t size);
    bool (*cancelled)(void *user);
    char *error;
    size_t error_size;
} solar_os_git_options_t;

/* Synchronous, serialized Git operations. cwd is an absolute VFS path.
 * argv begins with the subcommand. Call from a foreground worker with at
 * least 28 KiB of stack. Callbacks run on that worker. No process cwd changes. */
int solar_os_git_run(const char *cwd, int argc, char **argv,
                    const solar_os_git_options_t *options);
