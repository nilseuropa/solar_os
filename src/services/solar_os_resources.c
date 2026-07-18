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

static bool request_key_matches(const solar_os_resource_request_t *a,
                                const solar_os_resource_request_t *b)
{
    return a != NULL &&
        b != NULL &&
        a->kind == b->kind &&
        a->primary == b->primary &&
        a->secondary == b->secondary;
}

static solar_os_resource_claim_t *resource_find_locked(solar_os_resource_kind_t kind,
                                                       int primary,
                                                       int secondary)
{
    for (size_t i = 0; i < SOLAR_OS_RESOURCE_CLAIM_MAX; i++) {
        if (resource_key_matches(&claims[i], kind, primary, secondary)) {
            return &claims[i];
        }
    }
    return NULL;
}

esp_err_t solar_os_resources_init(void)
{
    return resources_ensure_init();
}

esp_err_t solar_os_resource_claim_bundle(const solar_os_resource_request_t *requests,
                                         size_t request_count,
                                         const char *owner,
                                         solar_os_resource_conflict_t *conflict)
{
    if (conflict != NULL) {
        memset(conflict, 0, sizeof(*conflict));
    }
    if (requests == NULL ||
        request_count == 0 ||
        request_count > SOLAR_OS_RESOURCE_BUNDLE_MAX ||
        owner == NULL ||
        owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < request_count; i++) {
        for (size_t j = i + 1; j < request_count; j++) {
            if (request_key_matches(&requests[i], &requests[j])) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    const esp_err_t init_ret = resources_ensure_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    xSemaphoreTake(claims_mutex, portMAX_DELAY);

    size_t free_count = 0;
    for (size_t i = 0; i < SOLAR_OS_RESOURCE_CLAIM_MAX; i++) {
        if (!claims[i].active) {
            free_count++;
        }
    }

    size_t required_count = 0;
    for (size_t i = 0; i < request_count; i++) {
        solar_os_resource_claim_t *existing = resource_find_locked(requests[i].kind,
                                                                    requests[i].primary,
                                                                    requests[i].secondary);
        if (existing == NULL) {
            required_count++;
            continue;
        }
        if (strncmp(existing->owner, owner, sizeof(existing->owner)) != 0) {
            if (conflict != NULL) {
                conflict->request_index = i;
                conflict->existing = *existing;
            }
            xSemaphoreGive(claims_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (required_count > free_count) {
        xSemaphoreGive(claims_mutex);
        return ESP_ERR_NO_MEM;
    }

    for (size_t request_index = 0; request_index < request_count; request_index++) {
        const solar_os_resource_request_t *request = &requests[request_index];
        if (resource_find_locked(request->kind, request->primary, request->secondary) != NULL) {
            continue;
        }
        for (size_t claim_index = 0; claim_index < SOLAR_OS_RESOURCE_CLAIM_MAX; claim_index++) {
            solar_os_resource_claim_t *claim = &claims[claim_index];
            if (claim->active) {
                continue;
            }
            *claim = (solar_os_resource_claim_t) {
                .active = true,
                .kind = request->kind,
                .primary = request->primary,
                .secondary = request->secondary,
            };
            strlcpy(claim->owner, owner, sizeof(claim->owner));
            strlcpy(claim->label,
                    request->label != NULL ? request->label : "",
                    sizeof(claim->label));
            break;
        }
    }

    xSemaphoreGive(claims_mutex);
    return ESP_OK;
}

esp_err_t solar_os_resource_claim(solar_os_resource_kind_t kind,
                                  int primary,
                                  int secondary,
                                  const char *owner,
                                  const char *label)
{
    const solar_os_resource_request_t request = {
        .kind = kind,
        .primary = primary,
        .secondary = secondary,
        .label = label,
    };
    return solar_os_resource_claim_bundle(&request, 1, owner, NULL);
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

bool solar_os_resource_find_claim(solar_os_resource_kind_t kind,
                                  int primary,
                                  int secondary,
                                  solar_os_resource_claim_t *claim)
{
    if (claim == NULL || resources_ensure_init() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(claims_mutex, portMAX_DELAY);
    solar_os_resource_claim_t *existing = resource_find_locked(kind, primary, secondary);
    if (existing != NULL) {
        *claim = *existing;
    }
    xSemaphoreGive(claims_mutex);
    return existing != NULL;
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
