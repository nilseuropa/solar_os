#include "solar_os_ps2_keyboard_job.h"

#include <stdbool.h>
#include <string.h>

#include "solar_os_expansion.h"

#define PS2_KEYBOARD_JOB_DEVICE "ps2-keyboard-job"

static bool attached;

static esp_err_t ps2_keyboard_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc != 2 || argv == NULL || argv[1] == NULL || attached) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_expansion_binding_t binding = {
        .kind = SOLAR_OS_EXPANSION_BINDING_PS2_BUS,
    };
    strlcpy(binding.target, argv[1], sizeof(binding.target));
    const esp_err_t err = solar_os_expansion_attach("ps2-keyboard",
                                                    PS2_KEYBOARD_JOB_DEVICE,
                                                    &binding,
                                                    1);
    if (err == ESP_OK) {
        attached = true;
    }
    return err;
}

static void ps2_keyboard_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (attached) {
        (void)solar_os_expansion_detach(PS2_KEYBOARD_JOB_DEVICE);
        attached = false;
    }
}

const solar_os_job_t solar_os_ps2_keyboard_job = {
    .name = "ps2-keyboard",
    .summary = "attach a PS/2 keyboard expansion",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = ps2_keyboard_start,
    .stop = ps2_keyboard_stop,
};
