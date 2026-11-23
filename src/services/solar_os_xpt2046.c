#include "xpt2046.h"

#include <string.h>
#include <stdatomic.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_display.h"
#include "solar_os_expansion.h"
#include "solar_os_input.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

typedef struct xpt_device {
    struct xpt_device *next;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX], target[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs, irq;
    uint32_t clock_hz;
    uint16_t pressure;
    bool swap_xy, invert_x, invert_y, pressed;
    int16_t x, y;
    unsigned failures;
    volatile bool stop_requested, worker_done;
    spi_device_handle_t spi;
    solar_os_xpt2046_io_t io;
    solar_os_input_source_t source;
    TaskHandle_t worker;
} xpt_device_t;

static xpt_device_t *devices;
static atomic_flag xpt_operation_running = ATOMIC_FLAG_INIT;

static esp_err_t xpt_transfer(void *context, const uint8_t tx[3], uint8_t rx[3])
{
    xpt_device_t *d = context;
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA, .length = 24U,
    };
    memcpy(transaction.tx_data, tx, 3U);
    esp_err_t err = spi_device_polling_transmit(d->spi, &transaction);
    if (err == ESP_OK) memcpy(rx, transaction.rx_data, 3U);
    return err;
}

static esp_err_t xpt_parse(const solar_os_expansion_binding_t *bindings, size_t count, xpt_device_t *d)
{
    if (!bindings) return ESP_ERR_INVALID_ARG;
    d->cs = d->irq = -1; d->clock_hz = 2000000U;
    d->pressure = SOLAR_OS_XPT2046_PRESSURE_DEFAULT;
    const char *cs_bus = NULL;
    for (size_t i = 0; i < count; ++i) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        for (size_t j = 0; j < i; ++j)
            if (b->kind == bindings[j].kind && !strcmp(b->role, bindings[j].role)) return ESP_ERR_INVALID_ARG;
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) {
            if (strlcpy(d->bus, b->target, sizeof(d->bus)) >= sizeof(d->bus)) return ESP_ERR_INVALID_ARG;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS && GPIO_IS_VALID_OUTPUT_GPIO(b->value)) {
            d->cs = b->value; cs_bus = b->target;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && !strcmp(b->role, "irq") && GPIO_IS_VALID_GPIO(b->value)) {
            d->irq = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET && !strcmp(b->role, "display")) {
            if (strlcpy(d->target, b->target, sizeof(d->target)) >= sizeof(d->target)) return ESP_ERR_INVALID_ARG;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !strcmp(b->role, "clock") && b->value >= 100 && b->value <= 2000) {
            d->clock_hz = (uint32_t)b->value * 1000U;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !strcmp(b->role, "pressure") && b->value >= 1 && b->value <= 4095) {
            d->pressure = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && (b->value == 0 || b->value == 1)) {
            if (!strcmp(b->role, "swap_xy")) d->swap_xy = b->value;
            else if (!strcmp(b->role, "invert_x")) d->invert_x = b->value;
            else if (!strcmp(b->role, "invert_y")) d->invert_y = b->value;
            else return ESP_ERR_INVALID_ARG;
        } else return ESP_ERR_INVALID_ARG;
    }
    return d->bus[0] && d->target[0] && d->cs >= 0 && d->cs != d->irq &&
        (!cs_bus || !cs_bus[0] || !strcmp(cs_bus, d->bus)) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t xpt_emit(xpt_device_t *d, const solar_os_xpt2046_sample_t *sample)
{
    if (!sample->valid) return ESP_OK;
    int16_t x = d->x, y = d->y;
    if (sample->touched) {
        x = d->swap_xy ? sample->y : sample->x;
        y = d->swap_xy ? sample->x : sample->y;
        if (d->invert_x) x = 4095 - x;
        if (d->invert_y) y = 4095 - y;
    }
    solar_os_input_pointer_action_t action;
    if (sample->touched && !d->pressed) action = SOLAR_OS_INPUT_POINTER_PRESS;
    else if (!sample->touched && d->pressed) action = SOLAR_OS_INPUT_POINTER_RELEASE;
    else if (sample->touched && (x != d->x || y != d->y)) action = SOLAR_OS_INPUT_POINTER_MOVE;
    else return ESP_OK;
    solar_os_input_pointer_event_t event = {
        .pointer_id = 0, .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .action = action,
        .buttons = sample->touched ? SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY : 0,
        .x = x, .y = y, .delta_x = x - d->x, .delta_y = y - d->y,
    };
    strlcpy(event.target, d->target, sizeof(event.target));
    esp_err_t err = solar_os_input_write_pointer(d->source, &event);
    if (err == ESP_OK) { d->pressed = sample->touched; d->x = x; d->y = y; }
    return err;
}

static void xpt_poll(xpt_device_t *d)
{
    const solar_os_xpt2046_sample_t released = {.valid = true};
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(d->target, &target) || !target.ready ||
        (d->irq >= 0 && d->failures == 0U && gpio_get_level(d->irq))) {
        (void)xpt_emit(d, &released);
        return;
    }
    solar_os_xpt2046_sample_t sample;
    /* ESP-IDF rejects finite waits here. Detach keeps the device alive if its
     * bounded task join expires while another SPI device owns the bus. */
    esp_err_t err = spi_device_acquire_bus(d->spi, portMAX_DELAY);
    if (err == ESP_OK) {
        if (d->stop_requested) {
            spi_device_release_bus(d->spi);
            return;
        }
        err = solar_os_xpt2046_read(&d->io, d->pressure, &sample);
        spi_device_release_bus(d->spi);
    }
    if (err == ESP_OK) {
        d->failures = 0;
        (void)xpt_emit(d, &sample);
    } else {
        if (d->failures < 3U) ++d->failures;
        if (d->failures == 3U) (void)xpt_emit(d, &released);
    }
}

