#include <assert.h>
#include <stdio.h>

#include "solar_os_shell_completion.h"

int main(void)
{
    assert(solar_os_shell_completion_common_prefix("", "radio") == 0U);
    assert(solar_os_shell_completion_common_prefix("radio", "radio") == 5U);
    assert(solar_os_shell_completion_common_prefix("radio", "ramfs") == 2U);
    assert(solar_os_shell_completion_common_prefix("mount", "unmount") == 0U);
    char aggregate[] = "radio";
    aggregate[solar_os_shell_completion_common_prefix(aggregate, "radar")] = '\0';
    aggregate[solar_os_shell_completion_common_prefix(aggregate, "radius")] = '\0';
    assert(aggregate[0] == 'r' && aggregate[1] == 'a' &&
           aggregate[2] == 'd' && aggregate[3] == '\0');
    assert(solar_os_shell_completion_common_prefix(NULL, "value") == 0U);
    assert(solar_os_shell_completion_common_prefix("value", NULL) == 0U);
    puts("shell completion tests: ok");
    return 0;
}
