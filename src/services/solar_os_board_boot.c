#include "solar_os_board_boot.h"
#include "solar_os_board.h"
#include "solar_os_config.h"
#include "solar_os_jobs.h"

#ifdef SOLAR_OS_BOARD_BOOT_OUTPUTS
#include "driver/gpio.h"
#endif

esp_err_t solar_os_board_boot_init(void)
{
#ifdef SOLAR_OS_BOARD_BOOT_OUTPUTS
    static const struct {
        gpio_num_t pin;
        int level;
    } outputs[] = SOLAR_OS_BOARD_BOOT_OUTPUTS;

    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        esp_err_t err = gpio_hold_dis(outputs[i].pin);
        if (err != ESP_OK) return err;
        err = gpio_reset_pin(outputs[i].pin);
        if (err != ESP_OK) return err;
        err = gpio_sleep_sel_dis(outputs[i].pin);
        if (err != ESP_OK) return err;
        /* Load the output latch before enabling the driver to avoid a low pulse. */
        err = gpio_set_level(outputs[i].pin, outputs[i].level);
        if (err != ESP_OK) return err;
        err = gpio_set_direction(outputs[i].pin, GPIO_MODE_OUTPUT);
        if (err != ESP_OK) return err;
    }
#endif
    return ESP_OK;
}

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
