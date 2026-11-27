#pragma once

#include "solar_os_agent.h"

#define SOLAR_OS_SCRIPT_AGENT_TEXT_MAX (16U * 1024U)

typedef struct {
    const char *prompt;
    const char *images[SOLAR_OS_AGENT_IMAGES_MAX];
    size_t image_count;
    const char *audio_file; /* Optional SolarOS storage path to one WAV/MP3 file. */
    /* NULL: stateless; "": new saved conversation; otherwise resume this ID. */
    const char *conversation_id;
    solar_os_agent_cancel_fn should_cancel;
    void *user_data;
} solar_os_script_agent_request_t;

typedef struct {
    char *text;
    size_t text_len;
    bool truncated;
    char conversation_id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    uint32_t prompt_tokens, completion_tokens, total_tokens;
} solar_os_script_agent_result_t;

/* Blocking on the caller's script worker. No interpreter or confirmation
 * callbacks are installed: script tools are unavailable and confirmation-only
 * tools are denied by the existing service policy. Always clear the result,
 * including after errors. Image sources and samples never outlive this call. */
esp_err_t solar_os_script_agent_ask(const solar_os_script_agent_request_t *request,
    solar_os_script_agent_result_t *result);
/* Collect text from an already resolved request; callers own the output. */
esp_err_t solar_os_script_agent_capture_answer(const solar_os_agent_request_t *request,
    char *text, size_t capacity, solar_os_native_agent_answer_v1_t *answer);
void solar_os_script_agent_result_clear(solar_os_script_agent_result_t *result);
