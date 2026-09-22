/*
 * Derived from esp_picotts.c in DiUS esp-picotts.
 * Copyright (C) 2024 DiUS Computing Pty Ltd.
 * Licensed under the Apache License, Version 2.0.
 */

#include "picotts.h"

#include <math.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "picoapi.h"
#include "picoapid.h"
#include "picotts_resource.h"

#define PICO_MEM_SIZE 1100000U
#define PICOTASK_EXIT 0x0000001U
#define INPUT_QUEUE_WAIT_MS 20U
#define EXIT_WAIT_MS 1000U
#define INPUT_COOPERATIVE_BYTES 64U
#define INPUT_PROGRESS_SLICE_BYTES 16U
#define OUTPUT_COOPERATIVE_STEPS 8U

static picotts_output_fn output_callback;
static picotts_error_notify_fn error_callback;
static picotts_idle_notify_fn idle_callback;
static picotts_progress_notify_fn progress_callback;
static SemaphoreHandle_t exit_lock;
static QueueHandle_t text_queue;
static TaskHandle_t pico_task;
static void *pico_memory;
static pico_System pico_system;
static pico_Resource pico_ta_resource;
static pico_Resource pico_sg_resource;
static pico_Engine pico_engine;
static const pico_Char voice_name[] = "PicoVoice";
static const char *TAG = "picotts";
static portMUX_TYPE progress_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned progress_done;
static unsigned progress_total;

static void pico_progress_reset(const char *text, unsigned length)
{
    const unsigned total = length > 0U && text[length - 1U] == '\0' ?
        length - 1U : length;
    portENTER_CRITICAL(&progress_lock);
    progress_done = 0U;
    progress_total = total;
    portEXIT_CRITICAL(&progress_lock);
}

static void pico_progress_accepted(uint8_t byte)
{
    if (byte == '\0') {
        return;
    }
    portENTER_CRITICAL(&progress_lock);
    if (progress_done < progress_total) {
        progress_done++;
    }
    portEXIT_CRITICAL(&progress_lock);
}

static void pico_progress_report(void)
{
    unsigned done = 0U;
    unsigned total = 0U;
    picotts_progress_notify_fn callback = NULL;
    portENTER_CRITICAL(&progress_lock);
    done = progress_done;
    total = progress_total;
    callback = progress_callback;
    portEXIT_CRITICAL(&progress_lock);
    if (callback != NULL && total > 0U) {
        callback(done, total);
    }
}

static bool pico_exit_requested(void)
{
    uint32_t flags = 0U;
    return xTaskNotifyWait(0, UINT32_MAX, &flags, 0) == pdPASS &&
           (flags & PICOTASK_EXIT) != 0U;
}

picoos_double picoos_quick_exp(const picoos_double value)
{
    return exp(value);
}

static void log_pico_error(const char *operation, int code)
{
    pico_Retstring message;
    pico_getSystemStatusMessage(pico_system, code, message);
    ESP_LOGE(TAG, "%s (%d): %s", operation, code, message);
}

