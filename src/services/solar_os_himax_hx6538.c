#include "solar_os_himax_hx6538.h"

#include <stdio.h>
#include <string.h>
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "himax_hx6538.h"
#include "solar_os_buses.h"
#include "solar_os_camera.h"
#include "solar_os_camera_stream.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"
#include "solar_os_sscma.h"

typedef struct {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_BUS_NAME_MAX];
    spi_device_handle_t spi;
    uint32_t speed_hz;
    size_t transfer_max;
    int cs;
    bool have_ready, have_reset, have_power, ready, started;
    solar_os_gpio_line_ref_t ready_line, reset, power;
    solar_os_himax_t camera;
    uint8_t packet[256];
    uint8_t *maintenance;
} himax_device_t;

static himax_device_t *device;
static const char *TAG = "himax-hx6538";
static const solar_os_camera_config_t default_config = {
    .width = 240, .height = 240, .jpeg_quality = 0,
};

static void wait_ms(void *ctx, uint32_t ms)
{ (void)ctx; vTaskDelay(pdMS_TO_TICKS(ms) + 1); }
static uint64_t now_ms(void *ctx)
{ (void)ctx; return (uint64_t)esp_timer_get_time() / 1000U; }
static void *resize(void *ctx, void *data, size_t size)
{
    (void)ctx;
    if (!size) { solar_os_memory_free(data); return NULL; }
    return solar_os_memory_realloc(data, size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "himax.reply");
}

