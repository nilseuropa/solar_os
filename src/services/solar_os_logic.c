#include "solar_os_logic.h"
#include "drivers/solar_os_gpio_isr.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
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
#include "solar_os_sync.h"

static const char *TAG = "solar_os_logic";

typedef struct {
    SemaphoreHandle_t mutex;
    uint8_t *samples;
    size_t capacity;
    solar_os_logic_status_t status;
    int pins[SOLAR_OS_LOGIC_MAX_CHANNELS + 1];
    uint16_t pin_claims;
    bool trigger_attached;
    gpio_num_t trigger_pin;
} logic_state_t;

static logic_state_t logic_state;
static StaticSemaphore_t logic_mutex_buffer;

static atomic_flag capture_busy = ATOMIC_FLAG_INIT;
static const char LOGIC_OWNER[] = "logic.capture";
static StaticSemaphore_t trigger_buffer;
static SemaphoreHandle_t trigger_signal;
static portMUX_TYPE trigger_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t trigger_sequence, trigger_armed;

static void logic_trigger_isr(void *arg)
{
    BaseType_t task_woken = pdFALSE;
    portENTER_CRITICAL_ISR(&trigger_lock);
    if (trigger_armed == (uint32_t)(uintptr_t)arg) {
        xSemaphoreGiveFromISR(trigger_signal, &task_woken);
    }
    portEXIT_CRITICAL_ISR(&trigger_lock);
    if (task_woken == pdTRUE) portYIELD_FROM_ISR();
}

