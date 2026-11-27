#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include <freertos/semphr.h>
#include "solar_os_gpio.h"
#include "solar_os_logic.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_native_logic.h"

static unsigned live_buffers, samples_read;
static uint64_t pins;
static uint32_t cpu_cycles;
static int64_t now_us;
static bool edge, replay_old_edge, fail_detach, fail_release, fail_allocate;
static void (*trigger)(void *), (*old_trigger)(void *);
static void *trigger_arg, *old_arg;
static const solar_os_native_logic_api_v1_t *api;
static const solar_os_logic_config_t base = {.channel_count = 2, .pins = {2, 35},
    .sample_rate_hz = 100000, .sample_count = 4};

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{
    assert(level == SOLAR_OS_LOG_LEVEL_INFO && !strcmp(tag, "solar_os_logic") && format);
    return ESP_OK;
}

int64_t esp_timer_get_time(void) { return now_us; }
uint32_t esp_cpu_get_cycle_count(void) { return cpu_cycles += 1000; }
uint32_t esp_rom_get_cpu_ticks_per_us(void) { return 100; }
uint32_t mock_gpio_read(unsigned reg)
{
    assert(!trigger && pins);
    samples_read++;
    now_us += 5;
    return reg == 0 ? (1U << 2) : 0;
}

esp_err_t solar_os_gpio_isr_service_ensure(void) { return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t pin, int type)
{
    assert(pins & (UINT64_C(1) << pin));
    assert(type == GPIO_INTR_ANYEDGE || type == GPIO_INTR_DISABLE);
    return ESP_OK;
}
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *user)
{
    assert(pins & (UINT64_C(1) << pin));
    assert(!trigger);
    trigger = handler;
    trigger_arg = user;
    return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
    assert(pins & (UINT64_C(1) << pin));
    if (fail_detach) return ESP_FAIL;
    old_trigger = trigger;
    old_arg = trigger_arg;
    trigger = NULL;
    return ESP_OK;
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    *storage = (StaticSemaphore_t){.binary = true};
    return storage;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    if (!sem->binary) {
        assert(!sem->set);
        sem->set = true;
        return pdTRUE;
    }
    if (ticks && replay_old_edge && old_trigger) {
        replay_old_edge = false;
        old_trigger(old_arg);
    }
    if (ticks && edge && trigger) {
        edge = false;
        trigger(trigger_arg);
    }
    if (sem->set) {
        sem->set = false;
        return pdTRUE;
    }
    now_us += (int64_t)ticks * 1000;
    return pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem->set && !sem->binary);
    sem->set = false;
    return pdTRUE;
}
void xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken)
{
    assert(sem->binary);
    sem->set = true;
    *woken = pdTRUE;
}

bool solar_os_gpio_is_runtime_allowed(int pin) { return pin == 2 || pin == 5 || pin == 35; }
size_t solar_os_gpio_pin_count(void) { return 3; }
bool solar_os_gpio_get_pin_info(size_t index, solar_os_gpio_pin_info_t *info)
{
    static const int defined[] = {2, 5, 35};
    assert(index < sizeof(defined)/sizeof(defined[0]));
    *info = (solar_os_gpio_pin_info_t){.pin = defined[index], .runtime_allowed = true};
    return true;
}
esp_err_t solar_os_gpio_claim_pins(const int *new_pins, const char *const *labels, size_t count,
    const char *owner, solar_os_resource_conflict_t *conflict)
{
    assert(!pins && !conflict && !strcmp(owner, "logic.capture"));
    for (size_t i = 0; i < count; ++i) {
        assert(labels[i] && !(pins & (UINT64_C(1) << new_pins[i])));
        pins |= UINT64_C(1) << new_pins[i];
    }
    return ESP_OK;
}
esp_err_t solar_os_gpio_configure_owned(int pin, solar_os_gpio_mode_t mode, solar_os_gpio_pull_t pull,
    const char *owner)
{
    assert((pins & (UINT64_C(1) << pin)) && mode == SOLAR_OS_GPIO_MODE_INPUT &&
        pull == SOLAR_OS_GPIO_PULL_NONE && !strcmp(owner, "logic.capture"));
    return ESP_OK;
}
esp_err_t solar_os_gpio_release_owned(int pin, const char *owner)
{
    assert(!trigger && (pins & (UINT64_C(1) << pin)) && !strcmp(owner, "logic.capture"));
    if (fail_release && pin == 2) return ESP_FAIL;
    pins &= ~(UINT64_C(1) << pin);
    return ESP_OK;
}

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t cls, const char *tag)
{
    assert(bytes && cls == SOLAR_OS_MEMORY_INTERNAL_PREFERRED && !strcmp(tag, "logic.samples"));
    if (fail_allocate) return NULL;
    void *result = malloc(bytes);
    assert(result);
    live_buffers++;
    return result;
}
void solar_os_memory_free(void *buffer)
{
    if (buffer) {
        assert(live_buffers);
        live_buffers--;
        free(buffer);
    }
}

