#include "solar_os_work.h"

#include <string.h>

#include "freertos/queue.h"
#include "solar_os_log.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"

#define SOLAR_OS_WORK_QUEUE_LEN 8U
#define SOLAR_OS_WORK_DEFAULT_PRIORITY (tskIDLE_PRIORITY + 1)

typedef enum {
    SOLAR_OS_WORK_SLOT_FREE = 0,
    SOLAR_OS_WORK_SLOT_QUEUED,
    SOLAR_OS_WORK_SLOT_RUNNING,
} solar_os_work_slot_state_t;

typedef struct {
    uint32_t generation;
    solar_os_work_slot_state_t state;
    solar_os_work_fn callback;
    void *arg;
    UBaseType_t priority;
    char name[SOLAR_OS_WORK_NAME_MAX];
} solar_os_work_slot_t;

typedef struct {
    uint32_t generation;
    uint8_t slot;
} solar_os_work_item_t;

static solar_os_work_slot_t work_slots[SOLAR_OS_WORK_QUEUE_LEN];
static portMUX_TYPE work_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t work_queue;
static TaskHandle_t work_task;
static bool work_initialized;
static bool work_initializing;
static uint32_t work_next_generation;
static uint32_t work_submitted;
static uint32_t work_completed;
static uint32_t work_cancelled;
static uint32_t work_rejected;
static uint8_t work_current_slot = SOLAR_OS_WORK_HANDLE_INVALID_SLOT;
static const char *TAG = "solar_os_work";

static uint32_t work_allocate_generation_locked(void)
{
    work_next_generation++;
    if (work_next_generation == 0) {
        work_next_generation++;
    }
    return work_next_generation;
}

static void work_clear_slot_locked(solar_os_work_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    const uint32_t generation = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = generation;
}

