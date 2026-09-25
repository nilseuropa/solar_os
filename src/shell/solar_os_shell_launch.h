#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Returns the argument index that names a local filesystem path for an app
 * launch, or -1 when the command has no path argument to resolve.  SCP has
 * two independently local-or-remote operands and is handled by the shell.
 */
int solar_os_shell_launch_path_arg(const char *app_name,
                                   int argc,
                                   char *const argv[]);

bool solar_os_shell_launch_raw_remainder(
    const char *line,
    int prefix_argc,
    const char *const prefix_argv[],
    char *buffer,
    size_t buffer_len);

bool solar_os_shell_path_is_script(const char *path);
