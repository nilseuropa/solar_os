#include "solar_os_native_agent.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_AGENT
#include "solar_os_agent.h"
#include "solar_os_agent_conversation.h"
#include "solar_os_agent_reference.h"
#include "solar_os_native_support.h"

_Static_assert(SOLAR_OS_NATIVE_AGENT_POLICY_OFF == SOLAR_OS_AGENT_TOOL_POLICY_OFF &&
               SOLAR_OS_NATIVE_AGENT_POLICY_READ_ONLY == SOLAR_OS_AGENT_TOOL_POLICY_READONLY &&
               SOLAR_OS_NATIVE_AGENT_POLICY_CONFIRM == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM &&
               SOLAR_OS_NATIVE_AGENT_POLICY_ALL == SOLAR_OS_AGENT_TOOL_POLICY_ALL,
               "agent tool policy values");

static int agent_status(solar_os_native_agent_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    solar_os_agent_status_t status;
    const esp_err_t err = solar_os_agent_get_status(&status);
    if (err != ESP_OK) return solar_os_native_result(err);
#define COPY_TEXT(field) \
    _Static_assert(sizeof(out->field) == sizeof(status.field), #field " capacity"); \
    memcpy(out->field, status.field, sizeof(out->field))
    COPY_TEXT(provider);
    COPY_TEXT(endpoint);
    COPY_TEXT(model);
    COPY_TEXT(reasoning_effort);
#undef COPY_TEXT
#define COPY(field) out->field = status.field
    COPY(initialized);
    COPY(configured);
    COPY(api_key_set);
    COPY(running);
    COPY(tool_policy);
    COPY(max_tools);
    COPY(last_tool_call_count);
    COPY(last_max_tools);
    COPY(request_count);
    COPY(failure_count);
    COPY(tool_executed_count);
    COPY(tool_denied_count);
    COPY(tool_failed_count);
    COPY(last_http_status);
    COPY(last_duration_ms);
    COPY(last_bytes_received);
    COPY(last_internal_before);
    COPY(last_internal_low);
    COPY(last_internal_after);
    COPY(last_internal_largest_before);
    COPY(last_internal_largest_after);
    COPY(last_psram_before);
    COPY(last_psram_after);
#undef COPY
    out->last_error = solar_os_native_result(status.last_error);
    return SOLAR_OS_NATIVE_OK;
}

static int agent_set_endpoint(const char *endpoint)
{
    return solar_os_native_result(solar_os_agent_set_endpoint(endpoint));
}

static int agent_set_model(const char *model)
{
    return solar_os_native_result(solar_os_agent_set_model(model));
}

static int agent_set_api_key(const char *api_key)
{
    return solar_os_native_result(solar_os_agent_set_api_key(api_key));
}

static int agent_set_reasoning_effort(const char *effort)
{
    return solar_os_native_result(solar_os_agent_set_reasoning_effort(effort));
}

static int agent_set_tool_policy(uint32_t policy)
{
    if (policy > SOLAR_OS_NATIVE_AGENT_POLICY_ALL) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_agent_set_tool_policy((solar_os_agent_tool_policy_t)policy));
}

static int agent_set_max_tools(uint32_t limit)
{
    if (limit > UINT8_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_agent_set_max_tools(limit));
}

static int agent_parse_tool_policy(const char *name, uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_agent_tool_policy_t policy;
    const esp_err_t err = solar_os_agent_parse_tool_policy(name, &policy);
    if (err == ESP_OK) *out = policy;
    return solar_os_native_result(err);
}

static const char *agent_tool_policy_name(uint32_t policy)
{
    return policy <= SOLAR_OS_NATIVE_AGENT_POLICY_ALL ?
        solar_os_agent_tool_policy_name((solar_os_agent_tool_policy_t)policy) : NULL;
}

static int agent_forget(void)
{
    const esp_err_t err = solar_os_agent_forget();
    return err == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(err);
}

static int agent_reference_search(const char *query, char *out, size_t capacity)
{
    const esp_err_t err = solar_os_agent_reference_search(query, out, capacity);
    if (err != ESP_OK && out && capacity) *out = '\0';
    return solar_os_native_result(err);
}

