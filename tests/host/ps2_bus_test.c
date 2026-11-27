#include <assert.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "ps2_bus.h"

static void (*irq)(void *);
static void *irq_arg;
static bool pin_level;
static bool fail_remove;
static unsigned reads;
static int64_t now_us;
static TickType_t tick;

esp_err_t solar_os_gpio_isr_service_ensure(void) { return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->mode == GPIO_MODE_INPUT_OUTPUT_OD);
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    assert(pin == 2 || pin == 3);
    assert(level <= 1);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin)
{
    assert(pin == 3);
    ++reads;
    return pin_level;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type)
{
    assert(pin == 2 && (type == GPIO_INTR_DISABLE || type == GPIO_INTR_NEGEDGE));
    return ESP_OK;
}
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg)
{
    assert(pin == 2 && !irq);
    irq = handler;
    irq_arg = arg;
    return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{
    assert(pin == 2 && irq);
    if (fail_remove) return ESP_FAIL;
    irq = NULL;
    irq_arg = NULL;
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return now_us; }
void esp_rom_delay_us(uint32_t us) { now_us += us; }
TickType_t xTaskGetTickCount(void) { return tick; }
void vTaskDelay(TickType_t ticks) { tick += ticks; }

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    *storage = (StaticSemaphore_t){.binary = true};
    return storage;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t wait)
{
    assert(sem->binary && !sem->set);
    tick += wait;
    return pdFALSE;
}
void xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *task_woken)
{
    assert(sem->binary);
    sem->set = true;
    *task_woken = pdTRUE;
}

static void feed_bit(bool bit)
{
    assert(irq);
    pin_level = bit;
    now_us += 100;
    irq(irq_arg);
}

static void feed_byte(uint8_t value)
{
    feed_bit(false);
    bool parity = true;
    for (unsigned bit = 0; bit < 8; ++bit) {
        const bool high = (value >> bit) & 1;
        feed_bit(high);
        parity ^= high;
    }
    feed_bit(parity);
    feed_bit(true);
}

int main(void)
{
    solar_os_ps2_bus_t bus = {0}, other = {0};
    const solar_os_bus_ps2_config_t config = {.clock_pin = 2, .data_pin = 3};
    assert(solar_os_ps2_bus_start(&bus, &config) == ESP_OK);
    assert(solar_os_ps2_bus_start(&other, &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ps2_bus_start(&bus, &config) == ESP_ERR_INVALID_STATE);
    feed_byte(0x1c);
    uint8_t received;
    assert(solar_os_ps2_bus_read(&bus, &received, 1) == 1 && received == 0x1c);
    assert(solar_os_ps2_bus_command(&bus, 0xf4, 20) == ESP_ERR_TIMEOUT && tick == 20);

    void (*stale_irq)(void *) = irq;
    void *stale_arg = irq_arg;
    fail_remove = true;
    assert(solar_os_ps2_bus_stop_checked(&bus) == ESP_FAIL);
    assert(!bus.running);
    unsigned before = reads;
    irq(irq_arg);
    assert(reads == before);
    assert(solar_os_ps2_bus_start(&bus, &config) == ESP_ERR_INVALID_STATE);

    fail_remove = false;
    assert(solar_os_ps2_bus_stop_checked(&bus) == ESP_OK && !irq);
    assert(solar_os_ps2_bus_start(&other, &config) == ESP_OK);
    stale_irq(stale_arg);
    assert(reads == before);
    feed_byte(0x5a);
    assert(solar_os_ps2_bus_read(&other, &received, 1) == 1 && received == 0x5a);
    solar_os_ps2_bus_stats_t stats;
    solar_os_ps2_bus_get_stats(&other, &stats);
    assert(stats.bytes == 1 && !stats.frame_errors);
    assert(solar_os_ps2_bus_stop_checked(&other) == ESP_OK && !irq);
    puts("PS/2 transport: bytes, command timeout, checked close and delayed IRQ lifetime passed");
    return 0;
}
