#include "solar_os_gt911.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gt911.h"
#include "solar_os_display.h"
#include "solar_os_input.h"
#include "solar_os_task.h"

#define GT911_POLL_MS 10U
#define GT911_TASK_STACK 3072U
#define GT911_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

typedef struct {
    bool active, pressed;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address, rotation, pointer_id;
    int irq_pin;
    uint16_t width, height;
    int16_t x, y;
    solar_os_input_source_t source;
    TaskHandle_t worker_task;
} gt911_device_t;

static const char *TAG = "gt911";
static gt911_device_t touch;

static void worker(void *arg);

static esp_err_t parse(const solar_os_expansion_binding_t *bindings, size_t count,
                       gt911_device_t *device)
{
    bool bus=false, addr=false, irq=false, rotation=false;
    if (!bindings || !device) return ESP_ERR_INVALID_ARG;
    device->irq_pin = -1;
    for (size_t i=0; i<count; ++i) {
        const solar_os_expansion_binding_t *b=&bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !bus) {
            strlcpy(device->bus, b->target, sizeof(device->bus)); bus=true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS && !addr &&
                   (b->value == GT911_ADDRESS ||
                    b->value == GT911_ALTERNATE_ADDRESS)) {
            device->address=(uint8_t)b->value; addr=true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO && !irq &&
                   strcmp(b->role, "irq") == 0) {
            device->irq_pin=b->value; irq=true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !rotation &&
                   strcmp(b->role, "rotation") == 0 && b->value >= 0 && b->value <= 3) {
            device->rotation=(uint8_t)b->value; rotation=true;
        } else return ESP_ERR_INVALID_ARG;
    }
    return bus && addr && irq && rotation ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear(void)
{
    if (touch.source != SOLAR_OS_INPUT_SOURCE_INVALID)
        solar_os_input_source_close(touch.source);
    gt911_deinit();
    memset(&touch, 0, sizeof(touch));
}

esp_err_t solar_os_gt911_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t binding_count)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (touch.active) return ESP_ERR_INVALID_STATE;
    gt911_device_t candidate={0};
    ESP_RETURN_ON_ERROR(parse(bindings, binding_count, &candidate), TAG, "invalid bindings");
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(SOLAR_OS_DISPLAY_PRIMARY_TARGET, &target) ||
        !target.width || !target.height) return ESP_ERR_NOT_FOUND;
    candidate.width=target.width; candidate.height=target.height;
    ESP_RETURN_ON_ERROR(gt911_init(candidate.bus, candidate.address, candidate.irq_pin),
                        TAG, "controller init failed");
    strlcpy(candidate.name, name, sizeof(candidate.name));
    esp_err_t err=solar_os_input_touch_source_open(candidate.name, &candidate.source);
    if (err != ESP_OK) { gt911_deinit(); return err; }
    candidate.active=true;
    touch=candidate;
    if (solar_os_task_create_pinned_internal(worker, touch.name, GT911_TASK_STACK,
            &touch, GT911_TASK_PRIORITY, &touch.worker_task, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t solar_os_gt911_detach(const char *name)
{
    if (!touch.active || !name || strcmp(touch.name,name)) return ESP_ERR_NOT_FOUND;
    touch.stop_requested = true;
    (void)xTaskNotifyGive(touch.worker_task);
    if (!solar_os_task_wait_done(touch.worker_task, &touch.worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    clear(); return ESP_OK;
}

static void poll_device(gt911_device_t *device)
{
    if (device == NULL || !device->active) return;
    gt911_sample_t raw;
    if (gt911_read(&raw) != ESP_OK) return;
    uint16_t x=0,y=0;
    if (raw.touched) {
        const uint16_t nw=(device->rotation&1U)?device->height:device->width;
        const uint16_t nh=(device->rotation&1U)?device->width:device->height;
        if (raw.x>=nw || raw.y>=nh) return;
        switch (device->rotation) {
        case 0: x=raw.x; y=raw.y; break;
        case 1: x=raw.y; y=(nw-1U)-raw.x; break;
        case 2: x=(nw-1U)-raw.x; y=(nh-1U)-raw.y; break;
        default: x=(nh-1U)-raw.y; y=raw.x; break;
        }
    }
    solar_os_input_pointer_action_t action;
    if (raw.touched && !device->pressed) action=SOLAR_OS_INPUT_POINTER_PRESS;
    else if (!raw.touched && device->pressed) action=SOLAR_OS_INPUT_POINTER_RELEASE;
    else if (raw.touched && (x!=(uint16_t)device->x || y!=(uint16_t)device->y))
        action=SOLAR_OS_INPUT_POINTER_MOVE;
    else return;
    const int16_t nx=raw.touched?(int16_t)x:device->x;
    const int16_t ny=raw.touched?(int16_t)y:device->y;
    solar_os_input_pointer_event_t event={
        .pointer_id=raw.touched?raw.id:device->pointer_id,
        .buttons=raw.touched?SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY:0,
        .mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE, .action=action,
        .x=nx, .y=ny, .delta_x=(int16_t)(nx-device->x), .delta_y=(int16_t)(ny-device->y),
    };
    strlcpy(event.target, SOLAR_OS_DISPLAY_PRIMARY_TARGET, sizeof(event.target));
    if (solar_os_input_write_pointer(device->source,&event)==ESP_OK) {
        device->pressed=raw.touched; device->pointer_id=event.pointer_id;
        device->x=nx; device->y=ny;
    }
}

static void worker(void *arg)
{
    gt911_device_t *device = arg;
    while (!device->stop_requested) {
        poll_device(device);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(GT911_POLL_MS));
    }
    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}