static void solar_os_work_task(void *arg)
{
    (void)arg;

    while (true) {
        solar_os_work_item_t item;
        if (xQueueReceive(work_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        solar_os_work_fn callback = NULL;
        void *callback_arg = NULL;
        UBaseType_t priority = SOLAR_OS_WORK_DEFAULT_PRIORITY;
        char name[SOLAR_OS_WORK_NAME_MAX] = {0};

        portENTER_CRITICAL(&work_lock);
        if (item.slot < SOLAR_OS_WORK_QUEUE_LEN) {
            solar_os_work_slot_t *slot = &work_slots[item.slot];
            if (slot->state == SOLAR_OS_WORK_SLOT_QUEUED &&
                slot->generation == item.generation) {
                slot->state = SOLAR_OS_WORK_SLOT_RUNNING;
                callback = slot->callback;
                callback_arg = slot->arg;
                priority = slot->priority;
                strlcpy(name, slot->name, sizeof(name));
                work_current_slot = item.slot;
            }
        }
        portEXIT_CRITICAL(&work_lock);

        if (callback == NULL) {
            continue;
        }

        vTaskPrioritySet(NULL, priority);
        SOLAR_OS_LOGI(TAG, "start: %s", name);
        callback(callback_arg);
        const UBaseType_t high_water = uxTaskGetStackHighWaterMark(NULL);
        SOLAR_OS_LOGI(TAG,
                      "done: %s stack_high_water=%u",
                      name,
                      (unsigned)high_water);
        vTaskPrioritySet(NULL, SOLAR_OS_WORK_DEFAULT_PRIORITY);

        portENTER_CRITICAL(&work_lock);
        solar_os_work_slot_t *slot = &work_slots[item.slot];
        if (slot->state == SOLAR_OS_WORK_SLOT_RUNNING &&
            slot->generation == item.generation) {
            work_clear_slot_locked(slot);
            work_completed++;
        }
        if (work_current_slot == item.slot) {
            work_current_slot = SOLAR_OS_WORK_HANDLE_INVALID_SLOT;
        }
        portEXIT_CRITICAL(&work_lock);
    }
}

esp_err_t solar_os_work_init(void)
{
    while (true) {
        portENTER_CRITICAL(&work_lock);
        if (work_initialized) {
            portEXIT_CRITICAL(&work_lock);
            return ESP_OK;
        }
        if (!work_initializing) {
            work_initializing = true;
            portEXIT_CRITICAL(&work_lock);
            break;
        }
        portEXIT_CRITICAL(&work_lock);
        vTaskDelay(1);
    }

    QueueHandle_t queue = solar_os_queue_create(SOLAR_OS_WORK_QUEUE_LEN,
                                                 sizeof(solar_os_work_item_t));
    TaskHandle_t task = NULL;
    BaseType_t created = pdFAIL;
    if (queue != NULL) {
        work_queue = queue;
        created = solar_os_task_create_pinned_internal(
            solar_os_work_task,
            "solar_os_work",
            SOLAR_OS_WORK_STACK_BYTES,
            NULL,
            SOLAR_OS_WORK_DEFAULT_PRIORITY,
            &task,
            tskNO_AFFINITY);
    }

    portENTER_CRITICAL(&work_lock);
    work_initializing = false;
    if (created == pdPASS) {
        work_task = task;
        work_initialized = true;
    }
    portEXIT_CRITICAL(&work_lock);

    if (created != pdPASS) {
        work_queue = NULL;
        solar_os_queue_delete(queue);
        return ESP_ERR_NO_MEM;
    }

    SOLAR_OS_LOGI(TAG,
                  "reserved one %u-byte internal transient stack",
                  (unsigned)SOLAR_OS_WORK_STACK_BYTES);
    return ESP_OK;
}

esp_err_t solar_os_work_submit(const char *name,
                               solar_os_work_fn callback,
                               void *arg,
                               UBaseType_t priority,
                               solar_os_work_handle_t *handle,
                               TaskHandle_t *task)
{
    if (callback == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *handle = (solar_os_work_handle_t)SOLAR_OS_WORK_HANDLE_INIT;
    if (task != NULL) {
        *task = NULL;
    }

    esp_err_t err = solar_os_work_init();
    if (err != ESP_OK) {
        return err;
    }

    solar_os_work_item_t item = {0};
    portENTER_CRITICAL(&work_lock);
    int slot_index = -1;
    for (size_t i = 0; i < SOLAR_OS_WORK_QUEUE_LEN; i++) {
        if (work_slots[i].state == SOLAR_OS_WORK_SLOT_FREE) {
            slot_index = (int)i;
            break;
        }
    }
    if (slot_index >= 0) {
        solar_os_work_slot_t *slot = &work_slots[slot_index];
        slot->generation = work_allocate_generation_locked();
        slot->state = SOLAR_OS_WORK_SLOT_QUEUED;
        slot->callback = callback;
        slot->arg = arg;
        slot->priority = priority;
        strlcpy(slot->name,
                name != NULL && name[0] != '\0' ? name : "work",
                sizeof(slot->name));
        item.generation = slot->generation;
        item.slot = (uint8_t)slot_index;
    } else {
        work_rejected++;
    }
    portEXIT_CRITICAL(&work_lock);

    if (slot_index < 0) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Publish the handle before waking the executor. A short callback may
     * finish before xQueueSend() returns to this task; publishing afterward
     * would resurrect a stale handle and shared-task pointer.
     */
    handle->generation = item.generation;
    handle->slot = item.slot;
    if (task != NULL) {
        *task = work_task;
    }

    if (xQueueSend(work_queue, &item, 0) != pdTRUE) {
        portENTER_CRITICAL(&work_lock);
        solar_os_work_slot_t *slot = &work_slots[item.slot];
        if (slot->state == SOLAR_OS_WORK_SLOT_QUEUED &&
            slot->generation == item.generation) {
            work_clear_slot_locked(slot);
        }
        work_rejected++;
        portEXIT_CRITICAL(&work_lock);
        *handle = (solar_os_work_handle_t)SOLAR_OS_WORK_HANDLE_INIT;
        if (task != NULL) {
            *task = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&work_lock);
    work_submitted++;
    portEXIT_CRITICAL(&work_lock);
    return ESP_OK;
}

esp_err_t solar_os_work_cancel(solar_os_work_handle_t handle)
{
    if (handle.slot >= SOLAR_OS_WORK_QUEUE_LEN || handle.generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&work_lock);
    solar_os_work_slot_t *slot = &work_slots[handle.slot];
    if (slot->generation == handle.generation) {
        if (slot->state == SOLAR_OS_WORK_SLOT_QUEUED) {
            work_clear_slot_locked(slot);
            work_cancelled++;
            result = ESP_OK;
        } else if (slot->state == SOLAR_OS_WORK_SLOT_RUNNING) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    portEXIT_CRITICAL(&work_lock);
    return result;
}

bool solar_os_work_active(solar_os_work_handle_t handle)
{
    if (handle.slot >= SOLAR_OS_WORK_QUEUE_LEN || handle.generation == 0) {
        return false;
    }

    portENTER_CRITICAL(&work_lock);
    const solar_os_work_slot_t *slot = &work_slots[handle.slot];
    const bool active = slot->generation == handle.generation &&
        slot->state != SOLAR_OS_WORK_SLOT_FREE;
    portEXIT_CRITICAL(&work_lock);
    return active;
}

void solar_os_work_get_status(solar_os_work_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));

    TaskHandle_t task = NULL;
    portENTER_CRITICAL(&work_lock);
    status->initialized = work_initialized;
    status->stack_bytes = SOLAR_OS_WORK_STACK_BYTES;
    status->submitted = work_submitted;
    status->completed = work_completed;
    status->cancelled = work_cancelled;
    status->rejected = work_rejected;
    task = work_task;
    for (size_t i = 0; i < SOLAR_OS_WORK_QUEUE_LEN; i++) {
        if (work_slots[i].state == SOLAR_OS_WORK_SLOT_QUEUED) {
            status->queued++;
        } else if (work_slots[i].state == SOLAR_OS_WORK_SLOT_RUNNING) {
            status->running = true;
        }
    }
    if (work_current_slot < SOLAR_OS_WORK_QUEUE_LEN) {
        strlcpy(status->current,
                work_slots[work_current_slot].name,
                sizeof(status->current));
    }
    portEXIT_CRITICAL(&work_lock);

    if (task != NULL) {
        status->stack_high_water = uxTaskGetStackHighWaterMark(task);
    }
}
