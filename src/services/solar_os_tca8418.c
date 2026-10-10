#include "solar_os_tca8418.h"

#include <string.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_input.h"
#include "solar_os_input_keymap.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define TCA8418_DEVICE_MAX 4U
#define TCA8418_POLL_MS 10U
#define TCA8418_RETRY_MS 250U
#define TCA8418_TASK_STACK 3072U
#define TCA8418_FIFO_MAX 10U
#define TCA8418_DRAIN_MAX 32U

#define REG_CFG 0x01U
#define REG_INT_STAT 0x02U
#define REG_KEY_COUNT 0x03U
#define REG_KEY_EVENT 0x04U
#define INT_KEY 0x01U
#define INT_OVERFLOW 0x08U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    solar_os_input_source_t source;
    TaskHandle_t task;
    solar_os_input_keymap_handle_t keymap;
    uint8_t rows, cols;
    uint32_t transitions;
    uint32_t overflows;
    uint32_t errors;
    int reset_pin;
    int irq_pin;
    bool recovering;
} tca8418_device_t;

static const char *TAG = "tca8418";
static EXT_RAM_BSS_ATTR tca8418_device_t devices[TCA8418_DEVICE_MAX];
static StaticSemaphore_t controller_mutex_buffer;
static SemaphoreHandle_t controller_mutex;
static portMUX_TYPE controller_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool take_mutex(void)
{
    portENTER_CRITICAL(&controller_init_lock);
    if (controller_mutex == NULL)
        controller_mutex = xSemaphoreCreateMutexStatic(&controller_mutex_buffer);
    portEXIT_CRITICAL(&controller_init_lock);
    return controller_mutex != NULL &&
        xSemaphoreTake(controller_mutex, portMAX_DELAY) == pdTRUE;
}

static tca8418_device_t *find_device(const char *name)
{
    if (name == NULL) return NULL;
    for (size_t i = 0; i < TCA8418_DEVICE_MAX; i++)
        if (devices[i].active && strcmp(devices[i].name, name) == 0) return &devices[i];
    return NULL;
}


static void release_pins(tca8418_device_t *device)
{
    if (device->reset_pin >= 0) (void)gpio_reset_pin(device->reset_pin);
    if (device->irq_pin >= 0) (void)gpio_reset_pin(device->irq_pin);
}

static esp_err_t configure_pins(tca8418_device_t *device)
{
    if (device->irq_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (unsigned)device->irq_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "interrupt pin");
    }
    if (device->reset_pin >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (unsigned)device->reset_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_set_level(device->reset_pin, 0), TAG, "reset assert");
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "reset pin");
        /* SCPS215G specifies at least 120 us for reset and recovery. */
        esp_rom_delay_us(200U);
        ESP_RETURN_ON_ERROR(gpio_set_level(device->reset_pin, 1), TAG, "reset release");
        esp_rom_delay_us(200U);
    }
    return ESP_OK;
}

static esp_err_t read_reg(tca8418_device_t *device, uint8_t reg, uint8_t *value)
{
    return solar_os_bus_i2c_read_reg(device->bus,
        SOLAR_OS_TCA8418_ADDRESS, reg, value, 1U);
}

static esp_err_t write_reg(tca8418_device_t *device, uint8_t reg, uint8_t value)
{
    return solar_os_bus_i2c_write_reg(device->bus,
        SOLAR_OS_TCA8418_ADDRESS, reg, &value, 1U);
}

static void release_keys(tca8418_device_t *device)
{
    solar_os_input_keymap_release_all(device->keymap);
}

