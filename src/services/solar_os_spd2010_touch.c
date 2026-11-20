#include "spd2010_touch.h"
#include "solar_os_buses.h"
#include "solar_os_display.h"
#include "solar_os_expansion.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_input.h"
#include "solar_os_task.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

typedef struct {
    bool active, pressed, have_irq, have_power, have_reset, power_active_high;
    volatile bool stop_requested, worker_done;
    char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX], bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t rotation;
    uint16_t width, height;
    int16_t x, y;
    solar_os_gpio_line_ref_t irq, power, reset;
    solar_os_input_source_t source;
    TaskHandle_t worker;
    solar_os_spd2010_touch_io_t io;
} touch_device_t;
static touch_device_t touch;

static esp_err_t write_data(void *ctx, const uint8_t *data, size_t len)
{
    touch_device_t *d = ctx;
    return solar_os_bus_i2c_transmit(d->bus, SOLAR_OS_SPD2010_TOUCH_ADDRESS, data, len);
}
static esp_err_t read_data(void *ctx, uint8_t *data, size_t len)
{
    touch_device_t *d = ctx;
    return solar_os_bus_i2c_receive(d->bus, SOLAR_OS_SPD2010_TOUCH_ADDRESS, data, len);
}
static void delay_us(void *ctx, uint32_t us) { (void)ctx; esp_rom_delay_us(us); }
static esp_err_t control(const solar_os_gpio_line_ref_t *line, bool level)
{
    esp_err_t error = solar_os_gpio_line_write(line, level);
    if (!error) error = solar_os_gpio_line_configure(line,
        SOLAR_OS_GPIO_LINE_MODE_OUTPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
    return error;
}

static esp_err_t parse(const solar_os_expansion_binding_t *bindings, size_t count, touch_device_t *d)
{
    bool have_bus = false, have_addr = false;
    const char *address_bus = NULL;
    d->power_active_high = true;
    for (size_t i = 0; bindings && i < count; ++i) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        for (size_t j = 0; j < i; ++j)
            if (b->kind == bindings[j].kind && !strcmp(b->role, bindings[j].role)) return ESP_ERR_INVALID_ARG;
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_bus) {
            if (strlcpy(d->bus, b->target, sizeof(d->bus)) >= sizeof(d->bus)) return ESP_ERR_INVALID_ARG;
            have_bus = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS && !have_addr && b->value == SOLAR_OS_SPD2010_TOUCH_ADDRESS) {
            address_bus = b->target; have_addr = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE) {
            solar_os_gpio_line_ref_t *line;
            if (!strcmp(b->role, "irq")) { d->have_irq = true; line = &d->irq; }
            else if (!strcmp(b->role, "reset")) { d->have_reset = true; line = &d->reset; }
            else if (!strcmp(b->role, "power")) { d->have_power = true; line = &d->power; }
            else return ESP_ERR_INVALID_ARG;
            if (b->value < 0 || b->value > UINT8_MAX ||
                strlcpy(line->controller, b->target, sizeof(line->controller)) >= sizeof(line->controller)) return ESP_ERR_INVALID_ARG;
            line->line = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !strcmp(b->role, "rotation") && b->value >= 0 && b->value <= 3) {
            d->rotation = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !strcmp(b->role, "active") && (b->value == 0 || b->value == 1)) {
            d->power_active_high = b->value;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (!have_bus || !have_addr || (address_bus[0] && strcmp(address_bus, d->bus))) return ESP_ERR_INVALID_ARG;
    const solar_os_gpio_line_ref_t *lines[] = {&d->irq, &d->reset, &d->power};
    const bool used[] = {d->have_irq, d->have_reset, d->have_power};
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = i + 1; j < 3; ++j)
            if (used[i] && used[j] && lines[i]->line == lines[j]->line &&
                !strcmp(lines[i]->controller, lines[j]->controller)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static void emit(touch_device_t *d, const solar_os_spd2010_touch_sample_t *s)
{
    if (!s->valid) return;
    int16_t x = d->x, y = d->y;
    if (s->touched) {
        if (s->x >= d->width || s->y >= d->height) return;
        switch (d->rotation) {
        case 0: x = s->x; y = s->y; break;
        case 1: x = s->y; y = d->width - 1U - s->x; break;
        case 2: x = d->width - 1U - s->x; y = d->height - 1U - s->y; break;
        default: x = d->height - 1U - s->y; y = s->x; break;
        }
    }
    solar_os_input_pointer_action_t action;
    if (s->touched && !d->pressed) action = SOLAR_OS_INPUT_POINTER_PRESS;
    else if (!s->touched && d->pressed) action = SOLAR_OS_INPUT_POINTER_RELEASE;
    else if (s->touched && (x != d->x || y != d->y)) action = SOLAR_OS_INPUT_POINTER_MOVE;
    else return;
    /* One stable logical pointer follows the first active contact. */
    solar_os_input_pointer_event_t event = {.pointer_id = 0,
        .buttons = s->touched ? SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY : 0,
        .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE, .action = action, .x = x, .y = y,
        .delta_x = x - d->x, .delta_y = y - d->y};
    strlcpy(event.target, SOLAR_OS_DISPLAY_PRIMARY_TARGET, sizeof(event.target));
    if (solar_os_input_write_pointer(d->source, &event) == ESP_OK) {
        d->pressed = s->touched; d->x = x; d->y = y;
    }
}

static void worker(void *arg)
{
    touch_device_t *d = arg;
    while (!d->stop_requested) {
        bool irq_high = false;
        esp_err_t error = d->have_irq ? solar_os_gpio_line_read(&d->irq, &irq_high) : ESP_OK;
        if (!error && !irq_high) {
            solar_os_spd2010_touch_sample_t sample;
            if (solar_os_spd2010_touch_read(&d->io, &sample) == ESP_OK) emit(d, &sample);
        }
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
    }
    d->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear(void)
{
    if (touch.source != SOLAR_OS_INPUT_SOURCE_INVALID) solar_os_input_source_close(touch.source);
    if (touch.have_power) (void)control(&touch.power, !touch.power_active_high);
    memset(&touch, 0, sizeof(touch));
}

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (touch.active) return ESP_ERR_INVALID_STATE;
    if (!name || !name[0] || strnlen(name, sizeof(touch.name)) >= sizeof(touch.name)) return ESP_ERR_INVALID_ARG;
    touch_device_t candidate = {0};
    esp_err_t error = parse(bindings, count, &candidate);
    if (error) return error;
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(SOLAR_OS_DISPLAY_PRIMARY_TARGET, &target)) return ESP_ERR_NOT_FOUND;
    candidate.width = target.native_width ? target.native_width : target.width;
    candidate.height = target.native_height ? target.native_height : target.height;
    if (!candidate.width || !candidate.height || candidate.width > INT16_MAX || candidate.height > INT16_MAX)
        return ESP_ERR_INVALID_SIZE;
    strlcpy(candidate.name, name, sizeof(candidate.name));
    touch = candidate;
    touch.io = (solar_os_spd2010_touch_io_t){.read = read_data, .write = write_data, .delay_us = delay_us, .ctx = &touch};
    if (touch.have_power) {
        error = control(&touch.power, touch.power_active_high);
        if (!error) vTaskDelay(pdMS_TO_TICKS(20) + 1);
    }
    if (!error && touch.have_reset) {
        error = control(&touch.reset, false);
        vTaskDelay(pdMS_TO_TICKS(2) + 1);
        if (!error) error = control(&touch.reset, true);
        vTaskDelay(pdMS_TO_TICKS(22) + 1);
    }
    if (!error && touch.have_irq) error = solar_os_gpio_line_configure(&touch.irq,
        SOLAR_OS_GPIO_LINE_MODE_INPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
    if (!error) error = solar_os_bus_i2c_probe(touch.bus, SOLAR_OS_SPD2010_TOUCH_ADDRESS);
    if (!error) error = solar_os_spd2010_touch_probe(&touch.io);
    if (!error) error = solar_os_input_touch_source_open(name, &touch.source);
    if (error) { clear(); return error; }
    touch.active = true;
    if (solar_os_task_create_pinned_internal(worker, name, 3072, &touch, tskIDLE_PRIORITY + 1,
        &touch.worker, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear(); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t detach(const char *name)
{
    if (!touch.active || !name || strcmp(name, touch.name)) return ESP_ERR_NOT_FOUND;
    touch.stop_requested = true;
    if (!solar_os_task_wait_done(touch.worker, &touch.worker_done, SOLAR_OS_TASK_STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
    if (touch.have_power) {
        esp_err_t error = control(&touch.power, !touch.power_active_high);
        if (error) return error;
        touch.have_power = false;
    }
    const solar_os_spd2010_touch_sample_t released = {.valid = true};
    emit(&touch, &released);
    clear(); return ESP_OK;
}

static const int address[] = {SOLAR_OS_SPD2010_TOUCH_ADDRESS};
#define LINE(key_) {.key = key_, .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = key_}
static const solar_os_expansion_binding_spec_t specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "0x53", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = address, .allowed_value_count = 1},
    LINE("irq"), LINE("reset"), LINE("power"),
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .has_value_range = true, .min_value = 0, .max_value = 3},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .has_value_range = true, .min_value = 0, .max_value = 1},
};
#undef LINE
const solar_os_expansion_driver_t solar_os_spd2010_touch_expansion_driver = {
    .name = "spd2010-touch", .category = SOLAR_OS_EXPANSION_CATEGORY_INPUT,
    .summary = "SPD2010 capacitive touch over I2C",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C | SOLAR_OS_BOARD_CAP_GFX,
    .probe_supported = true, .binding_specs = specs, .binding_spec_count = sizeof(specs) / sizeof(specs[0]),
    .attach = attach, .detach = detach,
};
