#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include <stdatomic.h>

extern int solar_os_cc_app_main(int, char **);
static atomic_bool cc_running;

void solar_os_shell_cmd_cc(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (atomic_exchange(&cc_running, true)) {
        solar_os_shell_diag_esp(io, "cc", ESP_ERR_INVALID_STATE,
                                "another C compiler command is running", NULL);
        return;
    }
    (void)solar_os_shell_run_native_builtin(ctx, solar_os_cc_app_main, argc, argv);
    atomic_store(&cc_running, false);
}
