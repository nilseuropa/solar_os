#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SOLAR_OS_RESOURCE_OWNER_MAX 24
#define SOLAR_OS_RESOURCE_LABEL_MAX 24

typedef enum {
    SOLAR_OS_RESOURCE_GPIO_PIN,
    SOLAR_OS_RESOURCE_ADC_PIN,
    SOLAR_OS_RESOURCE_PWM_PIN,
    SOLAR_OS_RESOURCE_I2C_ADDRESS,
    SOLAR_OS_RESOURCE_SPI_CS,
    SOLAR_OS_RESOURCE_UART_PORT,
} solar_os_resource_kind_t;

typedef struct {
    bool active;
    solar_os_resource_kind_t kind;
    int primary;
    int secondary;
    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    char label[SOLAR_OS_RESOURCE_LABEL_MAX];
} solar_os_resource_claim_t;

esp_err_t solar_os_resources_init(void);
esp_err_t solar_os_resource_claim(solar_os_resource_kind_t kind,
                                  int primary,
                                  int secondary,
                                  const char *owner,
                                  const char *label);
esp_err_t solar_os_resource_release(solar_os_resource_kind_t kind,
                                    int primary,
                                    int secondary,
                                    const char *owner);
size_t solar_os_resource_release_owner(const char *owner);
size_t solar_os_resource_claim_count(void);
bool solar_os_resource_get_claim(size_t index, solar_os_resource_claim_t *claim);
const char *solar_os_resource_kind_name(solar_os_resource_kind_t kind);
