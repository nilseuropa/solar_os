#include "solar_os_logic.h"

#include <string.h>

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "solar_os_gpio.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"

static const char *TAG = "solar_os_logic";

typedef struct {
    SemaphoreHandle_t mutex;
    uint8_t *samples;
    size_t capacity;
    solar_os_logic_status_t status;
} logic_state_t;

static logic_state_t logic_state;
static StaticSemaphore_t logic_mutex_buffer;

static esp_err_t logic_ensure_init(void)
{
    if (logic_state.mutex != NULL) {
        return ESP_OK;
    }

    if (logic_state.mutex == NULL) {
        logic_state.mutex = xSemaphoreCreateMutexStatic(&logic_mutex_buffer);
        logic_state.status.last_error = logic_state.mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    return logic_state.mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint8_t logic_read_pins(const solar_os_logic_config_t *config)
{
    const uint32_t low = REG_READ(GPIO_IN_REG);
    const uint32_t high = REG_READ(GPIO_IN1_REG);
    uint8_t sample = 0;

    for (uint8_t channel = 0; channel < config->channel_count; channel++) {
        const uint8_t pin = config->pins[channel];
        const bool level = pin < 32U ?
            (low & (1UL << pin)) != 0 :
            (high & (1UL << (pin - 32U))) != 0;
        if (level) {
            sample |= (uint8_t)(1U << channel);
        }
    }
    return sample;
}

static esp_err_t logic_ensure_capacity(size_t sample_count)
{
    if (logic_state.samples != NULL && logic_state.capacity >= sample_count) {
        return ESP_OK;
    }

    uint8_t *replacement = solar_os_memory_alloc(sample_count,
                                                 SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
                                                 "logic.samples");
    if (replacement == NULL) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_memory_free(logic_state.samples);
    logic_state.samples = replacement;
    logic_state.capacity = sample_count;
    return ESP_OK;
}

static void logic_sample(const solar_os_logic_config_t *config,
                         uint8_t *samples,
                         uint64_t *started_us,
                         uint64_t *duration_us)
{
    const uint32_t ticks_per_us = esp_rom_get_cpu_ticks_per_us();
    uint64_t cycles_per_sample_q32 =
        (((uint64_t)ticks_per_us * 1000000ULL) << 32) / config->sample_rate_hz;
    if (cycles_per_sample_q32 == 0) {
        cycles_per_sample_q32 = 1;
    }

    const uint32_t start_cycle = esp_cpu_get_cycle_count();
    uint64_t next_cycle_q32 = 0;
    *started_us = (uint64_t)esp_timer_get_time();

    for (uint32_t i = 0; i < config->sample_count; i++) {
        samples[i] = logic_read_pins(config);
        next_cycle_q32 += cycles_per_sample_q32;
        const uint32_t target = start_cycle + (uint32_t)(next_cycle_q32 >> 32);
        while ((int32_t)(esp_cpu_get_cycle_count() - target) < 0) {
        }
    }

    *duration_us = (uint64_t)esp_timer_get_time() - *started_us;
}

esp_err_t solar_os_logic_init(void)
{
    return logic_ensure_init();
}

esp_err_t solar_os_logic_default_config(solar_os_logic_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->sample_rate_hz = SOLAR_OS_LOGIC_DEFAULT_RATE_HZ;
    config->sample_count = SOLAR_OS_LOGIC_DEFAULT_SAMPLES;

    const size_t pin_count = solar_os_gpio_pin_count();
    for (size_t i = 0;
         i < pin_count && config->channel_count < SOLAR_OS_LOGIC_MAX_CHANNELS;
         i++) {
        solar_os_gpio_pin_info_t info;
        if (solar_os_gpio_get_pin_info(i, &info) && info.runtime_allowed) {
            config->pins[config->channel_count++] = (uint8_t)info.pin;
        }
    }

    return config->channel_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_logic_validate_config(const solar_os_logic_config_t *config)
{
    if (config == NULL ||
        config->channel_count == 0 ||
        config->channel_count > SOLAR_OS_LOGIC_MAX_CHANNELS ||
        config->sample_rate_hz < SOLAR_OS_LOGIC_MIN_RATE_HZ ||
        config->sample_rate_hz > SOLAR_OS_LOGIC_MAX_RATE_HZ ||
        config->sample_count == 0 ||
        config->sample_count > SOLAR_OS_LOGIC_MAX_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < config->channel_count; i++) {
        if (!solar_os_gpio_is_runtime_allowed(config->pins[i])) {
            return ESP_ERR_NOT_ALLOWED;
        }
        for (uint8_t j = 0; j < i; j++) {
            if (config->pins[i] == config->pins[j]) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_logic_capture(const solar_os_logic_config_t *config)
{
    uint32_t effective_rate_hz = 0;
    esp_err_t err = logic_ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_logic_validate_config(config);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(logic_state.mutex, portMAX_DELAY);
    logic_state.status.capturing = true;
    logic_state.status.last_error = ESP_OK;

    for (uint8_t i = 0; i < config->channel_count; i++) {
        err = solar_os_gpio_configure(config->pins[i],
                                      SOLAR_OS_GPIO_MODE_INPUT,
                                      SOLAR_OS_GPIO_PULL_NONE);
        if (err != ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        err = logic_ensure_capacity(config->sample_count);
    }

    uint64_t started_us = 0;
    uint64_t duration_us = 0;
    if (err == ESP_OK) {
        logic_sample(config, logic_state.samples, &started_us, &duration_us);
        logic_state.status.config = *config;
        logic_state.status.capture_started_us = started_us;
        logic_state.status.capture_duration_us = duration_us;
        logic_state.status.effective_rate_hz = duration_us > 0 ?
            (uint32_t)(((uint64_t)config->sample_count * 1000000ULL) / duration_us) :
            config->sample_rate_hz;
        effective_rate_hz = logic_state.status.effective_rate_hz;
        logic_state.status.has_capture = true;
        logic_state.status.generation++;
    }

    logic_state.status.capturing = false;
    logic_state.status.last_error = err;
    xSemaphoreGive(logic_state.mutex);

    if (err == ESP_OK) {
        SOLAR_OS_LOGI(TAG,
                      "capture: channels=%u samples=%lu requested=%luHz effective=%luHz",
                      (unsigned)config->channel_count,
                      (unsigned long)config->sample_count,
                      (unsigned long)config->sample_rate_hz,
                      (unsigned long)effective_rate_hz);
    }
    return err;
}

esp_err_t solar_os_logic_get_status(solar_os_logic_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = logic_ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(logic_state.mutex, portMAX_DELAY);
    *status = logic_state.status;
    xSemaphoreGive(logic_state.mutex);
    return ESP_OK;
}

esp_err_t solar_os_logic_copy_samples(size_t start,
                                      uint8_t *samples,
                                      size_t max_samples,
                                      size_t *copied)
{
    if (copied != NULL) {
        *copied = 0;
    }
    if ((samples == NULL && max_samples > 0) || copied == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = logic_ensure_init();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(logic_state.mutex, portMAX_DELAY);
    if (!logic_state.status.has_capture || start >= logic_state.status.config.sample_count) {
        xSemaphoreGive(logic_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t available = logic_state.status.config.sample_count - start;
    *copied = available < max_samples ? available : max_samples;
    memcpy(samples, &logic_state.samples[start], *copied);
    xSemaphoreGive(logic_state.mutex);
    return ESP_OK;
}