static const solar_os_native_agent_api_v1_t agent_api = {
    .abi_version = SOLAR_OS_NATIVE_AGENT_ABI,
    .struct_size = sizeof(agent_api),
    .status = agent_status,
    .set_endpoint = agent_set_endpoint,
    .set_model = agent_set_model,
    .set_api_key = agent_set_api_key,
    .set_reasoning_effort = agent_set_reasoning_effort,
    .set_tool_policy = agent_set_tool_policy,
    .set_max_tools = agent_set_max_tools,
    .parse_tool_policy = agent_parse_tool_policy,
    .tool_policy_name = agent_tool_policy_name,
    .forget = agent_forget,
    .reference_search = agent_reference_search,
};
_Static_assert(SOLAR_OS_NATIVE_AGENT_MESSAGE_USER == SOLAR_OS_AGENT_MESSAGE_USER &&
               SOLAR_OS_NATIVE_AGENT_MESSAGE_ASSISTANT == SOLAR_OS_AGENT_MESSAGE_ASSISTANT &&
               SOLAR_OS_NATIVE_AGENT_MESSAGE_TOOL == SOLAR_OS_AGENT_MESSAGE_TOOL,
               "agent message role values");

static int conversation_list(solar_os_native_agent_conversation_info_v1_t *items,
                             size_t capacity, size_t *count)
{
    return solar_os_native_result(solar_os_agent_conversations_list(items, capacity, count));
}

static int conversation_get(const char *id, solar_os_native_agent_conversation_info_v1_t *info)
{
    if (info) memset(info, 0, sizeof(*info));
    return solar_os_native_result(solar_os_agent_conversation_get(id, info));
}

typedef struct {
    int (*visitor)(const solar_os_native_agent_message_v1_t *, void *);
    void *user;
    int result;
} agent_visit_t;

static esp_err_t conversation_visit_message(solar_os_agent_message_role_t role,
                                             const char *text, size_t len, void *user)
{
    agent_visit_t *state = user;
    const solar_os_native_agent_message_v1_t message = {
        .role = role, .text = text, .length = len,
    };
    state->result = state->visitor(&message, state->user);
    return solar_os_native_callback_result(state->result);
}

static int conversation_visit(const char *id,
                              int (*visitor)(const solar_os_native_agent_message_v1_t *, void *),
                              void *user)
{
    if (!visitor) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    agent_visit_t state = {.visitor = visitor, .user = user};
    const esp_err_t err = solar_os_agent_conversation_visit(id, conversation_visit_message, &state);
    return state.result ? state.result : solar_os_native_result(err);
}

static int conversation_remove(const char *id)
{
    return solar_os_native_result(solar_os_agent_conversation_delete(id));
}

static int conversation_commit(const solar_os_native_agent_conversation_turn_v1_t *turn,
                               char *id, size_t capacity)
{
    if (!turn) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_agent_conversation_commit(
        turn->id, turn->provider, turn->model, turn->user_text, turn->assistant_text,
        turn->tool_summary, turn->provider_response_id, id, capacity));
}

static int conversation_with_history(const char *id, bool include_messages,
        int (*visitor)(const solar_os_native_agent_history_v1_t *, void *), void *user)
{
    if (!visitor) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_agent_history_t history;
    const esp_err_t err = solar_os_agent_conversation_load_history(id, include_messages, &history);
    if (err != ESP_OK) return solar_os_native_result(err);

    solar_os_native_agent_message_v1_t messages[SOLAR_OS_AGENT_HISTORY_MESSAGE_MAX];
    for (size_t i = 0; i < history.message_count; i++) {
        messages[i] = (solar_os_native_agent_message_v1_t){
            .role = history.messages[i].role,
            .text = history.messages[i].text,
            .length = history.messages[i].text_len,
        };
    }
    const solar_os_native_agent_history_v1_t view = {
        .provider_response_id = history.provider_response_id,
        .messages = history.message_count ? messages : NULL,
        .count = history.message_count,
    };
    const int result = visitor(&view, user);
    solar_os_agent_conversation_free_history(&history);
    return result;
}

static const solar_os_native_agent_conversation_api_v1_t agent_conversation_api = {
    .abi_version = SOLAR_OS_NATIVE_AGENT_CONVERSATION_ABI,
    .struct_size = sizeof(agent_conversation_api),
    .list = conversation_list,
    .get = conversation_get,
    .visit = conversation_visit,
    .remove = conversation_remove,
    .commit = conversation_commit,
    .with_history = conversation_with_history,
};
#endif

const void *solar_os_native_agent_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_agent_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
