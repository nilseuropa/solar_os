#include "solar_os_board_boot.h"
#include "solar_os_board.h"
#include "solar_os_config.h"
#include "solar_os_jobs.h"

esp_err_t solar_os_board_boot_start_jobs(solar_os_context_t *ctx)
{
    (void)ctx;
#if SOLAR_OS_PACKAGE_JOB_POINTER_ARROWS && defined(SOLAR_OS_BOARD_POINTER_ARROWS_SOURCE)
    char job[] = "pointer-arrows";
    char source[] = "source=" SOLAR_OS_BOARD_POINTER_ARROWS_SOURCE;
    char *args[] = {job, source};
    return solar_os_jobs_start(ctx, job, 2, args);
#else
    return ESP_OK;
#endif
}
