#include "solar_os_gpio_isr.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atomic_flag install_lock = ATOMIC_FLAG_INIT;
static atomic_bool installed;

esp_err_t solar_os_gpio_isr_service_ensure(void)
{
    if (atomic_load(&installed)) {
        return ESP_OK;
    }
    while (atomic_flag_test_and_set(&install_lock)) {
        vTaskDelay(1);
    }
    esp_err_t err = ESP_OK;
    if (!atomic_load(&installed)) {
        err = gpio_install_isr_service(0);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            atomic_store(&installed, true);
            err = ESP_OK;
        }
    }
    atomic_flag_clear(&install_lock);
    return err;
}