static esp_err_t discard_fifo(tca8418_device_t *device)
{
    for (size_t i = 0; i < TCA8418_DRAIN_MAX; i++) {
        uint8_t count = 0, discard = 0;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_COUNT, &count), TAG, "FIFO count");
        if ((count & 0x0fU) > TCA8418_FIFO_MAX) return ESP_ERR_INVALID_RESPONSE;
        if ((count & 0x0fU) == 0U) return ESP_OK;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_EVENT, &discard), TAG, "FIFO drain");
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t keymap_lock(void *context)
{
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    tca8418_device_t *device = context;
    if (!device->active || device->stop_requested) {
        xSemaphoreGive(controller_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
static void keymap_unlock(void *context)
{
    (void)context;
    xSemaphoreGive(controller_mutex);
}
static esp_err_t keymap_prepare(void *context)
{
    tca8418_device_t *device = context;
    esp_err_t err = discard_fifo(device);
    if (err != ESP_OK) {
        device->recovering = true;
        (void)solar_os_input_keyboard_source_set_ready(device->source, false);
    }
    return err;
}

static esp_err_t configure_matrix(tca8418_device_t *device)
{
    /* TI TCA8418 SCPS215G: configure the requested rows and columns, enable debounce,
     * disable GPIO events/interrupts and keypad lock. No host IRQ is needed.
     * Overflow erratum 8.6.4 requires BOTH OVR_FLOW_M and OVR_FLOW_IEN;
     * KE_IEN keeps key status available even though the INT pin is unused.
     * Auto-increment stays off: every transfer addresses a single register.
     */
    const unsigned column_mask = (1U << device->cols) - 1U;
    const uint8_t setup[][2] = {
        {REG_CFG, 0x00},
        {0x1d, 0x00}, {0x1e, 0x00}, {0x1f, 0x00},
        {0x23, 0x00}, {0x24, 0x00}, {0x25, 0x00},
        {0x1a, 0x00}, {0x1b, 0x00}, {0x1c, 0x00},
        {0x20, 0x00}, {0x21, 0x00}, {0x22, 0x00},
        {0x29, 0x00}, {0x2a, 0x00}, {0x2b, 0x00},
        {0x2c, 0x00}, {0x2d, 0x00}, {0x2e, 0x00},
        {REG_KEY_COUNT, 0x00},
        {0x1d, (uint8_t)((1U << device->rows) - 1U)},
        {0x1e, (uint8_t)column_mask}, {0x1f, (uint8_t)(column_mask >> 8U)},
        {REG_CFG, 0x29},
    };
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        ESP_RETURN_ON_ERROR(write_reg(device, setup[i][0], setup[i][1]), TAG, "matrix setup");
    }
    ESP_RETURN_ON_ERROR(discard_fifo(device), TAG, "stale FIFO");
    return write_reg(device, REG_INT_STAT, 0x1fU);
}

static esp_err_t recover_overflow(tca8418_device_t *device)
{
    device->overflows++;
    release_keys(device);
    ESP_RETURN_ON_ERROR(discard_fifo(device), TAG, "overflow FIFO");
    return write_reg(device, REG_INT_STAT, INT_OVERFLOW | INT_KEY);
}

static esp_err_t poll_once(tca8418_device_t *device)
{
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_reg(device, REG_INT_STAT, &status), TAG, "key status");
    if ((status & INT_OVERFLOW) != 0U) return recover_overflow(device);

    /* Stage a bounded batch before publishing. Check overflow again so an
     * overflow during the read cannot leave a partially trusted held state.
     * Poll the count regardless of K_INT to avoid an interrupt-ack race.
     */
    uint8_t events[TCA8418_FIFO_MAX];
    size_t event_count = 0;
    while (event_count < TCA8418_FIFO_MAX) {
        uint8_t count = 0;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_COUNT, &count), TAG, "key count");
        count &= 0x0fU;
        if (count > TCA8418_FIFO_MAX) return ESP_ERR_INVALID_RESPONSE;
        if (count == 0U) break;
        ESP_RETURN_ON_ERROR(read_reg(device, REG_KEY_EVENT, &events[event_count]), TAG, "key event");
        event_count++;
    }
    ESP_RETURN_ON_ERROR(read_reg(device, REG_INT_STAT, &status), TAG, "overflow status");
    if ((status & INT_OVERFLOW) != 0U) return recover_overflow(device);

    for (size_t i = 0; i < event_count; i++) {
        bool published = false;
        ESP_RETURN_ON_ERROR(solar_os_input_keymap_process(device->keymap,
            events[i] & 0x7fU, (events[i] & 0x80U) != 0U, &published), TAG, "mapped input");
        if (published) device->transitions++;
    }
    return write_reg(device, REG_INT_STAT, INT_KEY);
}

