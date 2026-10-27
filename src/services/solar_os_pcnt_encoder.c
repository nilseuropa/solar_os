#include "solar_os_pcnt_encoder.h"

#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pcnt_encoder.h"
#include "solar_os_encoder.h"
#include "solar_os_memory.h"

typedef struct device {
    struct device *next;
    char name[SOLAR_OS_ENCODER_NAME_MAX];
    pcnt_encoder_config_t config;
    pcnt_encoder_t chip;
} device_t;

static device_t *devices;
static StaticSemaphore_t registry_storage;
static SemaphoreHandle_t registry_mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (registry_mutex == NULL) registry_mutex = xSemaphoreCreateMutexStatic(&registry_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
}

static esp_err_t parse(const solar_os_expansion_binding_t *bindings, size_t count,
    pcnt_encoder_config_t *config)
{
    *config = (pcnt_encoder_config_t){.pin_a = -1, .pin_b = -1, .filter_ns = 1000};
    unsigned seen = 0;
    if (bindings == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        unsigned bit;
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && strcmp(b->role, "a") == 0 && GPIO_IS_VALID_GPIO(b->value)) {
            bit = 1U; config->pin_a = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && strcmp(b->role, "b") == 0 && GPIO_IS_VALID_GPIO(b->value)) {
            bit = 2U; config->pin_b = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && strcmp(b->role, "mode") == 0 &&
            (b->value == 1 || b->value == 2 || b->value == 4)) {
            bit = 4U; config->mode = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && strcmp(b->role, "invert") == 0 &&
            (b->value == 0 || b->value == 1)) {
            bit = 8U; config->invert = b->value != 0;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && strcmp(b->role, "pull") == 0 &&
            b->value >= 0 && b->value <= 2) {
            bit = 16U; config->pull = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && strcmp(b->role, "filter_ns") == 0 && b->value >= 0) {
            bit = 32U; config->filter_ns = (uint32_t)b->value;
        } else return ESP_ERR_INVALID_ARG;
        if (seen & bit) return ESP_ERR_INVALID_ARG;
        seen |= bit;
    }
    return (seen & 7U) == 7U && config->pin_a != config->pin_b ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_pcnt_encoder_validate(const solar_os_expansion_binding_t *bindings,
    size_t count, solar_os_expansion_binding_validation_t *validation)
{
    pcnt_encoder_config_t config;
    const esp_err_t ret = parse(bindings, count, &config);
    if (ret != ESP_OK && validation != NULL) {
        validation->reason = SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE;
        strlcpy(validation->key, "a/b/mode", sizeof(validation->key));
    }
    return ret;
}

static esp_err_t read_count(void *ctx, int32_t *count)
{
    return pcnt_encoder_read(&((device_t *)ctx)->chip, count);
}

static esp_err_t reset(void *ctx)
{
    return pcnt_encoder_reset(&((device_t *)ctx)->chip);
}

static esp_err_t shutdown(void *ctx)
{
    device_t *d = ctx;
    esp_err_t ret = pcnt_encoder_deinit(&d->chip);
    if (ret != ESP_OK) return ret;
    ret = gpio_reset_pin(d->config.pin_a);
    if (ret != ESP_OK) return ret;
    return gpio_reset_pin(d->config.pin_b);
}

esp_err_t solar_os_pcnt_encoder_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (name == NULL || name[0] == '\0' || strnlen(name, SOLAR_OS_ENCODER_NAME_MAX) >= SOLAR_OS_ENCODER_NAME_MAX)
        return ESP_ERR_INVALID_ARG;
    pcnt_encoder_config_t config;
    esp_err_t ret = parse(bindings, count, &config);
    if (ret != ESP_OK) return ret;
    lock();
    for (device_t *d = devices; d != NULL; d = d->next) if (strcmp(d->name, name) == 0) {
        xSemaphoreGive(registry_mutex); return ESP_ERR_INVALID_STATE;
    }
    device_t *d = solar_os_memory_calloc(1U, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "encoder.pcnt");
    if (d == NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_NO_MEM; }
    d->config = config;
    strlcpy(d->name, name, sizeof(d->name));
    ret = pcnt_encoder_init(&d->chip, &config);
    if (ret == ESP_OK) {
        static const solar_os_encoder_ops_t ops = {.read = read_count, .reset = reset, .shutdown = shutdown};
        const solar_os_encoder_registration_t registration = {
            .name = name, .driver = "pcnt-encoder", .mode = (solar_os_encoder_mode_t)config.mode, .ops = &ops, .ctx = d,
        };
        ret = solar_os_encoder_register(&registration);
    }
    if (ret == ESP_OK) { d->next = devices; devices = d; }
    else {
        (void)shutdown(d);
        solar_os_memory_free(d);
    }
    xSemaphoreGive(registry_mutex);
    return ret;
}

esp_err_t solar_os_pcnt_encoder_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock();
    device_t **link = &devices;
    while (*link != NULL && strcmp((*link)->name, name) != 0) link = &(*link)->next;
    if (*link == NULL) { xSemaphoreGive(registry_mutex); return ESP_ERR_NOT_FOUND; }
    device_t *d = *link;
    const esp_err_t ret = solar_os_encoder_unregister(name);
    if (ret == ESP_OK) { *link = d->next; solar_os_memory_free(d); }
    xSemaphoreGive(registry_mutex);
    return ret;
}
