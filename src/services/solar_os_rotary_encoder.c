#include "solar_os_rotary_encoder.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_task.h"

/*
 * Two-wire quadrature rotary encoder (A/B) decoded with the well-known
 * Buxtronix full-step state table - the same table LilyGoLib's Rotary.h
 * derives from - so one detent (click) yields exactly one event, with
 * contact bounce absorbed by the state machine rather than by timing.
 *
 * Each detent is delivered as a single Up/Down key tap through the same
 * input path the board buttons use, so the shell, menus, and apps see it
 * exactly like Elecrow's rotary buttons. The encoder's push switch is not
 * handled here; declare it in SOLAR_OS_BOARD_BUTTONS (as Enter) instead.
 */

#define ROTARY_POLL_MS 1U
#define ROTARY_TASK_STACK 2560U
#define ROTARY_TASK_PRIORITY (tskIDLE_PRIORITY + 2)

/* Turning clockwise scrolls "forward" (next item = Down). Swap these two if
 * the wheel on a given board feels backwards. */
#define ROTARY_KEY_CLOCKWISE SOLAR_OS_KEY_DOWN
#define ROTARY_KEY_COUNTER_CLOCKWISE SOLAR_OS_KEY_UP

#define R_START 0x0U
#define R_CW_FINAL 0x1U
#define R_CW_BEGIN 0x2U
#define R_CW_NEXT 0x3U
#define R_CCW_BEGIN 0x4U
#define R_CCW_FINAL 0x5U
#define R_CCW_NEXT 0x6U
#define DIR_CW 0x10U
#define DIR_CCW 0x20U
#define DIR_MASK 0x30U

static const uint8_t ttable[7][4] = {
    /* R_START */     {R_START,    R_CW_BEGIN,  R_CCW_BEGIN, R_START},
    /* R_CW_FINAL */  {R_CW_NEXT,  R_START,     R_CW_FINAL,  R_START | DIR_CW},
    /* R_CW_BEGIN */  {R_CW_NEXT,  R_CW_BEGIN,  R_START,     R_START},
    /* R_CW_NEXT */   {R_CW_NEXT,  R_CW_BEGIN,  R_CW_FINAL,  R_START},
    /* R_CCW_BEGIN */ {R_CCW_NEXT, R_START,     R_CCW_BEGIN, R_START},
    /* R_CCW_FINAL */ {R_CCW_NEXT, R_CCW_FINAL, R_START,     R_START | DIR_CCW},
    /* R_CCW_NEXT */  {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    int pin_a;
    int pin_b;
    uint8_t state;
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    uint32_t clockwise;
    uint32_t counter_clockwise;
    uint32_t dropped;
} solar_os_rotary_device_t;

static const char *TAG = "rotary";
static solar_os_rotary_device_t rotary;

static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                int *pin_a,
                                int *pin_b)
{
    if (bindings == NULL || pin_a == NULL || pin_b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pin_a = -1;
    *pin_b = -1;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind != SOLAR_OS_EXPANSION_BINDING_GPIO) {
            return ESP_ERR_INVALID_ARG;
        }
        if (binding_role_is(binding, "a") && *pin_a < 0) {
            *pin_a = binding->value;
        } else if (binding_role_is(binding, "b") && *pin_b < 0) {
            *pin_b = binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return (*pin_a >= 0 && *pin_b >= 0 && *pin_a != *pin_b) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void rotary_worker(void *arg)
{
    solar_os_rotary_device_t *device = arg;
    while (!device->stop_requested) {
        const uint8_t pinstate = (uint8_t)((gpio_get_level(device->pin_b) << 1) |
                                           gpio_get_level(device->pin_a));
        device->state = ttable[device->state & 0x0FU][pinstate];
        const uint8_t dir = device->state & DIR_MASK;
        if (dir != 0U) {
            const uint8_t key = (dir == DIR_CW) ? ROTARY_KEY_CLOCKWISE : ROTARY_KEY_COUNTER_CLOCKWISE;
            if (solar_os_input_write_char(device->input_source, (char)key) == ESP_OK) {
                if (dir == DIR_CW) {
                    device->clockwise++;
                } else {
                    device->counter_clockwise++;
                }
            } else {
                device->dropped++;
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ROTARY_POLL_MS));
    }
    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(solar_os_rotary_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
    device->pin_a = -1;
    device->pin_b = -1;
}

esp_err_t solar_os_rotary_encoder_attach(const char *name,
                                         const solar_os_expansion_binding_t *bindings,
                                         size_t binding_count)
{
    int pin_a = -1;
    int pin_b = -1;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (rotary.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &pin_a, &pin_b), TAG, "invalid bindings");

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << (uint32_t)pin_a) | (1ULL << (uint32_t)pin_b),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "gpio config failed");

    clear_device(&rotary);
    rotary.active = true;
    rotary.pin_a = pin_a;
    rotary.pin_b = pin_b;
    rotary.state = R_START;
    strlcpy(rotary.name, name, sizeof(rotary.name));

    esp_err_t err = solar_os_input_key_source_open(rotary.name,
                                                  SOLAR_OS_INPUT_SOURCE_BUTTONS,
                                                  &rotary.input_source);
    if (err != ESP_OK) {
        clear_device(&rotary);
        return err;
    }
    if (solar_os_task_create_pinned_internal(rotary_worker,
                                             rotary.name,
                                             ROTARY_TASK_STACK,
                                             &rotary,
                                             ROTARY_TASK_PRIORITY,
                                             &rotary.worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(&rotary);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "%s attached: A GPIO%d, B GPIO%d", name, pin_a, pin_b);
    return ESP_OK;
}

esp_err_t solar_os_rotary_encoder_detach(const char *name)
{
    if (!rotary.active || name == NULL || strcmp(rotary.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    rotary.stop_requested = true;
    if (rotary.worker_task != NULL) {
        (void)xTaskNotifyGive(rotary.worker_task);
    }
    if (!solar_os_task_wait_done(rotary.worker_task, &rotary.worker_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "%s detached: %lu cw, %lu ccw, %lu dropped", name,
             (unsigned long)rotary.clockwise, (unsigned long)rotary.counter_clockwise,
             (unsigned long)rotary.dropped);
    clear_device(&rotary);
    return ESP_OK;
}
