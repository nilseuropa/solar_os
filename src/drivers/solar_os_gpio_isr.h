#pragma once

#include "esp_err.h"

/* Shared, task-context installation. The service remains installed for boot. */
esp_err_t solar_os_gpio_isr_service_ensure(void);
