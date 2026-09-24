/*
 * Derived from esp_picotts.c in DiUS esp-picotts.
 * Copyright (C) 2024 DiUS Computing Pty Ltd.
 * Licensed under the Apache License, Version 2.0.
 */

#include "picotts.h"

#include <math.h>
#include <stdio.h>
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
#define PICOTASK_ABORT 0x0000002U
#define INPUT_QUEUE_WAIT_MS 20U
#define EXIT_WAIT_MS 1000U
#define ABORT_WAIT_MS 1000U
#define INPUT_COOPERATIVE_BYTES 64U
#define OUTPUT_COOPERATIVE_INTERVAL_MS 8U
#define QUEUE_BYTE_MASK 0x00ffU
#define QUEUE_COUNTS_PROGRESS 0x0100U
#define QUEUE_SEGMENT_END 0x0200U
#define QUEUE_UTTERANCE_END 0x0400U
#define PICOTTS_PITCH_MIN 50U
#define PICOTTS_PITCH_MAX 200U
#define PICOTTS_SPEED_MIN 20U
#define PICOTTS_SPEED_MAX 500U

static picotts_output_fn output_callback;
static picotts_error_notify_fn error_callback;
static picotts_idle_notify_fn idle_callback;
static picotts_progress_notify_fn progress_callback;
static SemaphoreHandle_t exit_lock;
static SemaphoreHandle_t abort_done;
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
static volatile bool abort_succeeded;

static void pico_progress_reset(unsigned total)
{
    portENTER_CRITICAL(&progress_lock);
    progress_done = 0U;
    progress_total = total;
    portEXIT_CRITICAL(&progress_lock);
}