static esp_err_t control(const solar_os_gpio_line_ref_t *line, bool level)
{
    esp_err_t error = solar_os_gpio_line_write(line, level);
    if (error == ESP_OK) error = solar_os_gpio_line_configure(line,
        SOLAR_OS_GPIO_LINE_MODE_OUTPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
    return error;
}

static esp_err_t transfer(himax_device_t *d, const uint8_t *tx, uint8_t *rx, size_t length)
{
    spi_transaction_t transaction = {.length = length * 8U, .tx_buffer = tx, .rx_buffer = rx};
    wait_ms(d, 2);
    return spi_device_polling_transmit(d->spi, &transaction);
}

static esp_err_t packet(himax_device_t *d, uint8_t operation, const uint8_t *data, size_t length)
{
    memset(d->packet, 0, sizeof(d->packet));
    d->packet[0] = 0x10; d->packet[1] = operation;
    d->packet[2] = (uint8_t)(length >> 8U); d->packet[3] = (uint8_t)length;
    if (data) memcpy(d->packet + 4, data, length);
    const size_t tail = data ? 4U + length : 4U;
    d->packet[tail] = d->packet[tail + 1] = 0xff;
    return transfer(d, d->packet, NULL, sizeof(d->packet));
}

static esp_err_t write_bytes(void *ctx, const uint8_t *data, size_t length)
{
    himax_device_t *d = ctx;
    esp_err_t error = spi_device_acquire_bus(d->spi, portMAX_DELAY);
    if (error != ESP_OK) return error;
    while (length && error == ESP_OK) {
        const size_t chunk = length > 250 ? 250 : length;
        error = packet(d, 2, data, chunk);
        data += chunk; length -= chunk;
    }
    spi_device_release_bus(d->spi);
    return error;
}

static esp_err_t read_bytes(void *ctx, uint8_t *data, size_t capacity, size_t *length)
{
    himax_device_t *d = ctx;
    *length = 0;
    if (d->have_ready) {
        bool ready;
        esp_err_t error = solar_os_gpio_line_read(&d->ready_line, &ready);
        if (error != ESP_OK || !ready) return error;
    }
    esp_err_t error = spi_device_acquire_bus(d->spi, portMAX_DELAY);
    if (error != ESP_OK) return error;
    uint8_t available[2];
    error = packet(d, 3, NULL, 0);
    if (error == ESP_OK) error = transfer(d, NULL, available, sizeof(available));
    if (error == ESP_OK) {
        size_t bytes = ((size_t)available[0] << 8U) | available[1];
        if (bytes == 0xffff) bytes = 0;
        if (bytes > capacity) bytes = capacity;
        if (bytes > d->transfer_max) bytes = d->transfer_max;
        if (bytes) {
            error = packet(d, 1, NULL, bytes);
            if (error == ESP_OK) error = transfer(d, NULL, data, bytes);
            if (error == ESP_OK) *length = bytes;
        }
    }
    spi_device_release_bus(d->spi);
    return error;
}

static esp_err_t prepare(himax_device_t *d)
{
    if (d->ready) return ESP_OK;
    solar_os_memory_free(d->maintenance); d->maintenance = NULL;
    solar_os_himax_clear(&d->camera);
    esp_err_t error = d->have_power ? control(&d->power, true) : ESP_OK;
    if (error == ESP_OK && d->have_ready)
        error = solar_os_gpio_line_configure(&d->ready_line,
            SOLAR_OS_GPIO_LINE_MODE_INPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
    if (error == ESP_OK && d->have_reset) {
        error = control(&d->reset, false);
        wait_ms(d, 100);
        if (error == ESP_OK) error = control(&d->reset, true);
    }
    if (error == ESP_OK) {
        wait_ms(d, 500);
        error = solar_os_himax_probe(&d->camera, SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS);
    }
    if (error == ESP_OK) {
        d->ready = true;
        ESP_LOGI(TAG, "factory firmware=%s sensor=%u modes=%u", d->camera.version,
                 d->camera.sensor_id, (unsigned)d->camera.mode_count);
    } else {
        solar_os_himax_clear(&d->camera);
    }
    return error;
}

static esp_err_t camera_start(void *ctx, const solar_os_camera_config_t *config,
    solar_os_camera_orientation_t orientation, solar_os_camera_sensor_info_t *sensor)
{
    himax_device_t *d = ctx;
    if (orientation != SOLAR_OS_CAMERA_ORIENTATION_NORMAL || config->jpeg_quality != 0)
        return ESP_ERR_NOT_SUPPORTED;
    esp_err_t error = prepare(d);
    if (error != ESP_OK) return error;
    const solar_os_himax_mode_t *mode = NULL;
    for (size_t i = 0; i < d->camera.mode_count; i++)
        if (d->camera.modes[i].width == config->width && d->camera.modes[i].height == config->height)
            mode = &d->camera.modes[i];
    if (!mode) return ESP_ERR_NOT_SUPPORTED;
    error = solar_os_himax_command(&d->camera, "AT+BREAK\r\n", "BREAK", SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS);
    char command[48];
    snprintf(command, sizeof(command), "AT+SENSOR=%u,1,%u\r\n", d->camera.sensor_id, mode->option);
    if (error == ESP_OK) error = solar_os_himax_command(&d->camera, command, "SENSOR", SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS);
    if (error != ESP_OK) {
        d->ready = false;
        solar_os_himax_clear(&d->camera);
        return error;
    }
    d->started = true;
    *sensor = (solar_os_camera_sensor_info_t){.frame_buffer_count = 1, .jpeg_quality_fixed = true};
    strlcpy(sensor->name, "Himax camera", sizeof(sensor->name));
    return ESP_OK;
}

static esp_err_t camera_stop(void *ctx)
{
    himax_device_t *d = ctx;
    solar_os_memory_free(d->maintenance); d->maintenance = NULL;
    if (!d->started) return ESP_OK;
    esp_err_t error = solar_os_himax_command(&d->camera, "AT+BREAK\r\n", "BREAK", SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS);
    char command[48];
    snprintf(command, sizeof(command), "AT+SENSOR=%u,0,0\r\n", d->camera.sensor_id);
    if (error == ESP_OK) error = solar_os_himax_command(&d->camera, command, "SENSOR", SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS);
    if (error == ESP_OK) { d->started = false; solar_os_himax_clear(&d->camera); }
    return error;
}

static esp_err_t camera_capture(void *ctx, solar_os_camera_backend_frame_t *frame)
{
    himax_device_t *d = ctx;
    if (!d->started) return ESP_ERR_INVALID_STATE;
    const uint8_t *jpeg; size_t length; uint16_t width, height;
    esp_err_t error = solar_os_himax_sample(&d->camera, SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS,
                                            &jpeg, &length, &width, &height);
    if (error != ESP_OK) { solar_os_himax_clear(&d->camera); return error; }
    *frame = (solar_os_camera_backend_frame_t){.data = jpeg, .length = length,
        .width = width, .height = height, .timestamp_us = esp_timer_get_time(), .release_token = d};
    return ESP_OK;
}

static void camera_release(void *ctx, void *token)
{ (void)token; solar_os_himax_clear(&((himax_device_t *)ctx)->camera); }

static void camera_invalidate(void *ctx)
{
    himax_device_t *d = ctx;
    solar_os_memory_free(d->maintenance); d->maintenance = NULL;
    d->ready = d->started = false;
    solar_os_himax_clear(&d->camera);
}

static esp_err_t maintenance_control(himax_device_t *d, const solar_os_sscma_spi_t *r)
{
    if (!d->have_reset || d->transfer_max < 262) return ESP_ERR_NOT_SUPPORTED;
    if (r->reset) { camera_invalidate(d); return prepare(d); }
    if (r->length > 262 || (r->length && !r->tx && !r->rx)) return ESP_ERR_INVALID_ARG;
    if (!r->length) return ESP_OK;
    if (d->started) return ESP_ERR_INVALID_STATE;
    if (!d->maintenance) d->maintenance = solar_os_memory_alloc(528,
        SOLAR_OS_MEMORY_DMA, "himax.spi");
    if (!d->maintenance) return ESP_ERR_NO_MEM;
    if (r->tx) memcpy(d->maintenance, r->tx, r->length);
    d->ready = false; /* Any later JSON operation must reset/probe first. */
    esp_err_t error = spi_device_acquire_bus(d->spi, portMAX_DELAY);
    if (!error) {
        spi_transaction_t transaction = {.length = r->length * 8,
            .tx_buffer = r->tx ? d->maintenance : NULL,
            .rx_buffer = r->rx ? d->maintenance + 264 : NULL};
        error = spi_device_polling_transmit(d->spi, &transaction);
        spi_device_release_bus(d->spi);
    }
    if (!error && r->rx) memcpy(r->rx, d->maintenance + 264, r->length);
    return error;
}

static esp_err_t camera_control(void *ctx, const char *protocol, const void *request)
{
    himax_device_t *d = ctx;
    if (!strcmp(protocol, SOLAR_OS_SSCMA_SPI_PROTOCOL)) return maintenance_control(d, request);
    const solar_os_sscma_exchange_t *r = request;
    if (strcmp(protocol, SOLAR_OS_NATIVE_SSCMA_SERVICE)) return ESP_ERR_NOT_SUPPORTED;
    if (!r->source || strcmp(r->source, d->name)) return ESP_ERR_NOT_FOUND;
    if (!r->command) return ESP_OK; /* Ownership/name probe; no hardware access. */
    if (strlen(r->command) > 4095) return ESP_ERR_INVALID_ARG;
    if (!r->response_name) {
        esp_err_t error = prepare(d);
        return error ? error : write_bytes(d, (const uint8_t *)r->command, strlen(r->command));
    }
    if (!r->reply || !r->timeout_ms || r->timeout_ms > 60000 || strlen(r->response_name) >= 32) return ESP_ERR_INVALID_ARG;
    *r->reply = NULL;
    if (r->cancel && r->cancel(r->user)) return ESP_ERR_TIMEOUT;
    esp_err_t error = prepare(d);
    if (!error) error = solar_os_himax_exchange(&d->camera, r->command,
        r->response_name, r->event, r->timeout_ms, r->cancel, r->user);
    if (!error) {
        *r->reply = solar_os_memory_alloc(d->camera.reply_used + 1,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "sscma.reply");
        if (!*r->reply) error = ESP_ERR_NO_MEM;
        else memcpy(*r->reply, d->camera.reply, d->camera.reply_used + 1);
    }
    solar_os_himax_clear(&d->camera);
    return error;
}

static esp_err_t camera_resolutions(void *ctx, solar_os_camera_resolution_t *out,
                                     size_t capacity, size_t *count)
{
    himax_device_t *d = ctx;
    esp_err_t error = prepare(d);
    if (error != ESP_OK) return error;
    *count = d->camera.mode_count;
    for (size_t i = 0; i < capacity && i < *count; i++)
        out[i] = (solar_os_camera_resolution_t){d->camera.modes[i].width, d->camera.modes[i].height};
    return ESP_OK;
}

static esp_err_t publish(void *ctx)
{ return solar_os_camera_stream_register_config(((himax_device_t *)ctx)->name, &default_config); }
static esp_err_t unpublish(void *ctx)
{
    himax_device_t *d = ctx;
    esp_err_t error = solar_os_camera_stream_unregister(d->name);
    if (error != ESP_OK) return error;
    error = spi_bus_remove_device(d->spi);
    if (error != ESP_OK) { (void)publish(d); return error; }
    d->spi = NULL;
    return ESP_OK;
}

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (device) return ESP_ERR_INVALID_STATE;
    if (!name || !name[0] || strlen(name) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX || !bindings)
        return ESP_ERR_INVALID_ARG;
    himax_device_t *d = solar_os_memory_calloc(1, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "himax.device");
    if (!d) return ESP_ERR_NO_MEM;
    d->cs = -1; d->speed_hz = 12000000U;
    strlcpy(d->name, name, sizeof(d->name));
    esp_err_t error = ESP_OK;
    for (size_t i = 0; i < count && error == ESP_OK; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        for (size_t j = 0; j < i; j++)
            if (b->kind == bindings[j].kind && !strcmp(b->role, bindings[j].role)) error = ESP_ERR_INVALID_ARG;
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) strlcpy(d->bus, b->target, sizeof(d->bus));
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS) d->cs = b->value;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !strcmp(b->role, "clock") && b->value >= 100 && b->value <= 12000)
            d->speed_hz = b->value * 1000U;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE && b->value >= 0 && b->value <= UINT8_MAX) {
            solar_os_gpio_line_ref_t *line = NULL;
            if (!strcmp(b->role, "ready")) { d->have_ready = true; line = &d->ready_line; }
            else if (!strcmp(b->role, "reset")) { d->have_reset = true; line = &d->reset; }
            else if (!strcmp(b->role, "power")) { d->have_power = true; line = &d->power; }
            if (!line) { error = ESP_ERR_INVALID_ARG; continue; }
            strlcpy(line->controller, b->target, sizeof(line->controller)); line->line = b->value;
        } else error = ESP_ERR_INVALID_ARG;
    }
    solar_os_bus_info_t bus;
    if (error == ESP_OK && (!d->bus[0] || d->cs < 0 ||
        !solar_os_bus_find(d->bus, SOLAR_OS_BUS_PROTOCOL_SPI, &bus) ||
        !solar_os_expansion_spi_cs_allowed(d->bus, d->cs) || bus.config.spi.max_transfer_size < 256))
        error = ESP_ERR_INVALID_ARG;
    if (error == ESP_OK) {
        d->transfer_max = bus.config.spi.max_transfer_size;
        const spi_device_interface_config_t config = {.clock_speed_hz = d->speed_hz,
            .mode = 0, .spics_io_num = d->cs, .queue_size = 1};
        error = solar_os_bus_spi_add_device(d->bus, &config, &d->spi);
    }
    if (error == ESP_OK) {
        const solar_os_himax_io_t io = {.ctx = d, .write = write_bytes, .read = read_bytes,
            .now_ms = now_ms, .wait_ms = wait_ms, .resize = resize};
        error = solar_os_himax_init(&d->camera, &io);
    }
    if (error == ESP_OK) {
        static const solar_os_camera_backend_ops_t operations = {.start = camera_start,
            .stop = camera_stop, .capture = camera_capture, .release = camera_release,
            .publish = publish, .unpublish = unpublish, .get_resolutions = camera_resolutions,
            .control = camera_control, .invalidate = camera_invalidate};
        const solar_os_camera_backend_t backend = {.driver = "himax-hx6538", .ctx = d,
            .ops = &operations, .default_config = &default_config, .processor = "himax", .processor_source = d->name};
        error = solar_os_camera_register_backend(&backend);
    }
    if (error != ESP_OK) {
        if (d->spi) (void)spi_bus_remove_device(d->spi);
        solar_os_memory_free(d);
        return error;
    }
    device = d;
    return ESP_OK;
}

