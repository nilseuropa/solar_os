#include "solar_os_input_actions.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"

#define INPUT_ACTION_QUEUE_DEPTH 8U
#define INPUT_ACTION_TASK_STACK 6144U
#define INPUT_ACTION_TASK_PRIORITY 3U
#define INPUT_ACTION_VIRTUAL_SOURCE "input.emit"

typedef struct {
    bool active;
    bool triggered;
    uint64_t last_trigger_ms;
    solar_os_input_action_binding_t binding;
} input_action_rule_t;

typedef struct {
    uint32_t id;
    uint32_t generation;
} input_action_queue_item_t;

typedef struct {
    uint32_t next_id;
    uint32_t generation;
    solar_os_input_action_runner_t runner;
    QueueHandle_t queue;
    TaskHandle_t worker;
    bool worker_starting;
    volatile bool worker_done;
    bool initialized;
    bool running;
    solar_os_input_source_t virtual_source;
    portMUX_TYPE lock;
} input_action_state_t;

static const char *TAG = "input-actions";
static input_action_state_t state = {
    .next_id = 1U,
    .generation = 1U,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static EXT_RAM_BSS_ATTR input_action_rule_t
    input_action_rules[SOLAR_OS_INPUT_ACTION_MAX_BINDINGS];

static uint64_t input_action_now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static input_action_rule_t *input_action_find_locked(uint32_t id)
{
    for (size_t i = 0; i < SOLAR_OS_INPUT_ACTION_MAX_BINDINGS; i++) {
        if (input_action_rules[i].active &&
            input_action_rules[i].binding.id == id) {
            return &input_action_rules[i];
        }
    }
    return NULL;
}

static void input_action_worker(void *context)
{
    (void)context;
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (;;) {
        portENTER_CRITICAL(&state.lock);
        const bool starting = state.worker_starting;
        portEXIT_CRITICAL(&state.lock);
        if (!starting) {
            break;
        }
        vTaskDelay(1);
    }

    for (;;) {
        input_action_queue_item_t item = {0};
        if (xQueueReceive(state.queue, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
            portENTER_CRITICAL(&state.lock);
            const bool running = state.running;
            const bool queue_empty = uxQueueMessagesWaiting(state.queue) == 0U;
            if (!running || queue_empty) {
                if (state.worker == self) {
                    state.worker = NULL;
                }
                state.worker_done = true;
            }
            portEXIT_CRITICAL(&state.lock);
            if (!running || queue_empty) {
                break;
            }
            continue;
        }

        char command[SOLAR_OS_INPUT_ACTION_COMMAND_MAX] = {0};
        solar_os_input_action_runner_t runner = NULL;
        portENTER_CRITICAL(&state.lock);
        input_action_rule_t *rule = input_action_find_locked(item.id);
        if (state.running && item.generation == state.generation && rule != NULL) {
            strlcpy(command, rule->binding.command, sizeof(command));
        }
        runner = state.runner;
        portEXIT_CRITICAL(&state.lock);

        if (command[0] == '\0') {
            continue;
        }
        if (runner == NULL) {
            ESP_LOGW(TAG,
                     "binding %lu has no command runner",
                     (unsigned long)item.id);
            continue;
        }
        const esp_err_t err = runner(command);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "binding %lu command failed: %s",
                     (unsigned long)item.id,
                     esp_err_to_name(err));
        }
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t input_action_ensure_worker(void)
{
    portENTER_CRITICAL(&state.lock);
    if (!state.running) {
        portEXIT_CRITICAL(&state.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (state.worker != NULL || state.worker_starting) {
        portEXIT_CRITICAL(&state.lock);
        return ESP_OK;
    }
    state.worker_starting = true;
    state.worker_done = false;
    portEXIT_CRITICAL(&state.lock);

    TaskHandle_t worker = NULL;
    if (solar_os_task_create_pinned_internal(input_action_worker,
                                             "input_actions",
                                             INPUT_ACTION_TASK_STACK,
                                             NULL,
                                             INPUT_ACTION_TASK_PRIORITY,
                                             &worker,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        portENTER_CRITICAL(&state.lock);
        state.worker_starting = false;
        state.worker_done = true;
        portEXIT_CRITICAL(&state.lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&state.lock);
    state.worker = worker;
    state.worker_starting = false;
    portEXIT_CRITICAL(&state.lock);
    return ESP_OK;
}

static esp_err_t input_action_ensure_queue(void)
{
    portENTER_CRITICAL(&state.lock);
    QueueHandle_t queue = state.queue;
    portEXIT_CRITICAL(&state.lock);
    if (queue != NULL) {
        return ESP_OK;
    }

    queue = solar_os_queue_create(INPUT_ACTION_QUEUE_DEPTH,
                                  sizeof(input_action_queue_item_t));
    if (queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&state.lock);
    state.queue = queue;
    portEXIT_CRITICAL(&state.lock);
    return ESP_OK;
}

static void input_action_note_drop(uint32_t id)
{
    portENTER_CRITICAL(&state.lock);
    input_action_rule_t *rule = input_action_find_locked(id);
    if (rule != NULL && rule->binding.dropped_count != UINT32_MAX) {
        rule->binding.dropped_count++;
    }
    portEXIT_CRITICAL(&state.lock);
}

static void input_action_observe(const solar_os_input_gesture_event_t *event,
                                 void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }
    solar_os_input_source_info_t source;
    if (!solar_os_input_source_get_info(event->source, &source)) {
        return;
    }

    input_action_queue_item_t matches[SOLAR_OS_INPUT_ACTION_MAX_BINDINGS];
    size_t match_count = 0;
    const uint64_t now_ms = input_action_now_ms();
    QueueHandle_t queue = NULL;
    portENTER_CRITICAL(&state.lock);
    if (!state.running) {
        portEXIT_CRITICAL(&state.lock);
        return;
    }
    queue = state.queue;
    for (size_t i = 0; i < SOLAR_OS_INPUT_ACTION_MAX_BINDINGS; i++) {
        input_action_rule_t *rule = &input_action_rules[i];
        if (!rule->active ||
            rule->binding.gesture != event->gesture ||
            (!rule->binding.any_source &&
             strcmp(rule->binding.source, source.name) != 0) ||
            (!rule->binding.any_direction &&
             rule->binding.direction != event->direction) ||
            (rule->triggered &&
             now_ms - rule->last_trigger_ms < rule->binding.cooldown_ms)) {
            continue;
        }
        rule->triggered = true;
        rule->last_trigger_ms = now_ms;
        if (rule->binding.trigger_count != UINT32_MAX) {
            rule->binding.trigger_count++;
        }
        matches[match_count++] = (input_action_queue_item_t) {
            .id = rule->binding.id,
            .generation = state.generation,
        };
    }
    portEXIT_CRITICAL(&state.lock);

    uint32_t queued_ids[SOLAR_OS_INPUT_ACTION_MAX_BINDINGS];
    size_t queued_count = 0U;
    for (size_t i = 0; i < match_count; i++) {
        if (queue == NULL || xQueueSend(queue, &matches[i], 0) != pdTRUE) {
            input_action_note_drop(matches[i].id);
        } else {
            queued_ids[queued_count++] = matches[i].id;
        }
    }
    if (queued_count > 0U && input_action_ensure_worker() != ESP_OK) {
        (void)xQueueReset(queue);
        for (size_t i = 0; i < queued_count; i++) {
            input_action_note_drop(queued_ids[i]);
        }
    }
}

esp_err_t solar_os_input_actions_init(void)
{
    portENTER_CRITICAL(&state.lock);
    if (state.initialized) {
        portEXIT_CRITICAL(&state.lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&state.lock);

    portENTER_CRITICAL(&state.lock);
    state.initialized = true;
    portEXIT_CRITICAL(&state.lock);
    return ESP_OK;
}

void solar_os_input_actions_set_runner(solar_os_input_action_runner_t runner)
{
    portENTER_CRITICAL(&state.lock);
    state.runner = runner;
    portEXIT_CRITICAL(&state.lock);
}

esp_err_t solar_os_input_actions_start(void)
{
    esp_err_t err = solar_os_input_actions_init();
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&state.lock);
    const bool running = state.running;
    portEXIT_CRITICAL(&state.lock);
    if (running) {
        return ESP_ERR_INVALID_STATE;
    }

    err = input_action_ensure_queue();
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&state.lock);
    state.running = true;
    state.generation++;
    if (state.generation == 0U) {
        state.generation = 1U;
    }
    portEXIT_CRITICAL(&state.lock);
    err = solar_os_input_gesture_observer_register(input_action_observe, NULL);
    if (err != ESP_OK) {
        solar_os_input_actions_stop();
        return err;
    }
    return ESP_OK;
}

void solar_os_input_actions_stop(void)
{
    QueueHandle_t queue = NULL;
    TaskHandle_t worker = NULL;
    portENTER_CRITICAL(&state.lock);
    state.running = false;
    state.generation++;
    if (state.generation == 0U) {
        state.generation = 1U;
    }
    queue = state.queue;
    worker = state.worker;
    portEXIT_CRITICAL(&state.lock);
    solar_os_input_gesture_observer_unregister(input_action_observe, NULL);
    if (queue != NULL) {
        (void)xQueueReset(queue);
    }
    if (worker == xTaskGetCurrentTaskHandle()) {
        return;
    }
    if (solar_os_task_wait_done(worker,
                                &state.worker_done,
                                SOLAR_OS_TASK_STOP_WAIT_MS)) {
        portENTER_CRITICAL(&state.lock);
        state.worker_done = false;
        portEXIT_CRITICAL(&state.lock);
    }
}

bool solar_os_input_actions_running(void)
{
    portENTER_CRITICAL(&state.lock);
    const bool running = state.running;
    portEXIT_CRITICAL(&state.lock);
    return running;
}

bool solar_os_input_actions_worker_active(void)
{
    portENTER_CRITICAL(&state.lock);
    const bool active = state.worker != NULL || state.worker_starting;
    portEXIT_CRITICAL(&state.lock);
    return active;
}

esp_err_t solar_os_input_actions_bind(
    const char *source,
    solar_os_input_gesture_t gesture,
    bool any_direction,
    solar_os_input_gesture_direction_t direction,
    uint32_t cooldown_ms,
    const char *command,
    uint32_t *binding_id)
{
    const bool any_source = source == NULL || strcmp(source, "*") == 0;
    if ((!any_source &&
         (source[0] == '\0' ||
          strnlen(source, SOLAR_OS_INPUT_SOURCE_NAME_MAX) >=
              SOLAR_OS_INPUT_SOURCE_NAME_MAX)) ||
        gesture < SOLAR_OS_INPUT_GESTURE_FLICK ||
        gesture >= SOLAR_OS_INPUT_GESTURE_COUNT ||
        (!any_direction &&
         (direction < SOLAR_OS_INPUT_GESTURE_DIRECTION_NONE ||
          direction >= SOLAR_OS_INPUT_GESTURE_DIRECTION_COUNT)) ||
        cooldown_ms > SOLAR_OS_INPUT_ACTION_COOLDOWN_MAX_MS ||
        command == NULL || command[0] == '\0' ||
        strnlen(command, SOLAR_OS_INPUT_ACTION_COMMAND_MAX) >=
            SOLAR_OS_INPUT_ACTION_COMMAND_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&state.lock);
    for (size_t i = 0; i < SOLAR_OS_INPUT_ACTION_MAX_BINDINGS; i++) {
        input_action_rule_t *rule = &input_action_rules[i];
        if (rule->active) {
            continue;
        }
        memset(rule, 0, sizeof(*rule));
        rule->active = true;
        rule->binding.id = state.next_id++;
        if (state.next_id == 0U) {
            state.next_id = 1U;
        }
        rule->binding.any_source = any_source;
        if (!any_source) {
            strlcpy(rule->binding.source, source, sizeof(rule->binding.source));
        }
        rule->binding.gesture = gesture;
        rule->binding.any_direction = any_direction;
        rule->binding.direction = direction;
        rule->binding.cooldown_ms = cooldown_ms;
        strlcpy(rule->binding.command, command, sizeof(rule->binding.command));
        if (binding_id != NULL) {
            *binding_id = rule->binding.id;
        }
        result = ESP_OK;
        break;
    }
    portEXIT_CRITICAL(&state.lock);
    return result;
}

size_t solar_os_input_actions_count(void)
{
    size_t count = 0;
    portENTER_CRITICAL(&state.lock);
    for (size_t i = 0; i < SOLAR_OS_INPUT_ACTION_MAX_BINDINGS; i++) {
        if (input_action_rules[i].active) {
            count++;
        }
    }
    portEXIT_CRITICAL(&state.lock);
    return count;
}

bool solar_os_input_actions_get(size_t index,
                                solar_os_input_action_binding_t *binding)
{
    if (binding == NULL) {
        return false;
    }
    bool found = false;
    size_t current = 0;
    portENTER_CRITICAL(&state.lock);
    for (size_t i = 0; i < SOLAR_OS_INPUT_ACTION_MAX_BINDINGS; i++) {
        if (!input_action_rules[i].active) {
            continue;
        }
        if (current++ == index) {
            *binding = input_action_rules[i].binding;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&state.lock);
    return found;
}

esp_err_t solar_os_input_actions_unbind(uint32_t binding_id)
{
    if (binding_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&state.lock);
    input_action_rule_t *rule = input_action_find_locked(binding_id);
    if (rule != NULL) {
        memset(rule, 0, sizeof(*rule));
        result = ESP_OK;
    }
    portEXIT_CRITICAL(&state.lock);
    return result;
}

size_t solar_os_input_actions_clear(void)
{
    size_t count = 0;
    QueueHandle_t queue = NULL;
    portENTER_CRITICAL(&state.lock);
    for (size_t i = 0; i < SOLAR_OS_INPUT_ACTION_MAX_BINDINGS; i++) {
        if (input_action_rules[i].active) {
            memset(&input_action_rules[i], 0, sizeof(input_action_rules[i]));
            count++;
        }
    }
    state.next_id = 1U;
    state.generation++;
    if (state.generation == 0U) {
        state.generation = 1U;
    }
    queue = state.queue;
    portEXIT_CRITICAL(&state.lock);
    if (queue != NULL) {
        (void)xQueueReset(queue);
    }
    return count;
}

esp_err_t solar_os_input_actions_emit_key(const char *name)
{
    uint8_t key = 0;
    uint8_t modifiers = 0;
    if (!solar_os_input_parse_key_chord(name, &key, &modifiers)) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_input_source_t source = SOLAR_OS_INPUT_SOURCE_INVALID;
    portENTER_CRITICAL(&state.lock);
    source = state.virtual_source;
    portEXIT_CRITICAL(&state.lock);
    solar_os_input_source_info_t info;
    if (source == SOLAR_OS_INPUT_SOURCE_INVALID ||
        !solar_os_input_source_get_info(source, &info) ||
        strcmp(info.name, INPUT_ACTION_VIRTUAL_SOURCE) != 0) {
        esp_err_t err = solar_os_input_keyboard_source_open(
            INPUT_ACTION_VIRTUAL_SOURCE, true, &source);
        if (err != ESP_OK) {
            return err;
        }
        portENTER_CRITICAL(&state.lock);
        state.virtual_source = source;
        portEXIT_CRITICAL(&state.lock);
    }

    const uint16_t physical_key = (uint16_t)key + 0x100U;
    esp_err_t err = solar_os_input_write_key(source,
                                             physical_key,
                                             SOLAR_OS_INPUT_USAGE_NONE,
                                             key,
                                             modifiers,
                                             SOLAR_OS_INPUT_KEY_PRESS);
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_input_write_key(source,
                                   physical_key,
                                   SOLAR_OS_INPUT_USAGE_NONE,
                                   key,
                                   modifiers,
                                   SOLAR_OS_INPUT_KEY_RELEASE);
    if (err != ESP_OK) {
        solar_os_input_source_release_all(source);
    }
    return err;
}
