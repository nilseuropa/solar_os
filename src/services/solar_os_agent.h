#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_script_runner.h"
#include "solar_os_media_sample.h"

#define SOLAR_OS_AGENT_ENDPOINT_MAX 256
#define SOLAR_OS_AGENT_MODEL_MAX 96
#define SOLAR_OS_AGENT_API_KEY_MAX 192
#define SOLAR_OS_AGENT_REASONING_EFFORT_MAX 12
#define SOLAR_OS_AGENT_PROMPT_MAX 1024
#define SOLAR_OS_AGENT_CONVERSATION_ID_MAX 24
#define SOLAR_OS_AGENT_CONVERSATION_TITLE_MAX 64
#define SOLAR_OS_AGENT_CONVERSATION_LIST_MAX 8
#define SOLAR_OS_AGENT_EVENT_TEXT_MAX 192
#define SOLAR_OS_AGENT_TOOL_NAME_MAX 48
#define SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS 16U
#define SOLAR_OS_AGENT_MAX_TOOLS_MIN 1U
#define SOLAR_OS_AGENT_MAX_TOOLS_MAX 32U
#define SOLAR_OS_AGENT_IMAGES_MAX 2U
#define SOLAR_OS_AGENT_IMAGE_BYTES_MAX (512U * 1024U)
#define SOLAR_OS_AGENT_AUDIO_BYTES_MAX (1024U * 1024U)

typedef enum {
    SOLAR_OS_AGENT_TOOL_POLICY_OFF = 0,
    SOLAR_OS_AGENT_TOOL_POLICY_READONLY,
    SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM,
    SOLAR_OS_AGENT_TOOL_POLICY_ALL,
} solar_os_agent_tool_policy_t;

typedef enum {
    SOLAR_OS_AGENT_SCRIPT_PYTHON = 1U << 0,
    SOLAR_OS_AGENT_SCRIPT_LUA = 1U << 1,
} solar_os_agent_script_language_t;

typedef enum {
    SOLAR_OS_AGENT_EVENT_STATUS = 0,
    SOLAR_OS_AGENT_EVENT_TEXT_DELTA,
    SOLAR_OS_AGENT_EVENT_TOOL_CALL,
    SOLAR_OS_AGENT_EVENT_TOOL_RESULT,
    SOLAR_OS_AGENT_EVENT_TOOL_CONFIRMATION,
    SOLAR_OS_AGENT_EVENT_USAGE,
    SOLAR_OS_AGENT_EVENT_ERROR,
    SOLAR_OS_AGENT_EVENT_DONE,
} solar_os_agent_event_type_t;

typedef struct {
    solar_os_agent_event_type_t type;
    char text[SOLAR_OS_AGENT_EVENT_TEXT_MAX];
    char tool_name[SOLAR_OS_AGENT_TOOL_NAME_MAX];
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    bool success;
} solar_os_agent_event_t;

typedef esp_err_t (*solar_os_agent_event_fn)(const solar_os_agent_event_t *event,
                                             void *user_data);
typedef bool (*solar_os_agent_cancel_fn)(void *user_data);

typedef esp_err_t (*solar_os_agent_tool_confirmation_fn)(
    const char *tool_name,
    const char *risk,
    const char *arguments,
    bool *allowed,
    void *user_data);

typedef esp_err_t (*solar_os_agent_script_run_fn)(
    solar_os_agent_script_language_t language,
    solar_os_script_input_t input_type,
    const char *input,
    int argc,
    const char *const *argv,
    char *output,
    size_t output_size,
    solar_os_script_run_result_t *result,
    void *user_data);

