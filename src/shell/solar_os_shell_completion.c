#include "solar_os_shell_completion.h"

size_t solar_os_shell_completion_common_prefix(const char *first,
                                               const char *second)
{
    if (first == NULL || second == NULL) {
        return 0U;
    }

    size_t length = 0U;
    while (first[length] != '\0' && first[length] == second[length]) {
        length++;
    }
    return length;
}
