#include "solar_os_hid.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_port.h"
#include "solar_os_task.h"

#define HID_KEYBOARD_QUEUE_LEN 32
#define HID_TASK_STACK 3072
#define HID_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define HID_RETRY_INTERVAL_MS 5
#define HID_KEYBOARD_KEYS_MAX 6
#define HID_MOUSE_DELTA_LIMIT 32767

typedef struct {
    hid_port_keyboard_report_t report;
} hid_keyboard_event_t;

typedef struct {
    SemaphoreHandle_t mutex;
    QueueHandle_t keyboard_queue;
    TaskHandle_t task;
    bool initialized;
    hid_port_keyboard_report_t keyboard;
    uint8_t mouse_buttons;
    int32_t mouse_x;
    int32_t mouse_y;
    bool mouse_dirty;
    uint32_t mouse_generation;
    int16_t gamepad_axes[SOLAR_OS_HID_AXIS_COUNT];
    uint32_t gamepad_buttons;
    uint8_t gamepad_hat;
    bool gamepad_dirty;
    uint32_t gamepad_generation;
} hid_state_t;

static hid_state_t hid;
static portMUX_TYPE hid_init_lock = portMUX_INITIALIZER_UNLOCKED;
static bool hid_initializing;

static bool hid_initialized(void)
{
    portENTER_CRITICAL(&hid_init_lock);
    const bool initialized = hid.initialized;
    portEXIT_CRITICAL(&hid_init_lock);
    return initialized;
}

static void hid_lock(void)
{
    xSemaphoreTake(hid.mutex, portMAX_DELAY);
}

static void hid_unlock(void)
{
    xSemaphoreGive(hid.mutex);
}

static bool hid_can_accept(void)
{
    return hid_initialized() && hid_port_is_connected();
}

static int8_t hid_mouse_chunk(int32_t value)
{
    if (value > INT8_MAX) {
        return INT8_MAX;
    }
    if (value < INT8_MIN) {
        return INT8_MIN;
    }
    return (int8_t)value;
}

static int8_t hid_axis_wire_value(int16_t value)
{
    if (value == INT16_MIN) {
        return INT8_MIN + 1;
    }
    if (value == INT16_MAX) {
        return INT8_MAX;
    }
    return (int8_t)(value / 258);
}

static bool hid_send_keyboard(void)
{
    bool sent = false;
    hid_lock();
    hid_keyboard_event_t event;
    if (xQueuePeek(hid.keyboard_queue, &event, 0) == pdTRUE &&
        hid_port_send_report(HID_PORT_REPORT_KEYBOARD,
                             &event.report,
                             sizeof(event.report)) == ESP_OK) {
        (void)xQueueReceive(hid.keyboard_queue, &event, 0);
        sent = true;
    }
    hid_unlock();
    return sent;
}

static bool hid_send_mouse(void)
{
    hid_port_mouse_report_t report;
    uint32_t generation;

    hid_lock();
    if (!hid.mouse_dirty) {
        hid_unlock();
        return false;
    }
    report = (hid_port_mouse_report_t){
        .buttons = hid.mouse_buttons,
        .x = hid_mouse_chunk(hid.mouse_x),
        .y = hid_mouse_chunk(hid.mouse_y),
    };
    generation = hid.mouse_generation;
    hid_unlock();

    if (hid_port_send_report(HID_PORT_REPORT_MOUSE, &report, sizeof(report)) != ESP_OK) {
        return false;
    }

    hid_lock();
    hid.mouse_x -= report.x;
    hid.mouse_y -= report.y;
    if (hid.mouse_generation == generation && hid.mouse_x == 0 && hid.mouse_y == 0) {
        hid.mouse_dirty = false;
    }
    hid_unlock();
    return true;
}

static bool hid_send_gamepad(void)
{
    hid_port_gamepad_report_t report;
    uint32_t generation;

    hid_lock();
    if (!hid.gamepad_dirty) {
        hid_unlock();
        return false;
    }
    report = (hid_port_gamepad_report_t){
        .x = hid_axis_wire_value(hid.gamepad_axes[SOLAR_OS_HID_AXIS_X]),
        .y = hid_axis_wire_value(hid.gamepad_axes[SOLAR_OS_HID_AXIS_Y]),
        .z = hid_axis_wire_value(hid.gamepad_axes[SOLAR_OS_HID_AXIS_Z]),
        .rz = hid_axis_wire_value(hid.gamepad_axes[SOLAR_OS_HID_AXIS_RZ]),
        .rx = hid_axis_wire_value(hid.gamepad_axes[SOLAR_OS_HID_AXIS_RX]),
        .ry = hid_axis_wire_value(hid.gamepad_axes[SOLAR_OS_HID_AXIS_RY]),
        .hat = hid.gamepad_hat,
        .buttons = hid.gamepad_buttons,
    };
    generation = hid.gamepad_generation;
    hid_unlock();

    if (hid_port_send_report(HID_PORT_REPORT_GAMEPAD, &report, sizeof(report)) != ESP_OK) {
        return false;
    }

    hid_lock();
    if (hid.gamepad_generation == generation) {
        hid.gamepad_dirty = false;
    }
    hid_unlock();
    return true;
}

