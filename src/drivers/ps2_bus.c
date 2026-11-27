#include "ps2_bus.h"
#include "solar_os_gpio_isr.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PS2_FRAME_GAP_US 2000LL
#define PS2_REQUEST_TO_SEND_US 120U
#define PS2_PROTOCOL_ACK 0xfaU
#define PS2_PROTOCOL_RESEND 0xfeU
#define PS2_COMMAND_RETRIES 2U

static solar_os_ps2_bus_t *ps2_buses;
static uint32_t ps2_next_interrupt;
static portMUX_TYPE ps2_bus_lock = portMUX_INITIALIZER_UNLOCKED;

static unsigned ps2_ones(uint8_t value)
{
    unsigned count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static void ps2_frame_reset(solar_os_ps2_bus_t *bus)
{
    bus->frame_bit = 0;
    bus->frame_data = 0;
    bus->frame_parity = 0;
}

static void ps2_handle_clock(solar_os_ps2_bus_t *bus, BaseType_t *woken)
{
    portENTER_CRITICAL_ISR(&bus->lock);
    if (bus->transmitting) {
        const uint8_t bit = bus->tx_bit;
        if (bit >= 1U && bit <= 8U) {
            (void)gpio_set_level((gpio_num_t)bus->data_pin,
                                 (bus->tx_byte >> (bit - 1U)) & 1U);
            bus->tx_bit++;
        } else if (bit == 9U) {
            (void)gpio_set_level((gpio_num_t)bus->data_pin,
                                 (ps2_ones(bus->tx_byte) & 1U) == 0U);
            bus->tx_bit++;
        } else if (bit == 10U) {
            /* Release data for the stop bit and device acknowledgement. */
            (void)gpio_set_level((gpio_num_t)bus->data_pin, 1);
            bus->tx_bit++;
        } else {
            const bool acknowledged =
                gpio_get_level((gpio_num_t)bus->data_pin) == 0;
            bus->tx_ack = acknowledged;
            bus->transmitting = false;
            bus->last_edge_us = 0;
            ps2_frame_reset(bus);
            if (acknowledged) {
                bus->stats.commands++;
            } else {
                bus->stats.command_errors++;
            }
            portEXIT_CRITICAL_ISR(&bus->lock);
            if (bus->tx_semaphore) xSemaphoreGiveFromISR(bus->tx_semaphore, woken);
            return;
        }
        portEXIT_CRITICAL_ISR(&bus->lock);
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (bus->last_edge_us != 0 && now_us - bus->last_edge_us > PS2_FRAME_GAP_US) {
        ps2_frame_reset(bus);
    }
    bus->last_edge_us = now_us;
    const bool high = gpio_get_level((gpio_num_t)bus->data_pin) != 0;

    if (bus->frame_bit == 0) {
        if (high) {
            bus->stats.frame_errors++;
            portEXIT_CRITICAL_ISR(&bus->lock);
            return;
        }
        bus->frame_bit = 1;
        portEXIT_CRITICAL_ISR(&bus->lock);
        return;
    }
    if (bus->frame_bit <= 8U) {
        if (high) {
            bus->frame_data |= (uint8_t)(1U << (bus->frame_bit - 1U));
        }
        bus->frame_bit++;
        portEXIT_CRITICAL_ISR(&bus->lock);
        return;
    }
    if (bus->frame_bit == 9U) {
        bus->frame_parity = high ? 1U : 0U;
        bus->frame_bit++;
        portEXIT_CRITICAL_ISR(&bus->lock);
        return;
    }

    const bool parity_ok = ((ps2_ones(bus->frame_data) + bus->frame_parity) & 1U) != 0;
    if (high && parity_ok) {
        const uint8_t next = (uint8_t)((bus->write_index + 1U) % SOLAR_OS_PS2_RX_BUFFER_SIZE);
        if (next == bus->read_index) {
            bus->stats.overruns++;
        } else {
            bus->rx_buffer[bus->write_index] = bus->frame_data;
            bus->write_index = next;
            bus->stats.bytes++;
        }
    } else {
        bus->stats.frame_errors++;
    }
    ps2_frame_reset(bus);
    portEXIT_CRITICAL_ISR(&bus->lock);
}

/* The GPIO callback owns an ID rather than a pointer to a caller's bus.
 * Detach removes the ID under this lock and joins any in-flight dispatch.
 * A delayed callback carrying an older ID cannot reach a reused bus. */
static void ps2_clock_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&ps2_bus_lock);
    for (solar_os_ps2_bus_t *bus = ps2_buses; bus; bus = bus->next) {
        if (bus->interrupt_id == (uint32_t)(uintptr_t)arg && bus->running) {
            ps2_handle_clock(bus, &woken);
            break;
        }
    }
    portEXIT_CRITICAL_ISR(&ps2_bus_lock);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
}