static void pico_progress_accepted(bool counts_progress)
{
    if (!counts_progress) {
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

static uint32_t pico_control_flags(TickType_t wait)
{
    uint32_t flags = 0U;
    (void)xTaskNotifyWait(0, UINT32_MAX, &flags, wait);
    return flags;
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
    bool segment_end_seen = false;
    bool utterance_end_seen = false;
    unsigned input_steps = 0U;
    TickType_t last_output_yield = xTaskGetTickCount();

    while (!failed) {
        uint32_t control = pico_control_flags(0);
        if ((control & PICOTASK_EXIT) != 0U) {
            goto stopped;
        }
        if ((control & PICOTASK_ABORT) != 0U) {
            goto aborted;
        }

        uint16_t item = 0U;
        while (xQueuePeek(text_queue, &item, 0) == pdPASS) {
            control = pico_control_flags(0);
            if ((control & PICOTASK_EXIT) != 0U) {
                goto stopped;
            }
            if ((control & PICOTASK_ABORT) != 0U) {
                goto aborted;
            }
            uint8_t byte = (uint8_t)(item & QUEUE_BYTE_MASK);
            int16_t processed = 0;
            const int result = pico_putTextUtf8(
                pico_engine, &byte, 1, &processed);
            if (result != 0) {
                log_pico_error("put text failed", result);
                failed = true;
                break;
            }
            if (processed != 0) {
                (void)xQueueReceive(text_queue, &item, 0);
                pico_progress_accepted(
                    (item & QUEUE_COUNTS_PROGRESS) != 0U);
                input_steps++;
                if ((item & QUEUE_SEGMENT_END) != 0U) {
                    segment_end_seen = true;
                    utterance_end_seen =
                        (item & QUEUE_UTTERANCE_END) != 0U;
                }
                if (state == WAITING_FOR_BYTES) {
                    state = WAITING_FOR_OUTPUT;
                }
                if (input_steps >= INPUT_COOPERATIVE_BYTES) {
                    input_steps = 0U;
                    vTaskDelay(1);
                }
                if (segment_end_seen) {
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
            control = pico_control_flags(pdMS_TO_TICKS(100U));
            if ((control & PICOTASK_EXIT) != 0U) {
                goto stopped;
            }
            if ((control & PICOTASK_ABORT) != 0U) {
                goto aborted;
            }
            continue;
        }

        int status = PICO_STEP_IDLE;
        do {
            control = pico_control_flags(0);
            if ((control & PICOTASK_EXIT) != 0U) {
                goto stopped;
            }
            if ((control & PICOTASK_ABORT) != 0U) {
                goto aborted;
            }
            int16_t output[128];
            int16_t bytes = 0;
            int16_t type = 0;
            status = pico_getData(
                pico_engine, output, sizeof(output), &bytes, &type);
            if (bytes > 0 && output_callback != NULL) {
                output_callback(output, (unsigned)bytes / 2U);
            }
            const TickType_t now = xTaskGetTickCount();
            if (now - last_output_yield >=
                pdMS_TO_TICKS(OUTPUT_COOPERATIVE_INTERVAL_MS)) {
                vTaskDelay(1);
                last_output_yield = xTaskGetTickCount();
            }
        } while (status == PICO_STEP_BUSY);

        if (status != PICO_STEP_IDLE) {
            log_pico_error("get data failed", status);
            failed = true;
        } else {
            state = WAITING_FOR_BYTES;
            if (segment_end_seen) {
                pico_progress_report();
                segment_end_seen = false;
            }
            if (utterance_end_seen) {
                utterance_end_seen = false;
                if (idle_callback != NULL) {
                    idle_callback();
                }
            }
        }
        continue;

aborted:
        {
            if (text_queue != NULL) {
                (void)xQueueReset(text_queue);
            }
            const int reset_result =
                pico_resetEngine(pico_engine, PICO_RESET_SOFT);
            abort_succeeded = reset_result == 0;
            if (!abort_succeeded) {
                log_pico_error("engine abort failed", reset_result);
                failed = true;
            }
            pico_progress_reset(0U);
            state = WAITING_FOR_BYTES;
            segment_end_seen = false;
            utterance_end_seen = false;
            input_steps = 0U;
            last_output_yield = xTaskGetTickCount();
            if (abort_done != NULL) {
                (void)xSemaphoreGive(abort_done);
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
    if (abort_done == NULL) {
        abort_done = xSemaphoreCreateBinary();
    }
    if (exit_lock == NULL || abort_done == NULL) {
        return false;
    }
    while (xSemaphoreTake(exit_lock, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(abort_done, 0) == pdTRUE) {
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

    text_queue = xQueueCreate(PICOTTS_INPUT_QUEUE_SIZE, sizeof(uint16_t));
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

static bool pico_queue_item(uint16_t item,
                            const volatile bool *cancelled)
{
    if (text_queue == NULL) {
        return false;
    }
    for (;;) {
        if (cancelled != NULL && *cancelled) {
            return false;
        }
        while (xQueueSendToBack(text_queue,
                                &item,
                                pdMS_TO_TICKS(INPUT_QUEUE_WAIT_MS)) != pdPASS) {
            if ((cancelled != NULL && *cancelled) || text_queue == NULL) {
                return false;
            }
        }
        return true;
    }
}

static bool pico_queue_bytes(const char *text,
                             unsigned length,
                             bool counts_progress,
                             const volatile bool *cancelled)
{
    const uint16_t flags = counts_progress ? QUEUE_COUNTS_PROGRESS : 0U;
    for (unsigned i = 0U; i < length; i++) {
        if (!pico_queue_item(flags | (uint8_t)text[i], cancelled)) {
            return false;
        }
    }
    return true;
}

static bool pico_stream_begin(unsigned pitch,
                              unsigned speed,
                              const volatile bool *cancelled)
{
    if (text_queue == NULL ||
        pitch < PICOTTS_PITCH_MIN || pitch > PICOTTS_PITCH_MAX ||
        speed < PICOTTS_SPEED_MIN || speed > PICOTTS_SPEED_MAX) {
        return false;
    }

    char prefix[64];
    const int prefix_length = snprintf(prefix,
                                       sizeof(prefix),
                                       "<pitch level=\"%u\"><speed level=\"%u\">",
                                       pitch,
                                       speed);
    if (prefix_length <= 0 || (size_t)prefix_length >= sizeof(prefix)) {
        return false;
    }
    return pico_queue_bytes(prefix,
                            (unsigned)prefix_length,
                            false,
                            cancelled);
}

static bool pico_stream_write(const char *text,
                              unsigned length,
                              bool final,
                              bool counts_progress,
                              const volatile bool *cancelled)
{
    if (text_queue == NULL || (length > 0U && text == NULL)) {
        return false;
    }
    const unsigned text_length = length > 0U && text[length - 1U] == '\0' ?
        length - 1U : length;
    if (text_length == 0U && !final) {
        return false;
    }
    if (counts_progress) {
        pico_progress_reset(text_length);
    }
    if (text_length > 0U &&
        !pico_queue_bytes(text, text_length, counts_progress, cancelled)) {
        return false;
    }
    if (!final) {
        return true;
    }
    static const char suffix[] = "</speed></pitch>";
    return pico_queue_bytes(suffix,
                            sizeof(suffix) - 1U,
                            false,
                            cancelled) &&
        pico_queue_item(QUEUE_SEGMENT_END | QUEUE_UTTERANCE_END,
                        cancelled);
}

bool picotts_add(const char *text,
                 unsigned length,
                 unsigned pitch,
                 unsigned speed,
                 const volatile bool *cancelled)
{
    if (text == NULL || length == 0U ||
        !pico_stream_begin(pitch, speed, cancelled)) {
        return false;
    }
    return pico_stream_write(text, length, true, true, cancelled);
}

bool picotts_stream_begin(unsigned pitch,
                          unsigned speed,
                          const volatile bool *cancelled)
{
    pico_progress_reset(0U);
    return pico_stream_begin(pitch, speed, cancelled);
}

bool picotts_stream_write(const char *text,
                          unsigned length,
                          bool final,
                          const volatile bool *cancelled)
{
    return pico_stream_write(text, length, final, false, cancelled);
}

bool picotts_stream_end(const volatile bool *cancelled)
{
    return pico_stream_write(NULL, 0U, true, false, cancelled);
}

bool picotts_abort(void)
{
    if (pico_task == NULL || text_queue == NULL || abort_done == NULL) {
        return false;
    }
    while (xSemaphoreTake(abort_done, 0) == pdTRUE) {
    }
    abort_succeeded = false;
    if (xTaskNotify(pico_task, PICOTASK_ABORT, eSetBits) != pdPASS ||
        xSemaphoreTake(abort_done, pdMS_TO_TICKS(ABORT_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "engine abort timed out after %u ms", ABORT_WAIT_MS);
        return false;
    }
    return abort_succeeded;
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
