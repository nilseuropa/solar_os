#pragma once
#include "solar_os.h"

extern const solar_os_job_t solar_os_screen_keyboard_job;
/* Runtime owner only; called before a higher-priority system overlay or sleep. */
void solar_os_screen_keyboard_hide(void);
void solar_os_shell_cmd_screen_keyboard(solar_os_context_t *ctx, int argc, char **argv);