static void pico_task_main(void *arg)
{
    (void)arg;
    bool failed = false;
    enum {
        WAITING_FOR_BYTES,
        WAITING_FOR_OUTPUT,
    } state = WAITING_FOR_BYTES;
    bool utterance_end_seen = false;
    unsigned input_steps = 0U;
    unsigned output_steps = 0U;

    while (!failed) {
        if (pico_exit_requested()) {
            break;
        }

        uint8_t byte = 0U;
        unsigned input_slice = 0U;
        while (xQueuePeek(text_queue, &byte, 0) == pdPASS) {
            if (pico_exit_requested()) {
                goto stopped;
            }
            int16_t processed = 0;
            const int result = pico_putTextUtf8(
                pico_engine, &byte, 1, &processed);
            if (result != 0) {
                log_pico_error("put text failed", result);
                failed = true;
                break;
            }
            if (processed != 0) {
                (void)xQueueReceive(text_queue, &byte, 0);
                pico_progress_accepted(byte);
                input_steps++;
                input_slice++;
                if (byte == '\0') {
                    utterance_end_seen = true;
                }
                if (state == WAITING_FOR_BYTES) {
                    state = WAITING_FOR_OUTPUT;
                }
                if (input_steps >= INPUT_COOPERATIVE_BYTES) {
                    input_steps = 0U;
                    vTaskDelay(1);
                }
                if (input_slice >= INPUT_PROGRESS_SLICE_BYTES) {
                    state = WAITING_FOR_OUTPUT;
                    break;
                }
            } else {
                /* Pico's input buffer is full. Generate output to make room
                 * before trying the same queued byte again. */
                state = WAITING_FOR_OUTPUT;
                break;
            }
        }

        if (state == WAITING_FOR_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        int status = PICO_STEP_IDLE;
        bool produced_output = false;
        do {
            if (pico_exit_requested()) {
                goto stopped;
            }
            int16_t output[128];
            int16_t bytes = 0;
            int16_t type = 0;
            status = pico_getData(
                pico_engine, output, sizeof(output), &bytes, &type);
            if (bytes > 0 && output_callback != NULL) {
                produced_output = true;
                output_callback(output, (unsigned)bytes / 2U);
            }
            output_steps++;
            if (output_steps >= OUTPUT_COOPERATIVE_STEPS) {
                output_steps = 0U;
                vTaskDelay(1);
            }
        } while (status == PICO_STEP_BUSY);

        if (status != PICO_STEP_IDLE) {
            log_pico_error("get data failed", status);
            failed = true;
        } else {
            state = WAITING_FOR_BYTES;
            if (produced_output || utterance_end_seen) {
                pico_progress_report();
            }
            if (utterance_end_seen) {
                utterance_end_seen = false;
                if (idle_callback != NULL) {
                    idle_callback();
                }
            }
        }
    }

stopped:
    if (failed && error_callback != NULL) {
        error_callback();
    }
    ESP_LOGI(TAG, "task stopped");
    (void)xSemaphoreGive(exit_lock);
    vTaskDelete(NULL);
}

static bool pico_cleanup(void)
{
    if (pico_task != NULL) {
        if (xSemaphoreTake(exit_lock, 0) != pdTRUE) {
            (void)xTaskNotify(pico_task, PICOTASK_EXIT, eSetBits);
            if (xSemaphoreTake(
                    exit_lock, pdMS_TO_TICKS(EXIT_WAIT_MS)) != pdTRUE) {
                ESP_LOGE(TAG, "task did not stop within %u ms", EXIT_WAIT_MS);
                return false;
            }
        }
        pico_task = NULL;
    }
    if (pico_engine != NULL) {
        (void)pico_disposeEngine(pico_system, &pico_engine);
        (void)pico_releaseVoiceDefinition(pico_system, voice_name);
        pico_engine = NULL;
    }
    if (pico_sg_resource != NULL) {
        (void)solar_os_pico_unload_resource(pico_system, &pico_sg_resource);
        pico_sg_resource = NULL;
    }
    if (pico_ta_resource != NULL) {
        (void)solar_os_pico_unload_resource(pico_system, &pico_ta_resource);
        pico_ta_resource = NULL;
    }
    if (pico_system != NULL) {
        (void)pico_terminate(&pico_system);
        pico_system = NULL;
    }
    free(pico_memory);
    pico_memory = NULL;
    if (text_queue != NULL) {
        vQueueDelete(text_queue);
        text_queue = NULL;
    }
    return true;
}

bool picotts_init_resources(unsigned priority,
                            picotts_output_fn output,
                            int core,
                            const void *ta_data,
                            size_t ta_size,
                            const void *sg_data,
                            size_t sg_size)
{
    if (output == NULL || ta_data == NULL || ta_size == 0U ||
        sg_data == NULL || sg_size == 0U || pico_memory != NULL) {
        return false;
    }
    if (exit_lock == NULL) {
        exit_lock = xSemaphoreCreateBinary();
    }
    if (exit_lock == NULL) {
        return false;
    }
    while (xSemaphoreTake(exit_lock, 0) == pdTRUE) {
    }

    output_callback = output;
    pico_memory = malloc(PICO_MEM_SIZE);
    if (pico_memory == NULL) {
        ESP_LOGE(TAG, "insufficient memory for PicoTTS");
        return false;
    }

#define PICO_INIT_CHECK(message)          \
    do {                                  \
        if (result != 0) {                \
            log_pico_error(message, result); \
            pico_cleanup();               \
            return false;                 \
        }                                 \
    } while (0)

    int result = pico_initialize(pico_memory, PICO_MEM_SIZE, &pico_system);
    PICO_INIT_CHECK("initialization failed");

    result = solar_os_pico_load_resource(
        pico_system, ta_data, ta_size, &pico_ta_resource);
    PICO_INIT_CHECK("text analysis resource load failed");
    result = solar_os_pico_load_resource(
        pico_system, sg_data, sg_size, &pico_sg_resource);
    PICO_INIT_CHECK("signal generator resource load failed");

    result = pico_createVoiceDefinition(pico_system, voice_name);
    PICO_INIT_CHECK("voice creation failed");

    pico_Retstring resource_name;
    result = pico_getResourceName(
        pico_system, pico_ta_resource, resource_name);
    PICO_INIT_CHECK("TA resource name failed");
    result = pico_addResourceToVoiceDefinition(
        pico_system, voice_name, (const pico_Char *)resource_name);
    PICO_INIT_CHECK("TA resource add failed");

    result = pico_getResourceName(
        pico_system, pico_sg_resource, resource_name);
    PICO_INIT_CHECK("SG resource name failed");
    result = pico_addResourceToVoiceDefinition(
        pico_system, voice_name, (const pico_Char *)resource_name);
    PICO_INIT_CHECK("SG resource add failed");

    result = pico_newEngine(pico_system, voice_name, &pico_engine);
    PICO_INIT_CHECK("engine creation failed");
#undef PICO_INIT_CHECK

    text_queue = xQueueCreate(PICOTTS_INPUT_QUEUE_SIZE, sizeof(char));
    if (text_queue == NULL) {
        ESP_LOGE(TAG, "failed to create input queue");
        pico_cleanup();
        return false;
    }
    if (xTaskCreatePinnedToCore(pico_task_main,
                                "picotts",
                                8192,
                                NULL,
                                priority,
                                &pico_task,
                                core == -1 ? tskNO_AFFINITY : core) != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        pico_cleanup();
        return false;
    }
    return true;
}

bool picotts_add(const char *text,
                 unsigned length,
                 const volatile bool *cancelled)
{
    if (text == NULL || text_queue == NULL) {
        return false;
    }
    pico_progress_reset(text, length);
    while (length-- > 0U) {
        if (cancelled != NULL && *cancelled) {
            return false;
        }
        while (xQueueSendToBack(text_queue,
                                text,
                                pdMS_TO_TICKS(INPUT_QUEUE_WAIT_MS)) != pdPASS) {
            if ((cancelled != NULL && *cancelled) || text_queue == NULL) {
                return false;
            }
        }
        ++text;
    }
    return true;
}

bool picotts_shutdown(void)
{
    const bool stopped = pico_cleanup();
    output_callback = NULL;
    error_callback = NULL;
    idle_callback = NULL;
    progress_callback = NULL;
    return stopped;
}

void picotts_set_error_notify(picotts_error_notify_fn callback)
{
    error_callback = callback;
}

void picotts_set_idle_notify(picotts_idle_notify_fn callback)
{
    idle_callback = callback;
}

void picotts_set_progress_notify(picotts_progress_notify_fn callback)
{
    portENTER_CRITICAL(&progress_lock);
    progress_callback = callback;
    portEXIT_CRITICAL(&progress_lock);
}
