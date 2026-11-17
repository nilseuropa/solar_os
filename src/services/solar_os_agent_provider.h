#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_agent.h"
#include "solar_os_http_client.h"

#define SOLAR_OS_AGENT_TOOL_CALL_ID_MAX 96
#define SOLAR_OS_AGENT_TOOL_ARGUMENTS_MAX 4096
#define SOLAR_OS_AGENT_TOOL_RESULT_MAX 4096
#define SOLAR_OS_AGENT_RESPONSE_ID_MAX 96
#define SOLAR_OS_AGENT_HISTORY_MESSAGE_MAX 24

typedef struct {
    char endpoint[SOLAR_OS_AGENT_ENDPOINT_MAX];
    char model[SOLAR_OS_AGENT_MODEL_MAX];
    char api_key[SOLAR_OS_AGENT_API_KEY_MAX];
    char reasoning_effort[SOLAR_OS_AGENT_REASONING_EFFORT_MAX];
} solar_os_agent_provider_config_t;

typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;
    bool strict;
} solar_os_agent_tool_descriptor_t;

typedef enum {
    SOLAR_OS_AGENT_PROVIDER_RESUME_LOCAL_HISTORY = 0,
    SOLAR_OS_AGENT_PROVIDER_RESUME_REMOTE_ID,
} solar_os_agent_provider_resume_mode_t;

typedef struct {
    solar_os_agent_message_role_t role;
    const char *text;
    size_t text_len;
} solar_os_agent_history_message_t;

typedef struct {
    const char *prompt;
    solar_os_media_sample_t *const *images;
    size_t image_count;
    solar_os_media_sample_t *audio;
    const solar_os_agent_history_message_t *history;
    size_t history_count;
    const solar_os_agent_tool_descriptor_t *tools;
    size_t tool_count;
    bool continuation;
    const char *previous_response_id;
    const char *tool_call_id;
    const char *tool_name;
    const char *tool_arguments;
    const char *tool_result;
    const char *tool_context;
} solar_os_agent_provider_turn_t;

typedef struct {
    bool tool_call;
    char tool_call_id[SOLAR_OS_AGENT_TOOL_CALL_ID_MAX];
    char tool_name[SOLAR_OS_AGENT_TOOL_NAME_MAX];
    char tool_arguments[SOLAR_OS_AGENT_TOOL_ARGUMENTS_MAX];
    char response_id[SOLAR_OS_AGENT_RESPONSE_ID_MAX];
    int http_status;
    uint32_t duration_ms;
    uint32_t bytes_received;
} solar_os_agent_provider_result_t;

typedef struct {
    const char *name;
    uint32_t capabilities;
    solar_os_agent_provider_resume_mode_t (*resume_mode)(
        const solar_os_agent_provider_config_t *config);
    esp_err_t (*run_turn)(const solar_os_agent_provider_config_t *config,
                          const solar_os_agent_provider_turn_t *turn,
                          solar_os_agent_event_fn event_handler,
                          void *user_data,
                          solar_os_agent_provider_result_t *result);
} solar_os_agent_provider_t;

#define SOLAR_OS_AGENT_PROVIDER_CAP_STREAMING (1U << 0)
#define SOLAR_OS_AGENT_PROVIDER_CAP_TOOLS (1U << 1)
#define SOLAR_OS_AGENT_PROVIDER_CAP_USAGE (1U << 2)
#define SOLAR_OS_AGENT_PROVIDER_CAP_IMAGES (1U << 3)
#define SOLAR_OS_AGENT_PROVIDER_CAP_AUDIO (1U << 4)

void solar_os_agent_provider_bind_request(solar_os_http_request_t *request);
void solar_os_agent_provider_unbind_request(solar_os_http_request_t *request);
bool solar_os_agent_provider_cancel_requested(void);
void solar_os_agent_provider_note_memory(void);

extern const solar_os_agent_provider_t solar_os_agent_openai_provider;