static void hid_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HID_RETRY_INTERVAL_MS));
        if (!hid_port_is_connected()) {
            continue;
        }
        if (hid_send_keyboard()) {
            continue;
        }
        if (hid_send_mouse()) {
            continue;
        }
        (void)hid_send_gamepad();
    }
}

static void hid_notify(void)
{
    if (hid.task != NULL) {
        xTaskNotifyGive(hid.task);
    }
}

esp_err_t solar_os_hid_init(void)
{
    if (!hid_port_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&hid_init_lock);
    if (hid.initialized) {
        portEXIT_CRITICAL(&hid_init_lock);
        return ESP_OK;
    }
    if (hid_initializing) {
        portEXIT_CRITICAL(&hid_init_lock);
        return ESP_ERR_INVALID_STATE;
    }
    hid_initializing = true;
    portEXIT_CRITICAL(&hid_init_lock);

    hid.mutex = xSemaphoreCreateMutex();
    hid.keyboard_queue = xQueueCreate(HID_KEYBOARD_QUEUE_LEN,
                                      sizeof(hid_keyboard_event_t));
    if (hid.mutex == NULL || hid.keyboard_queue == NULL) {
        goto failed;
    }
    hid.gamepad_hat = SOLAR_OS_HID_HAT_CENTERED;

    /* Report draining never runs with the flash cache disabled and does not
     * serve an ISR, so its stack can live in PSRAM on capable boards. */
    if (solar_os_task_create_pinned_external(hid_task,
                                             "solar_os_hid",
                                             HID_TASK_STACK,
                                             NULL,
                                             HID_TASK_PRIORITY,
                                             &hid.task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
        goto failed;
    }
    portENTER_CRITICAL(&hid_init_lock);
    hid.initialized = true;
    hid_initializing = false;
    portEXIT_CRITICAL(&hid_init_lock);
    return ESP_OK;

failed:
    if (hid.keyboard_queue != NULL) {
        vQueueDelete(hid.keyboard_queue);
        hid.keyboard_queue = NULL;
    }
    if (hid.mutex != NULL) {
        vSemaphoreDelete(hid.mutex);
        hid.mutex = NULL;
    }
    hid.task = NULL;
    portENTER_CRITICAL(&hid_init_lock);
    hid_initializing = false;
    portEXIT_CRITICAL(&hid_init_lock);
    return ESP_ERR_NO_MEM;
}

void solar_os_hid_get_status(solar_os_hid_status_t *status)
{
    if (status == NULL) {
        return;
    }
    const bool initialized = hid_initialized();
    *status = (solar_os_hid_status_t){
        .initialized = initialized,
        .connected = initialized && hid_port_is_connected(),
    };
}

static bool hid_keyboard_apply_key(hid_port_keyboard_report_t *report,
                                   uint16_t key,
                                   bool pressed)
{
    if ((key & SOLAR_OS_HID_KEY_MODIFIER_FLAG) != 0) {
        const uint8_t modifier = (uint8_t)key;
        if (modifier == 0 || (modifier & (modifier - 1U)) != 0) {
            return false;
        }
        if (pressed) {
            report->modifier |= modifier;
        } else {
            report->modifier &= (uint8_t)~modifier;
        }
        return true;
    }
    if (key == 0 || key > UINT8_MAX) {
        return false;
    }

    for (size_t index = 0; index < HID_KEYBOARD_KEYS_MAX; index++) {
        if (report->keycode[index] != (uint8_t)key) {
            continue;
        }
        if (pressed) {
            return true;
        }
        memmove(&report->keycode[index],
                &report->keycode[index + 1],
                HID_KEYBOARD_KEYS_MAX - index - 1U);
        report->keycode[HID_KEYBOARD_KEYS_MAX - 1U] = 0;
        return true;
    }
    if (!pressed) {
        return true;
    }
    for (size_t index = 0; index < HID_KEYBOARD_KEYS_MAX; index++) {
        if (report->keycode[index] == 0) {
            report->keycode[index] = (uint8_t)key;
            return true;
        }
    }
    return false;
}

static esp_err_t hid_keyboard_update(const uint16_t *keys,
                                     size_t key_count,
                                     bool pressed)
{
    if (keys == NULL || key_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }

    hid_lock();
    hid_port_keyboard_report_t next = hid.keyboard;
    for (size_t index = 0; index < key_count; index++) {
        if (!hid_keyboard_apply_key(&next, keys[index], pressed)) {
            hid_unlock();
            return ESP_ERR_INVALID_ARG;
        }
    }
    const hid_keyboard_event_t event = {.report = next};
    if (xQueueSend(hid.keyboard_queue, &event, 0) != pdTRUE) {
        hid_unlock();
        return ESP_ERR_TIMEOUT;
    }
    hid.keyboard = next;
    hid_unlock();
    hid_notify();
    return ESP_OK;
}

esp_err_t solar_os_hid_keyboard_press(const uint16_t *keys, size_t key_count)
{
    return hid_keyboard_update(keys, key_count, true);
}

esp_err_t solar_os_hid_keyboard_release(const uint16_t *keys, size_t key_count)
{
    return hid_keyboard_update(keys, key_count, false);
}

esp_err_t solar_os_hid_keyboard_release_all(void)
{
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }
    hid_lock();
    hid.keyboard = (hid_port_keyboard_report_t){0};
    const hid_keyboard_event_t event = {.report = hid.keyboard};
    const BaseType_t queued = xQueueSend(hid.keyboard_queue, &event, 0);
    hid_unlock();
    if (queued != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    hid_notify();
    return ESP_OK;
}

esp_err_t solar_os_hid_mouse_move(int32_t x, int32_t y)
{
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }
    hid_lock();
    int64_t next_x = (int64_t)hid.mouse_x + x;
    int64_t next_y = (int64_t)hid.mouse_y + y;
    if (next_x > HID_MOUSE_DELTA_LIMIT) next_x = HID_MOUSE_DELTA_LIMIT;
    if (next_x < -HID_MOUSE_DELTA_LIMIT) next_x = -HID_MOUSE_DELTA_LIMIT;
    if (next_y > HID_MOUSE_DELTA_LIMIT) next_y = HID_MOUSE_DELTA_LIMIT;
    if (next_y < -HID_MOUSE_DELTA_LIMIT) next_y = -HID_MOUSE_DELTA_LIMIT;
    hid.mouse_x = (int32_t)next_x;
    hid.mouse_y = (int32_t)next_y;
    hid.mouse_dirty = true;
    hid.mouse_generation++;
    hid_unlock();
    hid_notify();
    return ESP_OK;
}