static void worker(void *arg)
{
    tca8418_device_t *device = arg;
    while (!device->stop_requested) {
        if (!take_mutex()) break;
        esp_err_t err = device->recovering ? configure_matrix(device) : poll_once(device);
        if (err != ESP_OK) {
            device->errors++;
            if (!device->recovering) {
                release_keys(device);
                (void)solar_os_input_keyboard_source_set_ready(device->source, false);
                ESP_LOGW(TAG, "%s input interrupted: %s", device->name, esp_err_to_name(err));
            }
            device->recovering = true;
        } else if (device->recovering) {
            (void)solar_os_input_keyboard_source_set_ready(device->source, true);
            device->recovering = false;
        }
        const uint32_t delay = device->recovering ? TCA8418_RETRY_MS : TCA8418_POLL_MS;
        xSemaphoreGive(controller_mutex);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay));
    }
    if (take_mutex()) {
        release_keys(device);
        device->worker_done = true;
        xSemaphoreGive(controller_mutex);
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t attach_locked(
    const char *name, const solar_os_expansion_binding_t *bindings, size_t binding_count,
    const solar_os_matrix_keyboard_map_t *profile)
{
    if (name == NULL || name[0] == '\0' || bindings == NULL ||
        strlen(name) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) return ESP_ERR_INVALID_ARG;
    tca8418_device_t *device = NULL;
    for (size_t i = 0; i < TCA8418_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) return ESP_ERR_INVALID_STATE;
        if (!devices[i].active && device == NULL) device = &devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;

    const char *bus = NULL;
    bool have_address = false;
    int reset_pin = -1, irq_pin = -1;
    unsigned rows = 8U, cols = 10U;
    bool have_rows = false, have_cols = false;
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && bus == NULL) {
            bus = bindings[i].target;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address && bindings[i].value == SOLAR_OS_TCA8418_ADDRESS) {
            have_address = true;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(bindings[i].role, "reset") == 0 && reset_pin < 0 &&
                   GPIO_IS_VALID_OUTPUT_GPIO(bindings[i].value)) {
            reset_pin = bindings[i].value;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(bindings[i].role, "irq") == 0 && irq_pin < 0 &&
                   GPIO_IS_VALID_GPIO(bindings[i].value)) {
            irq_pin = bindings[i].value;
        } else if (profile == NULL && bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(bindings[i].role, "rows") == 0 && !have_rows &&
                   bindings[i].value >= 1 && bindings[i].value <= 8) {
            rows = (unsigned)bindings[i].value;
            have_rows = true;
        } else if (profile == NULL && bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(bindings[i].role, "cols") == 0 && !have_cols &&
                   bindings[i].value >= 1 && bindings[i].value <= 10) {
            cols = (unsigned)bindings[i].value;
            have_cols = true;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (bus == NULL || !have_address ||
        (reset_pin >= 0 && reset_pin == irq_pin) ||
        !solar_os_expansion_find_i2c_bus(bus, NULL, NULL))
        return ESP_ERR_INVALID_ARG;
    memset(device, 0, sizeof(*device));
    device->reset_pin = reset_pin;
    device->irq_pin = irq_pin;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->bus, bus, sizeof(device->bus));
    solar_os_matrix_keyboard_map_t *generic = NULL;
    esp_err_t err = ESP_OK;
    if (profile == NULL) {
        generic = solar_os_memory_alloc(sizeof(*generic),
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "keyboard-map");
        err = generic == NULL ? ESP_ERR_NO_MEM : solar_os_matrix_keyboard_default_map(generic, rows, cols);
        profile = generic;
    }
    if (err == ESP_OK) err = solar_os_input_keymap_validate(profile);
    if (err == ESP_OK && (profile->rows < 1U || profile->rows > 8U ||
        profile->cols < 1U || profile->cols > 10U || profile->first != 1U || profile->stride != 10U))
        err = ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) { free(generic); return err; }
    device->rows = profile->rows; device->cols = profile->cols;
    err = configure_pins(device);
    if (err == ESP_OK) err = solar_os_bus_i2c_probe(bus, SOLAR_OS_TCA8418_ADDRESS);
    if (err == ESP_OK) err = configure_matrix(device);
    if (err != ESP_OK) {
        release_pins(device);
        free(generic);
        ESP_RETURN_ON_ERROR(err, TAG, "keyboard initialization");
    }
    err = solar_os_input_keyboard_source_open(name, true, &device->source);
    if (err == ESP_OK) {
        const solar_os_input_keymap_ops_t ops = {
            .lock = keymap_lock, .unlock = keymap_unlock,
            .prepare = keymap_prepare, .context = device,
        };
        err = solar_os_input_keymap_register(device->source, profile, &ops, &device->keymap);
        if (err != ESP_OK) solar_os_input_source_close(device->source);
    }
    free(generic);
    if (err != ESP_OK) { release_pins(device); return err; }
    device->active = true;
    if (solar_os_task_create_pinned_internal(worker, name, TCA8418_TASK_STACK,
            device, tskIDLE_PRIORITY + 1, &device->task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        (void)solar_os_input_keymap_unregister(device->keymap);
        solar_os_input_source_close(device->source);
        release_pins(device);
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    (void)solar_os_input_keymap_activate(device->keymap);
    ESP_LOGI(TAG, "%s attached on %s address 0x34", name, bus);
    return ESP_OK;
}


esp_err_t solar_os_tca8418_attach_profile(
    const char *name, const solar_os_expansion_binding_t *bindings, size_t count,
    const solar_os_matrix_keyboard_map_t *profile)
{
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    const esp_err_t err = attach_locked(name, bindings, count, profile);
    xSemaphoreGive(controller_mutex);
    return err;
}

esp_err_t solar_os_tca8418_attach(
    const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    return solar_os_tca8418_attach_profile(name, bindings, count, NULL);
}

esp_err_t solar_os_tca8418_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    tca8418_device_t *device = find_device(name);
    if (device == NULL) {
        xSemaphoreGive(controller_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    device->stop_requested = true;
    if (!device->worker_done) (void)xTaskNotifyGive(device->task);
    xSemaphoreGive(controller_mutex);
    if (!device->worker_done && !solar_os_task_wait_done(device->task, &device->worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t err = solar_os_input_keymap_unregister(device->keymap);
    if (err != ESP_OK) return err;
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    if (!device->active || strcmp(device->name, name) != 0) {
        xSemaphoreGive(controller_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "%s detached: %lu transitions, %lu overflows, %lu errors",
        name, (unsigned long)device->transitions, (unsigned long)device->overflows,
        (unsigned long)device->errors);
    solar_os_input_source_close(device->source);
    release_pins(device);
    memset(device, 0, sizeof(*device));
    xSemaphoreGive(controller_mutex);
    return ESP_OK;
}

/* Compatibility entrypoints; public controls now use the input service. */
esp_err_t solar_os_tca8418_get_keymap(const char *name, bool defaults,
    solar_os_matrix_keyboard_map_t *map)
{
    return solar_os_input_keymap_get(name, defaults, map);
}
esp_err_t solar_os_tca8418_set_keymap(const char *name, const solar_os_matrix_keyboard_map_t *map)
{
    return solar_os_input_keymap_set(name, map);
}
esp_err_t solar_os_tca8418_reset_keymap(const char *name)
{
    return solar_os_input_keymap_reset(name);
}