typedef struct {
    const char *prompt;
    /* Borrowed immutable encoded samples, valid until run returns. Total image
     * bytes are bounded independently of text. No decoding or transcoding. */
    solar_os_media_sample_t *const *images;
    size_t image_count;
    /* Optional borrowed encoded WAV/MP3 sample; held until run returns. */
    solar_os_media_sample_t *audio;
    /* SolarOS-local durable conversation ID, not a provider response ID. */
    const char *conversation_id;
    /* Shell directory used to resolve relative storage-tool paths. */
    const char *storage_cwd;
    char *next_conversation_id;
    size_t next_conversation_id_len;
    solar_os_agent_event_fn event_handler;
    /* Polled synchronously during provider I/O; must not call an interpreter. */
    solar_os_agent_cancel_fn should_cancel;
    solar_os_agent_tool_confirmation_fn confirm_tool;
    solar_os_agent_script_run_fn run_script;
    uint32_t script_languages;
    void *user_data;
} solar_os_agent_request_t;

typedef enum {
    SOLAR_OS_AGENT_MESSAGE_USER = 0,
    SOLAR_OS_AGENT_MESSAGE_ASSISTANT,
    SOLAR_OS_AGENT_MESSAGE_TOOL,
} solar_os_agent_message_role_t;

typedef struct {
    char id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
    char title[SOLAR_OS_AGENT_CONVERSATION_TITLE_MAX];
    char provider[24];
    char model[SOLAR_OS_AGENT_MODEL_MAX];
    uint64_t created_at;
    uint64_t updated_at;
    uint16_t turn_count;
    uint32_t stored_bytes;
} solar_os_agent_conversation_info_t;

typedef esp_err_t (*solar_os_agent_message_visit_fn)(
    solar_os_agent_message_role_t role,
    const char *text,
    size_t text_len,
    void *user_data);

typedef struct {
    bool initialized;
    bool configured;
    bool api_key_set;
    bool running;
    char provider[24];
    char endpoint[SOLAR_OS_AGENT_ENDPOINT_MAX];
    char model[SOLAR_OS_AGENT_MODEL_MAX];
    char reasoning_effort[SOLAR_OS_AGENT_REASONING_EFFORT_MAX];
    solar_os_agent_tool_policy_t tool_policy;
    uint8_t max_tools;
    uint8_t last_tool_call_count;
    uint8_t last_max_tools;
    uint32_t request_count;
    uint32_t failure_count;
    uint32_t tool_executed_count;
    uint32_t tool_denied_count;
    uint32_t tool_failed_count;
    int last_http_status;
    esp_err_t last_error;
    uint32_t last_duration_ms;
    uint32_t last_bytes_received;
    uint32_t last_internal_before;
    uint32_t last_internal_low;
    uint32_t last_internal_after;
    uint32_t last_internal_largest_before;
    uint32_t last_internal_largest_after;
    uint32_t last_psram_before;
    uint32_t last_psram_after;
} solar_os_agent_status_t;

esp_err_t solar_os_agent_init(void);
esp_err_t solar_os_agent_set_endpoint(const char *endpoint);
esp_err_t solar_os_agent_set_model(const char *model);
esp_err_t solar_os_agent_set_api_key(const char *api_key);
esp_err_t solar_os_agent_set_reasoning_effort(const char *effort);
esp_err_t solar_os_agent_set_tool_policy(solar_os_agent_tool_policy_t policy);
esp_err_t solar_os_agent_set_max_tools(uint8_t max_tools);
esp_err_t solar_os_agent_parse_tool_policy(
    const char *name,
    solar_os_agent_tool_policy_t *policy);
const char *solar_os_agent_tool_policy_name(
    solar_os_agent_tool_policy_t policy);
esp_err_t solar_os_agent_forget(void);
esp_err_t solar_os_agent_get_status(solar_os_agent_status_t *status);
esp_err_t solar_os_agent_conversations_list(
    solar_os_agent_conversation_info_t *items,
    size_t capacity,
    size_t *count);
esp_err_t solar_os_agent_conversation_get(
    const char *id,
    solar_os_agent_conversation_info_t *info);
esp_err_t solar_os_agent_conversation_visit(
    const char *id,
    solar_os_agent_message_visit_fn visitor,
    void *user_data);
esp_err_t solar_os_agent_conversation_delete(const char *id);

/* Blocking; callers should use a foreground worker task. */
esp_err_t solar_os_agent_run(const solar_os_agent_request_t *request);
esp_err_t solar_os_agent_cancel(void);
