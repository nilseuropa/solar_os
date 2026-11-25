#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_components.h"
#include <stdatomic.h>

static atomic_bool cc_running;

void solar_os_shell_cmd_cc(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (atomic_exchange(&cc_running, true)) {
        solar_os_shell_diag_esp(io, "cc", ESP_ERR_INVALID_STATE,
                                "another C compiler command is running", NULL);
        return;
    }
    const solar_os_component_t *component = solar_os_component_find(SOLAR_OS_MODULE_TYPE_APP, "cc");
    if (component && component->entry)
        (void)solar_os_shell_run_native_builtin(ctx, component->entry, argc, argv);
    else
        solar_os_shell_diag_esp(io, "cc", ESP_ERR_NOT_FOUND, "compiled component is unavailable", NULL);
    atomic_store(&cc_running, false);
}
