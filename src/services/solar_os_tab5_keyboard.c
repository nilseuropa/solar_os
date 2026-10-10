#include "solar_os_tab5_keyboard.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define TAB5_DEVICE_MAX 4U
#define TAB5_POLL_MS 10U
#define TAB5_IRQ_FALLBACK_MS 50U
#define TAB5_RETRY_MS 250U
#define TAB5_STACK 3072U

typedef struct {
    bool active, detaching, recovering, irq_installed, pin_configured, saved_config;
    volatile bool stop_requested, worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address, saved_mode, saved_int_cfg;
    int irq_pin;
    solar_os_input_source_t source;
    solar_os_input_keymap_handle_t keymap;
    TaskHandle_t task;
} tab5_device_t;

/* ISR contexts must remain in internal RAM. */
static tab5_device_t devices[TAB5_DEVICE_MAX];
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE irq_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *TAG = "tab5_keyboard";

static bool take_mutex(void)
{
    portENTER_CRITICAL(&init_lock);
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static tab5_device_t *find_device(const char *name)
{
    for (unsigned i = 0; i < TAB5_DEVICE_MAX; i++)
        if (devices[i].active && strcmp(devices[i].name, name) == 0) return &devices[i];
    return NULL;
}

static esp_err_t restore_config(tab5_device_t *dev)
{
    if (!dev->saved_config) return ESP_OK;
    esp_err_t err = solar_os_tab5_keyboard_clear(dev->bus, dev->address);
    if (err == ESP_OK) err = solar_os_bus_i2c_write_reg(dev->bus, dev->address,
        SOLAR_OS_TAB5_KEYBOARD_REG_MODE, &dev->saved_mode, 1U);
    if (err == ESP_OK) err = solar_os_bus_i2c_write_reg(dev->bus, dev->address,
        SOLAR_OS_TAB5_KEYBOARD_REG_INT_CFG, &dev->saved_int_cfg, 1U);
    return err;
}

static void stop_irq(tab5_device_t *dev)
{
    if (dev->irq_installed) {
        (void)gpio_intr_disable(dev->irq_pin);
        (void)gpio_isr_handler_remove(dev->irq_pin);
        dev->irq_installed = false;
    }
}

static void release_pin(tab5_device_t *dev)
{
    stop_irq(dev);
    if (dev->pin_configured) {
        (void)gpio_reset_pin(dev->irq_pin);
        dev->pin_configured = false;
    }
}

static void irq_handler(void *arg)
{
    tab5_device_t *dev = arg;
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&irq_lock);
    if (dev->task != NULL) vTaskNotifyGiveFromISR(dev->task, &wake);
    portEXIT_CRITICAL_ISR(&irq_lock);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static esp_err_t keymap_lock(void *context)
{
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    tab5_device_t *dev = context;
    if (!dev->active || dev->stop_requested) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void keymap_unlock(void *context)
{
    (void)context;
    xSemaphoreGive(mutex);
}

static void interrupt_input(tab5_device_t *dev)
{
    solar_os_input_keymap_release_all(dev->keymap);
    (void)solar_os_input_keyboard_source_set_ready(dev->source, false);
    dev->recovering = true;
}

static esp_err_t keymap_prepare(void *context)
{
    tab5_device_t *dev = context;
    esp_err_t err = solar_os_tab5_keyboard_clear(dev->bus, dev->address);
    if (err != ESP_OK) interrupt_input(dev);
    return err;
}

static esp_err_t poll_once(tab5_device_t *dev)
{
    uint8_t events[SOLAR_OS_TAB5_KEYBOARD_FIFO_DEPTH];
    unsigned count;
    esp_err_t err = solar_os_tab5_keyboard_read(dev->bus, dev->address, events, &count);
    if (err != ESP_OK) return err;
    for (unsigned i = 0; i < count; i++) {
        uint16_t physical;
        bool pressed, published;
        err = solar_os_tab5_keyboard_decode(events[i], &physical, &pressed);
        if (err == ESP_OK)
            err = solar_os_input_keymap_process(dev->keymap, physical, pressed, &published);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void worker(void *arg)
{
    tab5_device_t *dev = arg;
    while (!dev->stop_requested) {
        if (!take_mutex()) break;
        if (dev->stop_requested) { xSemaphoreGive(mutex); break; }
        esp_err_t err = dev->recovering ?
            solar_os_tab5_keyboard_configure(dev->bus, dev->address) : poll_once(dev);
        if (err != ESP_OK) {
            if (!dev->recovering) {
                interrupt_input(dev);
                ESP_LOGW(TAG, "%s input interrupted: %s", dev->name, esp_err_to_name(err));
            }
        } else if (dev->recovering) {
            dev->recovering = false;
            (void)solar_os_input_keyboard_source_set_ready(dev->source, true);
        }
        const unsigned delay = dev->recovering ? TAB5_RETRY_MS :
            (dev->irq_pin >= 0 ? TAB5_IRQ_FALLBACK_MS : TAB5_POLL_MS);
        xSemaphoreGive(mutex);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay));
    }
    if (take_mutex()) {
        /* Remove the ISR before deleting its notification target, including
         * unexpected worker exits. The shared GPIO ISR service stays installed. */
        stop_irq(dev);
        portENTER_CRITICAL(&irq_lock);
        dev->task = NULL;
        portEXIT_CRITICAL(&irq_lock);
        solar_os_input_keymap_release_all(dev->keymap);
        (void)solar_os_input_keyboard_source_set_ready(dev->source, false);
        dev->worker_done = true;
        xSemaphoreGive(mutex);
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t attach_locked(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= SOLAR_OS_INPUT_SOURCE_NAME_MAX ||
        bindings == NULL || count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX)
        return ESP_ERR_INVALID_ARG;
    if (find_device(name) != NULL) return ESP_ERR_INVALID_STATE;
    const char *bus = NULL;
    int address = -1, irq = -1;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && bus == NULL)
            bus = b->target;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS && address < 0 &&
                 b->value >= 0x08 && b->value <= 0x77) address = b->value;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && irq < 0 &&
                 strcmp(b->role, "irq") == 0 && GPIO_IS_VALID_GPIO(b->value)) irq = b->value;
        else return ESP_ERR_INVALID_ARG;
    }
    if (bus == NULL || bus[0] == '\0' || strlen(bus) >= SOLAR_OS_EXPANSION_TARGET_MAX ||
        address < 0 || !solar_os_expansion_find_i2c_bus(bus, NULL, NULL))
        return ESP_ERR_INVALID_ARG;
    tab5_device_t *dev = NULL;
    for (unsigned i = 0; i < TAB5_DEVICE_MAX; i++) {
        if (devices[i].active && devices[i].address == address && strcmp(devices[i].bus, bus) == 0)
            return ESP_ERR_INVALID_STATE;
        if (!devices[i].active && dev == NULL) dev = &devices[i];
    }
    if (dev == NULL) return ESP_ERR_NO_MEM;
    memset(dev, 0, sizeof(*dev));
    dev->irq_pin = irq; dev->address = (uint8_t)address;
    strlcpy(dev->name, name, sizeof(dev->name));
    strlcpy(dev->bus, bus, sizeof(dev->bus));
    esp_err_t err = solar_os_bus_i2c_probe(bus, dev->address);
    if (err == ESP_OK) err = solar_os_bus_i2c_read_reg(bus, dev->address,
        SOLAR_OS_TAB5_KEYBOARD_REG_MODE, &dev->saved_mode, 1U);
    if (err == ESP_OK) err = solar_os_bus_i2c_read_reg(bus, dev->address,
        SOLAR_OS_TAB5_KEYBOARD_REG_INT_CFG, &dev->saved_int_cfg, 1U);
    if (err == ESP_OK && (dev->saved_mode > 2U || (dev->saved_int_cfg & ~7U) != 0U))
        err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        dev->saved_config = true;
        err = solar_os_tab5_keyboard_configure(bus, dev->address);
    }
    if (err == ESP_OK && irq >= 0) {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << (unsigned)irq, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        dev->pin_configured = true;
        err = gpio_config(&config);
        if (err == ESP_OK) {
            err = gpio_install_isr_service(0);
            if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
        }
    }
    solar_os_input_keymap_t *map = NULL;
    if (err == ESP_OK) {
        map = solar_os_memory_alloc(sizeof(*map), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "keyboard-map");
        if (map == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        solar_os_tab5_keyboard_map(map);
        err = solar_os_input_keyboard_source_open(name, true, &dev->source);
    }
    if (err == ESP_OK) {
        const solar_os_input_keymap_ops_t ops = {
            .lock = keymap_lock, .unlock = keymap_unlock,
            .prepare = keymap_prepare, .context = dev,
        };
        err = solar_os_input_keymap_register(dev->source, map, &ops, &dev->keymap);
    }
    free(map);
    /* Enable the GPIO before publishing. Until task creation finishes the ISR
     * has no notification target; polling covers a startup edge. */
    if (err == ESP_OK && irq >= 0) {
        err = gpio_isr_handler_add(irq, irq_handler, dev);
        dev->irq_installed = err == ESP_OK;
        if (err == ESP_OK) err = gpio_set_intr_type(irq, GPIO_INTR_NEGEDGE);
        if (err == ESP_OK) err = gpio_intr_enable(irq);
    }
    TaskHandle_t task = NULL;
    if (err == ESP_OK && solar_os_task_create_pinned_internal(worker, name, TAB5_STACK,
            dev, tskIDLE_PRIORITY + 1, &task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        portENTER_CRITICAL(&irq_lock);
        dev->task = task;
        portEXIT_CRITICAL(&irq_lock);
        dev->active = true;
        /* A successfully registered, unpublished handle cannot be closing.
         * Publish only after all fallible initialization has completed. */
        (void)solar_os_input_keymap_activate(dev->keymap);
    }
    if (err != ESP_OK) {
        release_pin(dev);
        /* The unpublished provider has no external references. */
        if (dev->keymap != SOLAR_OS_INPUT_KEYMAP_INVALID)
            (void)solar_os_input_keymap_unregister(dev->keymap);
        if (dev->source != SOLAR_OS_INPUT_SOURCE_INVALID) solar_os_input_source_close(dev->source);
        (void)restore_config(dev);
        memset(dev, 0, sizeof(*dev));
    }
    return err;
}

esp_err_t solar_os_tab5_keyboard_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    const esp_err_t err = attach_locked(name, bindings, count);
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t solar_os_tab5_keyboard_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    tab5_device_t *dev = find_device(name);
    if (dev == NULL || dev->detaching) {
        xSemaphoreGive(mutex);
        return dev == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    }
    dev->detaching = true;
    stop_irq(dev);
    dev->stop_requested = true;
    if (!dev->worker_done) (void)xTaskNotifyGive(dev->task);
    xSemaphoreGive(mutex);
    esp_err_t err = ESP_OK;
    if (!dev->worker_done && !solar_os_task_wait_done(dev->task, &dev->worker_done,
            SOLAR_OS_TASK_STOP_WAIT_MS)) err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK && dev->keymap != SOLAR_OS_INPUT_KEYMAP_INVALID) {
        err = solar_os_input_keymap_unregister(dev->keymap);
        if (err == ESP_OK) dev->keymap = SOLAR_OS_INPUT_KEYMAP_INVALID;
    }
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = restore_config(dev);
    if (err == ESP_OK) {
        solar_os_input_source_close(dev->source);
        release_pin(dev);
        memset(dev, 0, sizeof(*dev));
    } else dev->detaching = false;
    xSemaphoreGive(mutex);
    return err;
}