static esp_err_t logic_ensure_init(void)
{
    return solar_os_mutex_get(&logic_state.mutex, &logic_mutex_buffer) ? ESP_OK : ESP_ERR_NO_MEM;
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

static esp_err_t logic_detach_trigger(void)
{
    if (!logic_state.trigger_attached) return ESP_OK;
    portENTER_CRITICAL(&trigger_lock);
    trigger_armed = 0;
    portEXIT_CRITICAL(&trigger_lock);
    esp_err_t error = gpio_isr_handler_remove(logic_state.trigger_pin);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = gpio_set_intr_type(logic_state.trigger_pin, GPIO_INTR_DISABLE);
    if (error == ESP_OK) logic_state.trigger_attached = false;
    return error;
}

static esp_err_t logic_release_inputs(void)
{
    esp_err_t result = logic_detach_trigger();
    if (result != ESP_OK) return result;
    for (size_t i = 0; i < SOLAR_OS_LOGIC_MAX_CHANNELS + 1; ++i) {
        const uint16_t claim = UINT16_C(1) << i;
        if (!(logic_state.pin_claims & claim)) continue;
        const esp_err_t error = solar_os_gpio_release_owned(logic_state.pins[i], LOGIC_OWNER);
        if (error == ESP_OK || error == ESP_ERR_NOT_FOUND) logic_state.pin_claims &= ~claim;
        else if (result == ESP_OK) result = error;
    }
    return result;
}

static esp_err_t logic_configure_inputs(const solar_os_logic_config_t *config)
{
    const char *labels[SOLAR_OS_LOGIC_MAX_CHANNELS + 1];
    size_t count = config->channel_count;
    bool trigger_is_channel = false;
    for (size_t i = 0; i < count; ++i) {
        logic_state.pins[i] = config->pins[i];
        labels[i] = "capture input";
        if (config->pins[i] == config->trigger_pin) trigger_is_channel = true;
    }
    if (config->trigger_enabled && !trigger_is_channel) {
        logic_state.pins[count] = config->trigger_pin;
        labels[count++] = "capture trigger";
    }
    esp_err_t error = solar_os_gpio_claim_pins(logic_state.pins, labels, count, LOGIC_OWNER, NULL);
    if (error != ESP_OK) return error;
    logic_state.pin_claims = (UINT16_C(1) << count) - 1;
    for (size_t i = 0; i < count; ++i) {
        error = solar_os_gpio_configure_owned(logic_state.pins[i], SOLAR_OS_GPIO_MODE_INPUT,
            SOLAR_OS_GPIO_PULL_NONE, LOGIC_OWNER);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

static bool logic_cancelled(const solar_os_logic_capture_options_v1_t *options)
{
    return options->cancelled && options->cancelled(options->user);
}

static esp_err_t logic_wait_for_trigger(uint8_t pin, const solar_os_logic_capture_options_v1_t *options)
{
    if (!trigger_signal) trigger_signal = xSemaphoreCreateBinaryStatic(&trigger_buffer);
    if (!trigger_signal) return ESP_ERR_NO_MEM;
    (void)xSemaphoreTake(trigger_signal, 0);
    esp_err_t error = solar_os_gpio_isr_service_ensure();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    error = gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
    if (error != ESP_OK) return error;
    if (++trigger_sequence == 0) ++trigger_sequence;
    portENTER_CRITICAL(&trigger_lock);
    trigger_armed = trigger_sequence;
    portEXIT_CRITICAL(&trigger_lock);
    error = gpio_isr_handler_add(pin, logic_trigger_isr, (void *)(uintptr_t)trigger_sequence);
    if (error != ESP_OK) return error;
    logic_state.trigger_attached = true;
    logic_state.trigger_pin = pin;

    const bool bounded = options->trigger_timeout_ms != 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)options->trigger_timeout_ms * 1000;
    for (;;) {
        if (logic_cancelled(options)) return ESP_ERR_NOT_FINISHED;
        TickType_t wait = portMAX_DELAY;
        if (bounded || options->cancelled) {
            uint32_t wait_ms = 20;
            if (bounded) {
                const int64_t remaining_us = deadline - esp_timer_get_time();
                if (remaining_us <= 0) return ESP_ERR_TIMEOUT;
                if (remaining_us < 20000) wait_ms = (uint32_t)(remaining_us + 999) / 1000;
            }
            wait = pdMS_TO_TICKS(wait_ms);
            if (!wait) wait = 1;
        }
        if (xSemaphoreTake(trigger_signal, wait) == pdTRUE) return ESP_OK;
    }
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
    if (config->trigger_enabled &&
        !solar_os_gpio_is_runtime_allowed(config->trigger_pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

esp_err_t solar_os_logic_capture_with_options(const solar_os_logic_config_t *requested,
    const solar_os_logic_capture_options_v1_t *options, uint32_t *generation)
{
    if (generation) *generation = 0;
    if (!requested) return ESP_ERR_INVALID_ARG;
    const solar_os_logic_config_t config = *requested;
    const solar_os_logic_capture_options_v1_t policy = options ? *options : (solar_os_logic_capture_options_v1_t){0};
    esp_err_t error = solar_os_logic_validate_config(&config);
    if (error != ESP_OK) return error;
    error = logic_ensure_init();
    if (error != ESP_OK) return error;
    if (atomic_flag_test_and_set(&capture_busy)) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(logic_state.mutex, portMAX_DELAY);
    logic_state.status.capturing = true;
    logic_state.status.last_error = ESP_OK;
    uint8_t *buffer = logic_state.samples;
    const bool grow = logic_state.capacity < config.sample_count;
    xSemaphoreGive(logic_state.mutex);

    error = logic_release_inputs();
    if (error == ESP_OK && logic_cancelled(&policy)) error = ESP_ERR_NOT_FINISHED;
    if (error == ESP_OK && grow) {
        buffer = solar_os_memory_alloc(config.sample_count, SOLAR_OS_MEMORY_INTERNAL_PREFERRED, "logic.samples");
        if (!buffer) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK) error = logic_configure_inputs(&config);
    if (error == ESP_OK && config.trigger_enabled) {
        error = logic_wait_for_trigger(config.trigger_pin, &policy);
        if (error == ESP_OK) error = logic_detach_trigger();
    }

    uint32_t effective_rate_hz = 0;
    uint8_t *discard = NULL;
    if (error == ESP_OK) {
        xSemaphoreTake(logic_state.mutex, portMAX_DELAY);
        uint64_t started_us = 0, duration_us = 0;
        logic_sample(&config, buffer, &started_us, &duration_us);
        if (grow) {
            discard = logic_state.samples;
            logic_state.samples = buffer;
            logic_state.capacity = config.sample_count;
        }
        logic_state.status.config = config;
        logic_state.status.capture_started_us = started_us;
        logic_state.status.capture_duration_us = duration_us;
        effective_rate_hz = duration_us ?
            (uint32_t)(((uint64_t)config.sample_count * 1000000ULL) / duration_us) : config.sample_rate_hz;
        logic_state.status.effective_rate_hz = effective_rate_hz;
        logic_state.status.has_capture = true;
        if (++logic_state.status.generation == 0) ++logic_state.status.generation;
        if (generation) *generation = logic_state.status.generation;
        xSemaphoreGive(logic_state.mutex);
    } else if (grow && buffer != logic_state.samples) {
        discard = buffer;
    }
    solar_os_memory_free(discard);
    const esp_err_t release = logic_release_inputs();
    if (error == ESP_OK) error = release;
    xSemaphoreTake(logic_state.mutex, portMAX_DELAY);
    logic_state.status.capturing = false;
    logic_state.status.last_error = error;
    xSemaphoreGive(logic_state.mutex);
    atomic_flag_clear(&capture_busy);
    if (error == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "capture: channels=%u samples=%lu requested=%luHz effective=%luHz trigger=%d",
            (unsigned)config.channel_count, (unsigned long)config.sample_count,
            (unsigned long)config.sample_rate_hz, (unsigned long)effective_rate_hz,
            config.trigger_enabled ? (int)config.trigger_pin : -1);
    }
    return error;
}

esp_err_t solar_os_logic_capture(const solar_os_logic_config_t *config)
{
    return solar_os_logic_capture_with_options(config, NULL, NULL);
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

esp_err_t solar_os_logic_copy_generation(uint32_t generation, size_t start,
    uint8_t *samples, size_t max_samples, size_t *copied)
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
    if (logic_state.status.capturing) {
        xSemaphoreGive(logic_state.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!logic_state.status.has_capture || (generation && generation != logic_state.status.generation) ||
        start >= logic_state.status.config.sample_count) {
        xSemaphoreGive(logic_state.mutex);
        return ESP_ERR_NOT_FOUND;
    }

    const size_t available = logic_state.status.config.sample_count - start;
    *copied = available < max_samples ? available : max_samples;
    if (*copied) memcpy(samples, &logic_state.samples[start], *copied);
    xSemaphoreGive(logic_state.mutex);
    return ESP_OK;
}

esp_err_t solar_os_logic_copy_samples(size_t start, uint8_t *samples, size_t max_samples, size_t *copied)
{
    return solar_os_logic_copy_generation(0, start, samples, max_samples, copied);
}