esp_err_t solar_os_hid_mouse_button(uint8_t button, bool pressed)
{
    const uint8_t valid = SOLAR_OS_HID_MOUSE_LEFT | SOLAR_OS_HID_MOUSE_RIGHT |
        SOLAR_OS_HID_MOUSE_MIDDLE | SOLAR_OS_HID_MOUSE_BACK |
        SOLAR_OS_HID_MOUSE_FORWARD;
    if (button == 0 || (button & ~valid) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }
    hid_lock();
    if (pressed) {
        hid.mouse_buttons |= button;
    } else {
        hid.mouse_buttons &= (uint8_t)~button;
    }
    hid.mouse_dirty = true;
    hid.mouse_generation++;
    hid_unlock();
    hid_notify();
    return ESP_OK;
}

esp_err_t solar_os_hid_gamepad_axis(solar_os_hid_axis_t axis, int16_t value)
{
    if (axis < 0 || axis >= SOLAR_OS_HID_AXIS_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }
    hid_lock();
    hid.gamepad_axes[axis] = value;
    hid.gamepad_generation++;
    hid_unlock();
    return ESP_OK;
}

esp_err_t solar_os_hid_gamepad_button(uint8_t button, bool pressed)
{
    if (button < 1 || button > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t mask = 1UL << (button - 1U);
    hid_lock();
    if (pressed) {
        hid.gamepad_buttons |= mask;
    } else {
        hid.gamepad_buttons &= ~mask;
    }
    hid.gamepad_generation++;
    hid_unlock();
    return ESP_OK;
}

esp_err_t solar_os_hid_gamepad_hat(solar_os_hid_hat_t hat)
{
    if (hat < SOLAR_OS_HID_HAT_CENTERED || hat > SOLAR_OS_HID_HAT_UP_LEFT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }
    hid_lock();
    hid.gamepad_hat = (uint8_t)hat;
    hid.gamepad_generation++;
    hid_unlock();
    return ESP_OK;
}

esp_err_t solar_os_hid_gamepad_send(void)
{
    if (!hid_can_accept()) {
        return ESP_ERR_INVALID_STATE;
    }
    hid_lock();
    hid.gamepad_dirty = true;
    hid.gamepad_generation++;
    hid_unlock();
    hid_notify();
    return ESP_OK;
}

void solar_os_hid_release_all(void)
{
    if (!hid_initialized()) {
        return;
    }
    hid_lock();
    hid.keyboard = (hid_port_keyboard_report_t){0};
    const hid_keyboard_event_t event = {.report = hid.keyboard};
    /* Preserve already queued key transitions while the host is present. A
     * short script can otherwise enqueue press/release and have lifecycle
     * cleanup erase both before the HID task gets CPU time. If the host went
     * away, discard stale transitions so reconnecting cannot replay them. */
    if (!hid_port_is_connected() ||
        xQueueSend(hid.keyboard_queue, &event, 0) != pdTRUE) {
        (void)xQueueReset(hid.keyboard_queue);
        (void)xQueueSend(hid.keyboard_queue, &event, 0);
    }
    hid.mouse_buttons = 0;
    hid.mouse_dirty = true;
    hid.mouse_generation++;
    memset(hid.gamepad_axes, 0, sizeof(hid.gamepad_axes));
    hid.gamepad_buttons = 0;
    hid.gamepad_hat = SOLAR_OS_HID_HAT_CENTERED;
    hid.gamepad_dirty = true;
    hid.gamepad_generation++;
    hid_unlock();
    hid_notify();
}