static esp_err_t detach(const char *name)
{
    if (!device || strcmp(device->name, name)) return ESP_ERR_NOT_FOUND;
    esp_err_t error = solar_os_camera_unregister_backend("himax-hx6538");
    if (error != ESP_OK) return error;
    solar_os_himax_clear(&device->camera);
    solar_os_memory_free(device); device = NULL;
    return ESP_OK;
}

#define LINE(key_) {.key = key_, .role = key_, .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .value_hint = "gpio|controller:line"}
static const solar_os_expansion_binding_spec_t specs[] = {
    {.key = "spi", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .value_hint = "bus", .required = true},
    {.key = "cs", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .value_hint = "cs", .required = true},
    LINE("ready"), LINE("reset"), LINE("power"),
    {.key = "clock", .role = "clock", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
     .value_hint = "100..12000 kHz", .has_value_range = true, .min_value = 100, .max_value = 12000},
};
#undef LINE
const solar_os_expansion_driver_t solar_os_himax_hx6538_expansion_driver = {
    .name = "himax-hx6538", .summary = "Himax HX6538 SSCMA JPEG camera",
    .category = SOLAR_OS_EXPANSION_CATEGORY_SENSOR, .early = true,
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI | SOLAR_OS_BOARD_CAP_PSRAM,
    .binding_specs = specs, .binding_spec_count = sizeof(specs) / sizeof(specs[0]),
    .attach = attach, .detach = detach,
};