static bool cancel_armed(void *user)
{
    const solar_os_logic_config_t *config = user;
    solar_os_logic_status_t status;
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK && status.capturing);
    assert(api->capture(config, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return trigger != NULL;
}

static void expect_latest(uint32_t generation)
{
    solar_os_logic_status_t status;
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK && !status.capturing);
    assert(status.has_capture && status.generation == generation && status.effective_rate_hz == 100000);
    uint8_t data[8];
    size_t count;
    assert(api->read(generation, 0, data, sizeof(data), &count) == SOLAR_OS_NATIVE_OK);
    assert(count == status.config.sample_count && count <= sizeof(data));
    for (size_t i = 0; i < count; ++i) assert(data[i] == 1);
    assert(api->read(generation, 0, NULL, 0, &count) == SOLAR_OS_NATIVE_OK && count == 0);
}

int main(void)
{
    api = solar_os_native_logic_get_service(SOLAR_OS_NATIVE_LOGIC_SERVICE, SOLAR_OS_NATIVE_LOGIC_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    solar_os_logic_config_t config;
    assert(api->defaults(&config) == SOLAR_OS_NATIVE_OK && config.channel_count == 3);
    config.pins[1] = config.pins[0];
    assert(api->validate(&config) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    uint32_t generation = 0;
    assert(api->capture(&base, NULL, &generation) == SOLAR_OS_NATIVE_OK && generation == 1);
    assert(!pins && live_buffers == 1);
    expect_latest(1);

    config = base;
    config.trigger_enabled = true;
    config.trigger_pin = 5;
    config.sample_count = 8;
    solar_os_logic_capture_options_v1_t options = {.cancelled = cancel_armed, .user = &config};
    assert(api->capture(&config, &options, &generation) == SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(!generation && !pins && !trigger && live_buffers == 1);
    expect_latest(1);

    options = (solar_os_logic_capture_options_v1_t){.trigger_timeout_ms = 7};
    replay_old_edge = true;
    assert(api->capture(&config, &options, &generation) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!generation && !pins && !trigger && !replay_old_edge && live_buffers == 1);
    expect_latest(1);

    fail_allocate = true;
    assert(api->capture(&config, &options, &generation) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(!generation && !pins);
    fail_allocate = false;

    fail_detach = true;
    edge = true;
    assert(api->capture(&config, &options, &generation) == SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!generation && pins && trigger && live_buffers == 1);
    trigger(trigger_arg); /* The retired callback cannot arm another capture. */
    expect_latest(1);

    fail_detach = false;
    edge = true;
    assert(api->capture(&config, &options, &generation) == SOLAR_OS_NATIVE_OK && generation == 2);
    assert(!pins && !trigger && live_buffers == 1);
    expect_latest(2);
    uint8_t byte;
    size_t count;
    assert(api->read(1, 0, &byte, 1, &count) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !count);

    fail_release = true;
    assert(api->capture(&base, NULL, &generation) == SOLAR_OS_NATIVE_ERROR_FAILED && generation == 3);
    assert(pins == (UINT64_C(1) << 2));
    fail_release = false;
    assert(api->capture(&base, NULL, &generation) == SOLAR_OS_NATIVE_OK && generation == 4);
    assert(!pins && samples_read == 40 && live_buffers == 1);
    puts("native logic capture: ownership, cancellation, trigger generation, read consistency, and cleanup retry passed");
    return 0;
}