static void xpt_worker(void *context)
{
    xpt_device_t *d = context;
    while (!d->stop_requested) {
        xpt_poll(d);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10) + 1U);
    }
    d->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static esp_err_t xpt_attach_locked(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!name || !name[0] || strnlen(name, SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX)
        return ESP_ERR_INVALID_ARG;
    for (xpt_device_t *d = devices; d; d = d->next)
        if (!strcmp(d->name, name)) return ESP_ERR_INVALID_STATE;
    xpt_device_t *d = solar_os_memory_calloc(1, sizeof(*d), SOLAR_OS_MEMORY_TRANSIENT, "xpt2046.device");
    if (!d) return ESP_ERR_NO_MEM;
    esp_err_t err = xpt_parse(bindings, count, d);
    solar_os_display_target_t target;
    solar_os_bus_info_t bus;
    if (err == ESP_OK && (!solar_os_bus_find(d->bus, SOLAR_OS_BUS_PROTOCOL_SPI, &bus) ||
        bus.config.spi.miso_pin < 0)) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && (!solar_os_display_find_target(d->target, &target) || !target.ready)) err = ESP_ERR_NOT_FOUND;
    solar_os_input_pointer_calibration_t fallback = {.min_x = 0, .max_x = 4095, .min_y = 0, .max_y = 4095};
    if (err == ESP_OK) {
        fallback.width = target.native_width ? target.native_width : target.width;
        fallback.height = target.native_height ? target.native_height : target.height;
        if (!fallback.width || !fallback.height || fallback.width > INT16_MAX || fallback.height > INT16_MAX)
            err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && d->irq >= 0) {
        const gpio_config_t irq = {.pin_bit_mask = 1ULL << (unsigned)d->irq,
            .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
        err = gpio_config(&irq);
    }
    if (err == ESP_OK) {
        const spi_device_interface_config_t config = {.clock_speed_hz = (int)d->clock_hz,
            .mode = 0, .spics_io_num = d->cs, .queue_size = 1};
        err = solar_os_bus_spi_add_device(d->bus, &config, &d->spi);
    }
    d->io = (solar_os_xpt2046_io_t){.transfer = xpt_transfer, .context = d};
    if (err == ESP_OK) err = solar_os_xpt2046_idle(&d->io);
    if (err == ESP_OK) err = solar_os_input_touch_source_open(name, &d->source);
    if (err == ESP_OK) err = solar_os_input_pointer_calibration_default(d->source, &fallback);
    if (err == ESP_OK) {
        strlcpy(d->name, name, sizeof(d->name));
        if (solar_os_task_create_pinned_internal(xpt_worker, d->name, 3072U, d, tskIDLE_PRIORITY + 1,
            &d->worker, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        if (d->source != SOLAR_OS_INPUT_SOURCE_INVALID) solar_os_input_source_close(d->source);
        if (d->spi) (void)spi_bus_remove_device(d->spi);
        solar_os_memory_free(d);
        return err;
    }
    d->next = devices; devices = d;
    return ESP_OK;
}

static esp_err_t xpt_detach_locked(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    xpt_device_t **link = &devices;
    while (*link && strcmp((*link)->name, name)) link = &(*link)->next;
    xpt_device_t *d = *link;
    if (!d) return ESP_ERR_NOT_FOUND;
    d->stop_requested = true;
    if (!solar_os_task_wait_done(d->worker, &d->worker_done, SOLAR_OS_TASK_STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
    const solar_os_xpt2046_sample_t released = {.valid = true};
    esp_err_t err = xpt_emit(d, &released);
    if (err == ESP_OK) err = spi_bus_remove_device(d->spi);
    if (err != ESP_OK) return err;
    solar_os_input_source_close(d->source);
    *link = d->next; solar_os_memory_free(d);
    return ESP_OK;
}

static esp_err_t xpt_attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (atomic_flag_test_and_set(&xpt_operation_running)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = xpt_attach_locked(name, bindings, count);
    atomic_flag_clear(&xpt_operation_running);
    return err;
}

static esp_err_t xpt_detach(const char *name)
{
    if (atomic_flag_test_and_set(&xpt_operation_running)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = xpt_detach_locked(name);
    atomic_flag_clear(&xpt_operation_running);
    return err;
}

#define PARAM(key_, lo_, hi_) {.key = key_, .value_hint = #lo_ ".." #hi_, .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = key_, .has_value_range = true, .min_value = lo_, .max_value = hi_}
static const solar_os_expansion_binding_spec_t specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "display", .value_hint = "target", .kind = SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET, .role = "display", .required = true},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    PARAM("clock", 100, 2000), PARAM("pressure", 1, 4095),
    PARAM("swap_xy", 0, 1), PARAM("invert_x", 0, 1), PARAM("invert_y", 0, 1),
};
#undef PARAM

const solar_os_expansion_driver_t solar_os_xpt2046_expansion_driver = {
    .name = "xpt2046", .summary = "SPI resistive touch controller",
    .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI | SOLAR_OS_BOARD_CAP_EXPANSION_GPIO | SOLAR_OS_BOARD_CAP_GFX,
    .probe_supported = false, .binding_specs = specs, .binding_spec_count = sizeof(specs) / sizeof(specs[0]),
    .attach = xpt_attach, .detach = xpt_detach,
};