static esp_err_t ps2_reserve(solar_os_ps2_bus_t *bus, const solar_os_bus_ps2_config_t *config)
{
    portENTER_CRITICAL(&ps2_bus_lock);
    for (solar_os_ps2_bus_t *active = ps2_buses; active; active = active->next) {
        if (active == bus || active->clock_pin == config->clock_pin || active->data_pin == config->data_pin ||
            active->clock_pin == config->data_pin || active->data_pin == config->clock_pin) {
            portEXIT_CRITICAL(&ps2_bus_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (bus->lines_pending) {
        portEXIT_CRITICAL(&ps2_bus_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (ps2_next_interrupt == UINT32_MAX) {
        portEXIT_CRITICAL(&ps2_bus_lock);
        return ESP_ERR_NO_MEM;
    }
    memset(bus, 0, sizeof(*bus));
    bus->clock_pin = config->clock_pin;
    bus->data_pin = config->data_pin;
    bus->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    bus->interrupt_id = ++ps2_next_interrupt;
    bus->next = ps2_buses;
    ps2_buses = bus;
    portEXIT_CRITICAL(&ps2_bus_lock);
    return ESP_OK;
}

static esp_err_t ps2_connect(solar_os_ps2_bus_t *bus)
{
    bus->tx_semaphore = xSemaphoreCreateBinaryStatic(&bus->tx_semaphore_storage);
    if (!bus->tx_semaphore) return ESP_ERR_NO_MEM;
    /* Preload released levels before output enable. */
    bus->lines_pending = true;
    (void)gpio_set_level(bus->data_pin, 1);
    (void)gpio_set_level(bus->clock_pin, 1);
    gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) << bus->data_pin, .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    if (error != ESP_OK) return error;
    config.pin_bit_mask = UINT64_C(1) << bus->clock_pin;
    config.intr_type = GPIO_INTR_NEGEDGE;
    error = gpio_config(&config);
    if (error != ESP_OK) return error;
    (void)gpio_set_level(bus->data_pin, 1);
    (void)gpio_set_level(bus->clock_pin, 1);
    error = solar_os_gpio_isr_service_ensure();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;

    portENTER_CRITICAL(&ps2_bus_lock);
    bus->running = true;
    portEXIT_CRITICAL(&ps2_bus_lock);
    error = gpio_isr_handler_add(bus->clock_pin, ps2_clock_isr, (void *)(uintptr_t)bus->interrupt_id);
    bus->handler_attached = error == ESP_OK;
    return error;
}

esp_err_t solar_os_ps2_bus_start(solar_os_ps2_bus_t *bus, const solar_os_bus_ps2_config_t *config)
{
    if (!bus || !config || !GPIO_IS_VALID_OUTPUT_GPIO(config->clock_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->data_pin) || config->clock_pin == config->data_pin) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = ps2_reserve(bus, config);
    if (error != ESP_OK) return error;
    error = ps2_connect(bus);
    if (error != ESP_OK) (void)solar_os_ps2_bus_stop_checked(bus);
    return error;
}

esp_err_t solar_os_ps2_bus_stop_checked(solar_os_ps2_bus_t *bus)
{
    if (!bus) return ESP_OK;
    portENTER_CRITICAL(&ps2_bus_lock);
    solar_os_ps2_bus_t **slot = &ps2_buses;
    while (*slot && *slot != bus) slot = &(*slot)->next;
    if (*slot) *slot = bus->next;
    bus->running = false;
    bus->next = NULL;
    portEXIT_CRITICAL(&ps2_bus_lock);

    if (!bus->lines_pending) return ESP_OK;
    esp_err_t error = gpio_set_intr_type(bus->clock_pin, GPIO_INTR_DISABLE);
    if (error != ESP_OK) return error;
    if (bus->handler_attached) {
        error = gpio_isr_handler_remove(bus->clock_pin);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
        bus->handler_attached = false;
    }
    const esp_err_t data_result = gpio_set_level(bus->data_pin, 1);
    const esp_err_t clock_result = gpio_set_level(bus->clock_pin, 1);
    error = data_result == ESP_OK ? clock_result : data_result;
    if (error == ESP_OK) bus->lines_pending = false;
    return error;
}

void solar_os_ps2_bus_stop(solar_os_ps2_bus_t *bus)
{
    (void)solar_os_ps2_bus_stop_checked(bus);
}

size_t solar_os_ps2_bus_read(solar_os_ps2_bus_t *bus,
                             uint8_t *data,
                             size_t data_len)
{
    if (bus == NULL || data == NULL || data_len == 0) {
        return 0;
    }
    portENTER_CRITICAL(&bus->lock);
    size_t count = 0;
    while (count < data_len && bus->read_index != bus->write_index) {
        data[count++] = bus->rx_buffer[bus->read_index];
        bus->read_index = (uint8_t)((bus->read_index + 1U) % SOLAR_OS_PS2_RX_BUFFER_SIZE);
    }
    portEXIT_CRITICAL(&bus->lock);
    return count;
}

static TickType_t ps2_timeout_ticks(uint32_t milliseconds)
{
    const uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
    return (TickType_t)(ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : ticks);
}

static esp_err_t ps2_bus_write(solar_os_ps2_bus_t *bus,
                               uint8_t byte,
                               uint32_t timeout_ms)
{
    if (bus == NULL || !bus->running || timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)xSemaphoreTake(bus->tx_semaphore, 0);
    (void)gpio_set_intr_type((gpio_num_t)bus->clock_pin, GPIO_INTR_DISABLE);

    portENTER_CRITICAL(&bus->lock);
    if (bus->transmitting) {
        portEXIT_CRITICAL(&bus->lock);
        (void)gpio_set_intr_type((gpio_num_t)bus->clock_pin, GPIO_INTR_NEGEDGE);
        return ESP_ERR_INVALID_STATE;
    }
    bus->transmitting = true;
    bus->tx_ack = false;
    bus->tx_bit = 1;
    bus->tx_byte = byte;
    portEXIT_CRITICAL(&bus->lock);

    /* Host request-to-send: inhibit clock, assert start, then release clock. */
    (void)gpio_set_level((gpio_num_t)bus->clock_pin, 0);
    esp_rom_delay_us(PS2_REQUEST_TO_SEND_US);
    (void)gpio_set_level((gpio_num_t)bus->data_pin, 0);
    (void)gpio_set_intr_type((gpio_num_t)bus->clock_pin, GPIO_INTR_NEGEDGE);
    (void)gpio_set_level((gpio_num_t)bus->clock_pin, 1);

    const TickType_t timeout_ticks = ps2_timeout_ticks(timeout_ms);
    if (xSemaphoreTake(bus->tx_semaphore, timeout_ticks) == pdTRUE) {
        portENTER_CRITICAL(&bus->lock);
        const bool acknowledged = bus->tx_ack;
        portEXIT_CRITICAL(&bus->lock);
        return acknowledged ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }

    (void)gpio_set_intr_type((gpio_num_t)bus->clock_pin, GPIO_INTR_DISABLE);
    portENTER_CRITICAL(&bus->lock);
    const bool completed = !bus->transmitting;
    const bool acknowledged = bus->tx_ack;
    if (bus->transmitting) {
        bus->transmitting = false;
        bus->stats.command_errors++;
    }
    bus->last_edge_us = 0;
    ps2_frame_reset(bus);
    portEXIT_CRITICAL(&bus->lock);
    (void)gpio_set_level((gpio_num_t)bus->data_pin, 1);
    (void)gpio_set_level((gpio_num_t)bus->clock_pin, 1);
    (void)gpio_set_intr_type((gpio_num_t)bus->clock_pin, GPIO_INTR_NEGEDGE);
    return completed && acknowledged ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t ps2_bus_wait_response(solar_os_ps2_bus_t *bus,
                                       uint32_t timeout_ms,
                                       uint8_t *response)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = ps2_timeout_ticks(timeout_ms);
    do {
        if (solar_os_ps2_bus_read(bus, response, 1) == 1) {
            return ESP_OK;
        }
        vTaskDelay(1);
    } while ((xTaskGetTickCount() - start) < timeout_ticks);
    return ESP_ERR_TIMEOUT;
}

esp_err_t solar_os_ps2_bus_command(solar_os_ps2_bus_t *bus,
                                   uint8_t command,
                                   uint32_t timeout_ms)
{
    if (bus == NULL || timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t attempt = 0; attempt < PS2_COMMAND_RETRIES; attempt++) {
        esp_err_t err = ps2_bus_write(bus, command, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        uint8_t response = 0;
        err = ps2_bus_wait_response(bus, timeout_ms, &response);
        if (err != ESP_OK) {
            return err;
        }
        if (response == PS2_PROTOCOL_ACK) {
            return ESP_OK;
        }
        if (response != PS2_PROTOCOL_RESEND) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

void solar_os_ps2_bus_get_stats(solar_os_ps2_bus_t *bus,
                                solar_os_ps2_bus_stats_t *stats)
{
    if (bus == NULL || stats == NULL) {
        return;
    }
    portENTER_CRITICAL(&bus->lock);
    *stats = bus->stats;
    portEXIT_CRITICAL(&bus->lock);
}
