#include "ps2_bus.h"

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

static void ps2_clock_isr(void *arg)
{
    solar_os_ps2_bus_t *bus = arg;
    if (bus == NULL || !bus->running) {
        return;
    }

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
            if (bus->tx_semaphore != NULL) {
                BaseType_t task_woken = pdFALSE;
                xSemaphoreGiveFromISR(bus->tx_semaphore, &task_woken);
                if (task_woken == pdTRUE) {
                    portYIELD_FROM_ISR();
                }
            }
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

esp_err_t solar_os_ps2_bus_start(solar_os_ps2_bus_t *bus,
                                 const solar_os_bus_ps2_config_t *config)
{
    if (bus == NULL || config == NULL || config->clock_pin < 0 ||
        config->data_pin < 0 || config->clock_pin == config->data_pin) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(bus, 0, sizeof(*bus));
    bus->clock_pin = config->clock_pin;
    bus->data_pin = config->data_pin;
    bus->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    bus->tx_semaphore = xSemaphoreCreateBinaryStatic(
        &bus->tx_semaphore_storage);
    if (bus->tx_semaphore == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Preload released levels before output enable to avoid a low glitch. */
    (void)gpio_set_level((gpio_num_t)config->data_pin, 1);
    (void)gpio_set_level((gpio_num_t)config->clock_pin, 1);

    const gpio_config_t data_config = {
        .pin_bit_mask = 1ULL << (unsigned)config->data_pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&data_config);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t clock_config = {
        .pin_bit_mask = 1ULL << (unsigned)config->clock_pin,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&clock_config);
    if (err != ESP_OK) {
        return err;
    }
    (void)gpio_set_level((gpio_num_t)config->data_pin, 1);
    (void)gpio_set_level((gpio_num_t)config->clock_pin, 1);
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)gpio_set_intr_type((gpio_num_t)config->clock_pin, GPIO_INTR_DISABLE);
        return err;
    }
    bus->running = true;
    err = gpio_isr_handler_add((gpio_num_t)config->clock_pin, ps2_clock_isr, bus);
    if (err != ESP_OK) {
        bus->running = false;
        (void)gpio_set_intr_type((gpio_num_t)config->clock_pin, GPIO_INTR_DISABLE);
        return err;
    }
    return ESP_OK;
}

void solar_os_ps2_bus_stop(solar_os_ps2_bus_t *bus)
{
    if (bus == NULL || !bus->running) {
        return;
    }
    bus->running = false;
    (void)gpio_set_intr_type((gpio_num_t)bus->clock_pin, GPIO_INTR_DISABLE);
    (void)gpio_isr_handler_remove((gpio_num_t)bus->clock_pin);
    (void)gpio_set_level((gpio_num_t)bus->data_pin, 1);
    (void)gpio_set_level((gpio_num_t)bus->clock_pin, 1);
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

    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms) > 0
        ? pdMS_TO_TICKS(timeout_ms)
        : 1;
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
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms) > 0
        ? pdMS_TO_TICKS(timeout_ms)
        : 1;
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
