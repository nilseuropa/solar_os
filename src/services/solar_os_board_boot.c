#include "solar_os_board_boot.h"
#include "solar_os_board.h"
#include "solar_os_config.h"
#include "solar_os_jobs.h"

#ifdef SOLAR_OS_BOARD_DEFAULT_PORT_SHELL
#include "esp_log.h"
#include "solar_os_port_shell.h"
#endif

#if defined(SOLAR_OS_BOARD_BOOT_OUTPUTS) || defined(SOLAR_OS_BOARD_POWER_LATCH_PINS)
#include "driver/gpio.h"
#endif

#ifdef SOLAR_OS_BOARD_POWER_LATCH_PINS
#include "solar_os_power.h"

static gpio_num_t power_latch_pins[] = SOLAR_OS_BOARD_POWER_LATCH_PINS;

static esp_err_t board_power_off(void *user)
{
    if (user != power_latch_pins) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < sizeof(power_latch_pins) / sizeof(power_latch_pins[0]); i++) {
        esp_err_t err = gpio_hold_dis(power_latch_pins[i]);
        if (err != ESP_OK) return err;
        err = gpio_set_level(power_latch_pins[i], 0);
        if (err != ESP_OK) return err;
        err = gpio_hold_en(power_latch_pins[i]);
        if (err != ESP_OK) return err;
    }
    gpio_deep_sleep_hold_en();
    return ESP_OK;
}
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
#ifdef SOLAR_OS_BOARD_POWER_LATCH_PINS
    return solar_os_power_register_off_provider(SOLAR_OS_BOARD_POWEROFF_PROVIDER,
                                               board_power_off, power_latch_pins);
#endif
    return ESP_OK;
}

esp_err_t solar_os_board_boot_start_jobs(solar_os_context_t *ctx)
{
    (void)ctx;
#ifdef SOLAR_OS_BOARD_DEFAULT_PORT_SHELL
    uint8_t session_id = 0;
    const esp_err_t shell_err = solar_os_port_shell_start(
        ctx, SOLAR_OS_BOARD_DEFAULT_PORT_SHELL, false, &session_id);
    if (shell_err != ESP_OK) return shell_err;
    ESP_LOGI("board_boot", "Console shell session %u started on %s",
             (unsigned)session_id, SOLAR_OS_BOARD_DEFAULT_PORT_SHELL);
#endif
#if SOLAR_OS_PACKAGE_JOB_POINTER_ARROWS && defined(SOLAR_OS_BOARD_POINTER_ARROWS_SOURCE)
    char job[] = "pointer-arrows";
    char source[] = "source=" SOLAR_OS_BOARD_POINTER_ARROWS_SOURCE;
    char *args[] = {job, source};
    return solar_os_jobs_start(ctx, job, 2, args);
#else
    return ESP_OK;
#endif
}
