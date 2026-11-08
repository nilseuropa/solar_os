#include "solar_os_gpio_matrix.h"

#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "keyboard_button.h"
#include "solar_os_input_keymap.h"
#include "solar_os_memory.h"
#include "solar_os_resources.h"

#define MATRIX_DEVICES 4U
#define MATRIX_ROWS 8U
#define MATRIX_COLS 10U
#define MATRIX_KEYS (MATRIX_ROWS * MATRIX_COLS)
#define MATRIX_SCAN_US 1000U

typedef struct {
    bool active, stopping, detaching, touched, columns_drive, diodes;
    unsigned rows, cols, debounce;
    int row_pins[MATRIX_ROWS], col_pins[MATRIX_COLS];
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    bool raw[MATRIX_KEYS], held[MATRIX_KEYS], blocked[MATRIX_KEYS];
    keyboard_btn_handle_t scanner;
    solar_os_input_source_t source;
    solar_os_input_keymap_handle_t keymap;
} matrix_device_t;

static EXT_RAM_BSS_ATTR matrix_device_t devices[MATRIX_DEVICES];
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool take_mutex(void)
{
    portENTER_CRITICAL(&init_lock);
    if (mutex == NULL) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static matrix_device_t *find_device(const char *name)
{
    for (unsigned i = 0; i < MATRIX_DEVICES; i++)
        if (devices[i].active && strcmp(devices[i].name, name) == 0) return &devices[i];
    return NULL;
}

static esp_err_t keymap_lock(void *context)
{
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    const matrix_device_t *dev = context;
    if (!dev->active || dev->stopping) {
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

static esp_err_t keymap_prepare(void *context)
{
    matrix_device_t *dev = context;
    /* A held switch must be released before it can use the new mapping. */
    memcpy(dev->blocked, dev->raw, sizeof(dev->blocked));
    memset(dev->held, 0, sizeof(dev->held));
    return ESP_OK;
}

static void report_keys(keyboard_btn_handle_t scanner, keyboard_btn_report_t report, void *context)
{
    (void)scanner;
    if (!take_mutex()) return;
    matrix_device_t *dev = context;
    if (!dev->active || dev->stopping) { xSemaphoreGive(mutex); return; }
    bool raw[MATRIX_KEYS] = {0}, ghost[MATRIX_KEYS] = {0};
    uint16_t row_bits[MATRIX_ROWS] = {0};
    bool valid = report.key_pressed_num <= dev->rows * dev->cols &&
        (report.key_pressed_num == 0 || report.key_data != NULL);
    for (unsigned i = 0; valid && i < report.key_pressed_num; i++) {
        unsigned row = dev->columns_drive ? report.key_data[i].input_index : report.key_data[i].output_index;
        unsigned col = dev->columns_drive ? report.key_data[i].output_index : report.key_data[i].input_index;
        if (row >= dev->rows || col >= dev->cols || raw[row * dev->cols + col]) {
            valid = false;
            break;
        }
        raw[row * dev->cols + col] = true;
        row_bits[row] |= 1U << col;
    }
    if (!valid) {
        solar_os_input_keymap_release_all(dev->keymap);
        memset(dev->held, 0, sizeof(dev->held));
        /* A later valid snapshot restores the source. */
        (void)solar_os_input_keyboard_source_set_ready(dev->source, false);
        xSemaphoreGive(mutex);
        return;
    }
    if (!dev->diodes) {
        for (unsigned a = 0; a < dev->rows; a++) {
            for (unsigned b = a + 1U; b < dev->rows; b++) {
                unsigned common = row_bits[a] & row_bits[b];
                if ((common & (common - 1U)) == 0U) continue;
                for (unsigned col = 0; col < dev->cols; col++) {
                    if (common & (1U << col)) {
                        ghost[a * dev->cols + col] = true;
                        ghost[b * dev->cols + col] = true;
                    }
                }
            }
        }
    }
    memcpy(dev->raw, raw, sizeof(raw));
    (void)solar_os_input_keyboard_source_set_ready(dev->source, true);
    /* Apply a simultaneous layer/modifier press before any ordinary keys,
     * regardless of where those switches are wired in the matrix. */
    for (unsigned pass = 0; pass < 4; pass++) {
        for (unsigned key = 0; key < dev->rows * dev->cols; key++) {
            if (!raw[key]) dev->blocked[key] = false;
            bool pressed = raw[key] && !dev->blocked[key] && (!ghost[key] || dev->held[key]);
            if (pressed == dev->held[key] || pressed != (pass != 0)) continue;
            if (pressed && solar_os_input_keymap_press_priority(dev->keymap, key + 1U) != pass - 1U) continue;
            bool published;
            esp_err_t err = solar_os_input_keymap_process(dev->keymap, key + 1U, pressed, &published);
            if (err != ESP_OK) {
                solar_os_input_keymap_release_all(dev->keymap);
                memset(dev->held, 0, sizeof(dev->held));
                memcpy(dev->blocked, raw, sizeof(dev->blocked));
                xSemaphoreGive(mutex);
                return;
            }
            dev->held[key] = pressed;
        }
    }
    xSemaphoreGive(mutex);
}

static esp_err_t parse_bindings(matrix_device_t *dev,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    bool drive_seen = false, debounce_seen = false, diodes_seen = false;
    for (unsigned i = 0; i < MATRIX_ROWS; i++) dev->row_pins[i] = -1;
    for (unsigned i = 0; i < MATRIX_COLS; i++) dev->col_pins[i] = -1;
    dev->debounce = 10U;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            if (strcmp(b->role, "drive") == 0 && !drive_seen && b->value >= 0 && b->value <= 1) {
                drive_seen = true; dev->columns_drive = b->value != 0;
            } else if (strcmp(b->role, "diodes") == 0 && !diodes_seen && b->value >= 0 && b->value <= 1) {
                diodes_seen = true; dev->diodes = b->value != 0;
            } else if (strcmp(b->role, "debounce-ms") == 0 && !debounce_seen && b->value >= 1 && b->value <= 100) {
                debounce_seen = true; dev->debounce = (unsigned)b->value;
            } else return ESP_ERR_INVALID_ARG;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            int *pins = NULL;
            unsigned index = 0;
            if (strncmp(b->role, "row", 3) == 0 && b->role[3] >= '0' && b->role[3] <= '7' && b->role[4] == '\0') {
                index = (unsigned)(b->role[3] - '0'); pins = dev->row_pins;
                if (dev->rows <= index) dev->rows = index + 1U;
            } else if (strncmp(b->role, "col", 3) == 0 && b->role[3] >= '0' && b->role[3] <= '9' && b->role[4] == '\0') {
                index = (unsigned)(b->role[3] - '0'); pins = dev->col_pins;
                if (dev->cols <= index) dev->cols = index + 1U;
            } else return ESP_ERR_INVALID_ARG;
            if (pins[index] >= 0 || !GPIO_IS_VALID_GPIO(b->value)) return ESP_ERR_INVALID_ARG;
            pins[index] = b->value;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (dev->rows == 0 || dev->cols == 0) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < dev->rows + dev->cols; i++) {
        int pin = i < dev->rows ? dev->row_pins[i] : dev->col_pins[i - dev->rows];
        /* Classic ESP32 input-only pins lack the internal pull-up we need. */
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return ESP_ERR_INVALID_ARG;
        for (unsigned j = 0; j < i; j++) {
            int previous = j < dev->rows ? dev->row_pins[j] : dev->col_pins[j - dev->rows];
            if (pin == previous) return ESP_ERR_INVALID_ARG;
        }
        solar_os_resource_claim_t claim;
        if (!solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim) ||
            strcmp(claim.owner, dev->name) != 0) return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void reset_pins(matrix_device_t *dev)
{
    if (!dev->touched) return;
    for (unsigned i = 0; i < dev->rows; i++) (void)gpio_reset_pin(dev->row_pins[i]);
    for (unsigned i = 0; i < dev->cols; i++) (void)gpio_reset_pin(dev->col_pins[i]);
    dev->touched = false;
}

static esp_err_t attach_locked(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= SOLAR_OS_EXPANSION_DEVICE_NAME_MAX ||
        strlen(name) >= SOLAR_OS_INPUT_SOURCE_NAME_MAX || bindings == NULL ||
        count == 0 || count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX) return ESP_ERR_INVALID_ARG;
    if (find_device(name) != NULL) return ESP_ERR_INVALID_STATE;
    matrix_device_t *dev = NULL;
    for (unsigned i = 0; i < MATRIX_DEVICES; i++)
        if (!devices[i].active) { dev = &devices[i]; break; }
    if (dev == NULL) return ESP_ERR_NO_MEM;
    memset(dev, 0, sizeof(*dev));
    strlcpy(dev->name, name, sizeof(dev->name));
    esp_err_t err = parse_bindings(dev, bindings, count);
    solar_os_input_keymap_t *map = NULL;
    if (err == ESP_OK) {
        map = solar_os_memory_alloc(sizeof(*map), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "matrix-map");
        if (map == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        memset(map, 0, sizeof(*map));
        map->rows = dev->rows; map->cols = dev->cols;
        map->first = 1U; map->stride = dev->cols;
        map->slot_count = dev->rows * dev->cols;
        for (unsigned key = 1; key <= map->slot_count; key++) {
            map->physical[key] = key;
            map->keys[0][key].flags = SOLAR_OS_INPUT_KEYMAP_RAW;
        }
        err = solar_os_input_keyboard_source_open(name, true, &dev->source);
    }
    if (err == ESP_OK) {
        const solar_os_input_keymap_ops_t ops = {
            .lock = keymap_lock, .unlock = keymap_unlock, .prepare = keymap_prepare, .context = dev,
        };
        err = solar_os_input_keymap_register(dev->source, map, &ops, &dev->keymap);
    }
    free(map);
    if (err == ESP_OK) {
        keyboard_btn_config_t config = {
            .output_gpios = dev->columns_drive ? dev->col_pins : dev->row_pins,
            .input_gpios = dev->columns_drive ? dev->row_pins : dev->col_pins,
            .output_gpio_num = dev->columns_drive ? dev->cols : dev->rows,
            .input_gpio_num = dev->columns_drive ? dev->rows : dev->cols,
            .active_level = 0, .debounce_ticks = dev->debounce, .ticks_interval = MATRIX_SCAN_US,
            .enable_power_save = false, .priority = 1, .core_id = tskNO_AFFINITY,
            .start_paused = true,
        };
        dev->touched = true;
        err = keyboard_button_create(&config, &dev->scanner);
    }
    if (err == ESP_OK) {
        const keyboard_btn_cb_config_t cb = {
            .event = KBD_EVENT_PRESSED, .callback = report_keys, .user_data = dev,
        };
        err = keyboard_button_register_cb(dev->scanner, cb, NULL);
    }
    if (err == ESP_OK) err = solar_os_keyboard_button_start(dev->scanner);
    if (err == ESP_OK) {
        dev->active = true;
        (void)solar_os_input_keymap_activate(dev->keymap);
    } else {
        /* The paused scanner cannot call into this locked producer on failure. */
        if (dev->scanner != NULL) (void)keyboard_button_delete(dev->scanner);
        if (dev->keymap != SOLAR_OS_INPUT_KEYMAP_INVALID)
            (void)solar_os_input_keymap_unregister(dev->keymap);
        if (dev->source != SOLAR_OS_INPUT_SOURCE_INVALID) solar_os_input_source_close(dev->source);
        reset_pins(dev);
        memset(dev, 0, sizeof(*dev));
    }
    return err;
}

esp_err_t solar_os_gpio_matrix_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    esp_err_t err = attach_locked(name, bindings, count);
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t solar_os_gpio_matrix_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    matrix_device_t *dev = find_device(name);
    if (dev == NULL || dev->detaching) {
        xSemaphoreGive(mutex);
        return dev == NULL ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_STATE;
    }
    dev->detaching = true;
    dev->stopping = true;
    solar_os_input_keymap_release_all(dev->keymap);
    (void)solar_os_input_keyboard_source_set_ready(dev->source, false);
    xSemaphoreGive(mutex);
    /* Delete joins the component's task; callbacks must be able to take our lock. */
    esp_err_t err = ESP_OK;
    if (dev->scanner != NULL) {
        err = keyboard_button_delete(dev->scanner);
        if (err == ESP_OK) dev->scanner = NULL;
    }
    if (err == ESP_OK && dev->keymap != SOLAR_OS_INPUT_KEYMAP_INVALID) {
        err = solar_os_input_keymap_unregister(dev->keymap);
        if (err == ESP_OK) dev->keymap = SOLAR_OS_INPUT_KEYMAP_INVALID;
    }
    if (!take_mutex()) return ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        solar_os_input_source_close(dev->source);
        reset_pins(dev);
        memset(dev, 0, sizeof(*dev));
    } else dev->detaching = false;
    xSemaphoreGive(mutex);
    return err;
}
