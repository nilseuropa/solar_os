#include "solar_os_resources.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SOLAR_OS_RESOURCE_CLAIM_MAX 32

static solar_os_resource_claim_t claims[SOLAR_OS_RESOURCE_CLAIM_MAX];
static SemaphoreHandle_t claims_mutex;

static esp_err_t resources_ensure_init(void)
{
    if (claims_mutex != NULL) {
        return ESP_OK;
    }

    claims_mutex = xSemaphoreCreateMutex();
    if (claims_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static bool resource_key_matches(const solar_os_resource_claim_t *claim,
                                 solar_os_resource_kind_t kind,
                                 int primary,
                                 int secondary)
{
    return claim != NULL &&
        claim->active &&
        claim->kind == kind &&
        claim->primary == primary &&
        claim->secondary == secondary;
}

esp_err_t solar_os_resources_init(void)
{
    return resources_ensure_init();
}

esp_err_t solar_os_resource_claim(solar_os_resource_kind_t kind,
                                  int primary,
                                  int secondary,
                                  const char *owner,
                                  const char *label)
{
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = resources_ensure_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    xSemaphoreTake(claims_mutex, portMAX_DELAY);

    solar_os_resource_claim_t *free_claim = NULL;
    for (size_t i = 0; i < SOLAR_OS_RESOURCE_CLAIM_MAX; i++) {
        solar_os_resource_claim_t *claim = &claims[i];
        if (resource_key_matches(claim, kind, primary, secondary)) {
            if (strncmp(claim->owner, owner, sizeof(claim->owner)) == 0) {
                xSemaphoreGive(claims_mutex);
                return ESP_OK;
            }
            xSemaphoreGive(claims_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (!claim->active && free_claim == NULL) {
            free_claim = claim;
        }
    }

    if (free_claim == NULL) {
        xSemaphoreGive(claims_mutex);
        return ESP_ERR_NO_MEM;
    }

    *free_claim = (solar_os_resource_claim_t) {
        .active = true,
        .kind = kind,
        .primary = primary,
        .secondary = secondary,
    };
    strlcpy(free_claim->owner, owner, sizeof(free_claim->owner));
    strlcpy(free_claim->label, label != NULL ? label : "", sizeof(free_claim->label));

    xSemaphoreGive(claims_mutex);
    return ESP_OK;
}

esp_err_t solar_os_resource_release(solar_os_resource_kind_t kind,
                                    int primary,
                                    int secondary,
                                    const char *owner)
{
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = resources_ensure_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    xSemaphoreTake(claims_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_RESOURCE_CLAIM_MAX; i++) {
        solar_os_resource_claim_t *claim = &claims[i];
        if (!resource_key_matches(claim, kind, primary, secondary)) {
            continue;
        }
        if (strncmp(claim->owner, owner, sizeof(claim->owner)) != 0) {
            xSemaphoreGive(claims_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        memset(claim, 0, sizeof(*claim));
        xSemaphoreGive(claims_mutex);
        return ESP_OK;
    }

    xSemaphoreGive(claims_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_resource_release_owner(const char *owner)
{
    size_t released = 0;

    if (owner == NULL || owner[0] == '\0' || resources_ensure_init() != ESP_OK) {
        return 0;
    }

    xSemaphoreTake(claims_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_RESOURCE_CLAIM_MAX; i++) {
        solar_os_resource_claim_t *claim = &claims[i];
        if (!claim->active || strncmp(claim->owner, owner, sizeof(claim->owner)) != 0) {
            continue;
        }
        memset(claim, 0, sizeof(*claim));
        released++;
    }
    xSemaphoreGive(claims_mutex);

    return released;
}

size_t solar_os_resource_claim_count(void)
{
    size_t count = 0;

    if (resources_ensure_init() != ESP_OK) {
        return 0;
    }

    xSemaphoreTake(claims_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_RESOURCE_CLAIM_MAX; i++) {
        if (claims[i].active) {
            count++;
        }
    }
    xSemaphoreGive(claims_mutex);

    return count;
}

bool solar_os_resource_get_claim(size_t index, solar_os_resource_claim_t *claim)
{
    size_t current = 0;

    if (claim == NULL || resources_ensure_init() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(claims_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_RESOURCE_CLAIM_MAX; i++) {
        if (!claims[i].active) {
            continue;
        }
        if (current++ == index) {
            *claim = claims[i];
            xSemaphoreGive(claims_mutex);
            return true;
        }
    }
    xSemaphoreGive(claims_mutex);

    return false;
}

const char *solar_os_resource_kind_name(solar_os_resource_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_RESOURCE_GPIO_PIN:
        return "gpio";
    case SOLAR_OS_RESOURCE_ADC_PIN:
        return "adc";
    case SOLAR_OS_RESOURCE_PWM_PIN:
        return "pwm";
    case SOLAR_OS_RESOURCE_I2C_ADDRESS:
        return "i2c_addr";
    case SOLAR_OS_RESOURCE_SPI_CS:
        return "spi_cs";
    case SOLAR_OS_RESOURCE_UART_PORT:
        return "uart";
    default:
        return "unknown";
    }
}
