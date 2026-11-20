#include "solar_os_qemu_input.h"
#include "solar_os_qemu_input_codec.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "solar_os_board.h"
#include "solar_os_task.h"

static struct {
    bool active;
    volatile bool stop, done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    TaskHandle_t task;
    solar_os_qemu_input_state_t state;
} device;
static const char *TAG = "qemu_input";

static uint32_t read_register(uint32_t offset)
{
    return REG_READ(SOLAR_OS_QEMU_INPUT_BASE + offset);
}

static void write_register(uint32_t offset)
{
    REG_WRITE(SOLAR_OS_QEMU_INPUT_BASE + offset, 1);
}

static void worker(void *arg)
{
    (void)arg;
    uint32_t generation = read_register(SOLAR_OS_QEMU_INPUT_REG_GENERATION);
    bool blocked = false;
    TickType_t blocked_since = 0;
    while (!device.stop) {
        for (unsigned batch = 0; batch < 32 && !device.stop; batch++) {
            const uint32_t current = read_register(SOLAR_OS_QEMU_INPUT_REG_GENERATION);
            if (generation != current) {
                if (solar_os_qemu_input_release_all(&device.state) != ESP_OK) break;
                write_register(SOLAR_OS_QEMU_INPUT_REG_POP);
                generation = current;
                blocked = false;
            }
            if (!read_register(SOLAR_OS_QEMU_INPUT_REG_COUNT)) break;
            solar_os_qemu_input_event_t event;
            /* TYPE latches the record. Keep these volatile reads ordered. */
            event.type = read_register(SOLAR_OS_QEMU_INPUT_REG_TYPE);
            event.a = read_register(SOLAR_OS_QEMU_INPUT_REG_A);
            event.b = read_register(SOLAR_OS_QEMU_INPUT_REG_B);
            event.c = read_register(SOLAR_OS_QEMU_INPUT_REG_C);
            event.d = read_register(SOLAR_OS_QEMU_INPUT_REG_D);
            const esp_err_t err = solar_os_qemu_input_process(&device.state, &event);
            if (err == ESP_ERR_NO_MEM) {
                const TickType_t now = xTaskGetTickCount();
                if (!blocked) { blocked = true; blocked_since = now; }
                /* A held-key-slot limit must not block later key releases. */
                if ((TickType_t)(now - blocked_since) >= pdMS_TO_TICKS(64)) {
                    write_register(SOLAR_OS_QEMU_INPUT_REG_RESET);
                    blocked = false;
                }
                break;
            }
            blocked = false;
            if (err != ESP_OK) {
                if (solar_os_qemu_input_release_all(&device.state) != ESP_OK) break;
                ESP_LOGW(TAG, "discarded invalid input event: %s", esp_err_to_name(err));
            }
            write_register(SOLAR_OS_QEMU_INPUT_REG_POP);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
    }
    device.done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(void)
{
    if (device.state.keyboard) solar_os_input_source_close(device.state.keyboard);
    if (device.state.touch) solar_os_input_source_close(device.state.touch);
    if (device.state.mouse) solar_os_input_source_close(device.state.mouse);
    memset(&device, 0, sizeof(device));
}

esp_err_t solar_os_qemu_input_attach(const char *name,
                                     const solar_os_expansion_binding_t *bindings,
                                     size_t binding_count)
{
    (void)bindings;
    if (!name || !name[0] || strlen(name) >= sizeof(device.name) || binding_count) return ESP_ERR_INVALID_ARG;
    if (device.active) return ESP_ERR_INVALID_STATE;
    if (read_register(SOLAR_OS_QEMU_INPUT_REG_MAGIC) != SOLAR_OS_QEMU_INPUT_MAGIC ||
        read_register(SOLAR_OS_QEMU_INPUT_REG_VERSION) != SOLAR_OS_QEMU_INPUT_VERSION) {
        ESP_LOGE(TAG, "native input device missing; launch with the PlatformIO qemu target");
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(&device, 0, sizeof(device));
    strcpy(device.name, name);
    device.state.width = SOLAR_OS_BOARD_DISPLAY_WIDTH;
    device.state.height = SOLAR_OS_BOARD_DISPLAY_HEIGHT;
    esp_err_t err = solar_os_input_keyboard_source_open("qemu-keyboard", true, &device.state.keyboard);
    const uint32_t pointer_caps = SOLAR_OS_INPUT_CAP_POINTER_BUTTONS |
        SOLAR_OS_INPUT_CAP_SCROLL | SOLAR_OS_INPUT_CAP_AXIS_EVENTS;
    if (err == ESP_OK) err = solar_os_input_source_open_typed("qemu-touch", SOLAR_OS_INPUT_SOURCE_TOUCH,
        pointer_caps | SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE, true, &device.state.touch);
    if (err == ESP_OK) err = solar_os_input_source_open_typed("qemu-mouse", SOLAR_OS_INPUT_SOURCE_MOUSE,
        pointer_caps | SOLAR_OS_INPUT_CAP_POINTER_RELATIVE, true, &device.state.mouse);
    if (err != ESP_OK) { clear_device(); return err; }
    write_register(SOLAR_OS_QEMU_INPUT_REG_RESET);
    device.active = true;
    if (solar_os_task_create_pinned_internal(worker, "qemu-input", 3072, NULL,
        tskIDLE_PRIORITY + 1, &device.task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s attached: HID keyboard, touch, relative mouse", name);
    return ESP_OK;
}

esp_err_t solar_os_qemu_input_detach(const char *name)
{
    if (!device.active || !name || strcmp(name, device.name)) return ESP_ERR_NOT_FOUND;
    if (!device.done) {
        device.stop = true;
        xTaskNotifyGive(device.task);
        if (!solar_os_task_wait_done(device.task, &device.done, SOLAR_OS_TASK_STOP_WAIT_MS)) return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = solar_os_qemu_input_release_all(&device.state);
    if (err != ESP_OK) return err;
    clear_device();
    return ESP_OK;
}
