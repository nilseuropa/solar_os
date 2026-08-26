#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t solar_os_shell_completion_common_prefix(const char *first,
                                               const char *second);

#ifdef __cplusplus
}
#endif
