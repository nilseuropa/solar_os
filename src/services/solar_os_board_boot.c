#include "solar_os_board_boot.h"

#include "solar_os_board.h"
#include "solar_os_config.h"
#include "solar_os_jobs.h"

#include "esp_log.h"

static const char *TAG = "board_boot";

esp_err_t solar_os_board_boot_start_jobs(solar_os_context_t *ctx)
{
#if SOLAR_OS_BOARD_HAS_PS2_KEYBOARD
#if SOLAR_OS_PACKAGE_JOB_PS2_KEYBOARD
    char job_arg[] = "ps2-keyboard";
    char bus_arg[] = SOLAR_OS_BOARD_AUTOSTART_PS2_BUS;
    char *argv[] = {job_arg, bus_arg};
    const esp_err_t err = solar_os_jobs_start(ctx, job_arg, 2, argv);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "autostarted %s on %s", job_arg, bus_arg);
    }
    return err;
#else
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
#endif
#else
    (void)ctx;
    return ESP_OK;
#endif
}
