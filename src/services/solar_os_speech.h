#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_native_speech_abi.h"

/* Public producer API. Requests are copied before enqueue returns. */
esp_err_t solar_os_speech_enqueue(const solar_os_speech_request_t *request,
                                  uint32_t *request_id);
/*
 * A stream is one speech request whose text arrives in bounded chunks. Only
 * one stream may be active at a time. The final chunk may be empty.
 */
esp_err_t solar_os_speech_stream_begin(
    const solar_os_speech_stream_options_t *options,
    uint32_t *request_id);
esp_err_t solar_os_speech_stream_write(uint32_t request_id,
                                       const char *text,
                                       size_t text_len,
                                       bool final);
esp_err_t solar_os_speech_cancel(uint32_t request_id);
bool solar_os_speech_request_status(uint32_t request_id,
                                    solar_os_speech_request_status_t *status);
void solar_os_speech_queue_get_status(solar_os_speech_queue_status_t *status);
const char *solar_os_speech_request_state_name(
    solar_os_speech_request_state_t state);

/* Consumer API owned by the speechd job. */
/* Start, signal, and detach one worker. Keep owner live until detach. Stop
 * rejects new requests and wakes a blocked read. Detach after the consumer
 * has finished all operations for this worker. */
esp_err_t solar_os_speech_worker_start(const void *owner);
esp_err_t solar_os_speech_worker_stop(const void *owner);
esp_err_t solar_os_speech_worker_detach(const void *owner);
esp_err_t solar_os_speech_worker_take(solar_os_speech_work_t *work,
                                      uint32_t timeout_ms);
esp_err_t solar_os_speech_worker_stream_take(uint32_t request_id,
                                             char *text,
                                             size_t text_capacity,
                                             size_t *text_len,
                                             bool *final,
                                             uint32_t timeout_ms);
esp_err_t solar_os_speech_worker_set_state(uint32_t request_id,
                                           solar_os_speech_request_state_t state);
esp_err_t solar_os_speech_worker_set_progress(uint32_t request_id,
                                              size_t bytes_done,
                                              size_t bytes_total);
esp_err_t solar_os_speech_worker_finish(uint32_t request_id,
                                        solar_os_speech_request_state_t state,
                                        esp_err_t error);
const volatile bool *solar_os_speech_worker_cancel_flag(uint32_t request_id);
bool solar_os_speech_worker_cancelled(uint32_t request_id);
