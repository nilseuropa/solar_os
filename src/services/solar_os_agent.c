#include "solar_os_agent.h"
#include "solar_os_agent_images.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_agent_conversation.h"
#include "solar_os_agent_provider.h"
#include "solar_os_agent_tools.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_sync.h"

#define AGENT_NVS_NAMESPACE "agent"
#define AGENT_NVS_ENDPOINT_KEY "endpoint"
#define AGENT_NVS_MODEL_KEY "model"
#define AGENT_NVS_API_KEY_KEY "api_key"
#define AGENT_NVS_REASONING_KEY "reasoning"
#define AGENT_NVS_TOOL_POLICY_KEY "tool_policy"
#define AGENT_NVS_MAX_TOOLS_KEY "max_tools"
#define AGENT_DEFAULT_REASONING_EFFORT "medium"
#define AGENT_DEFAULT_TOOL_POLICY SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM
#define AGENT_ASSISTANT_CAPTURE_MAX (16U * 1024U)
#define AGENT_TOOL_SUMMARY_MAX 768U
#define AGENT_TOOL_CONTEXT_MAX 4096U
#define AGENT_TOOL_CONTEXT_RESULT_MAX 1536U
#define AGENT_TOOL_SIGNATURE_MAX 8U
#define AGENT_DUPLICATE_TOOL_LIMIT 1U
#define AGENT_EMPTY_RESPONSE_RETRY_MAX 1U

typedef struct {
    solar_os_http_request_t *request;
    uint32_t refs;
    bool closing;
} agent_request_handle_t;

typedef struct {
    const solar_os_agent_request_t *request;
    char *assistant;
    size_t assistant_len;
    bool assistant_truncated;
} agent_event_capture_t;

typedef struct {
    solar_os_agent_provider_result_t provider_result;
    char tool_call_id[SOLAR_OS_AGENT_TOOL_CALL_ID_MAX];
    char tool_name[SOLAR_OS_AGENT_TOOL_NAME_MAX];
    char tool_arguments[SOLAR_OS_AGENT_TOOL_ARGUMENTS_MAX];
    char previous_response_id[SOLAR_OS_AGENT_RESPONSE_ID_MAX];
    char tool_summary[AGENT_TOOL_SUMMARY_MAX];
    size_t tool_summary_len;
    uint32_t completed_tool_signatures[AGENT_TOOL_SIGNATURE_MAX];
    uint8_t completed_tool_signature_count;
    uint8_t duplicate_tool_calls;
    agent_event_capture_t capture;
} agent_run_turn_state_t;

typedef struct {
    bool initialized;
    bool running;
    bool cancel_requested;
    solar_os_agent_cancel_fn should_cancel;
    void *cancel_user_data;
    solar_os_agent_provider_config_t config;
    solar_os_agent_tool_policy_t tool_policy;
    uint8_t max_tools;
    uint8_t last_tool_call_count;
    uint8_t last_max_tools;
    agent_request_handle_t *active_request;
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
} agent_state_t;

static const char *TAG = "agent";
static portMUX_TYPE agent_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR agent_state_t agent;
static solar_os_lifecycle_t agent_settings;

static uint32_t agent_internal_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t agent_internal_largest(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT);
}

static uint32_t agent_psram_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool agent_string_valid(const char *text, size_t max_len, bool allow_empty)
{
    if (text == NULL) {
        return false;
    }
    const size_t len = strlen(text);
    if ((!allow_empty && len == 0) || len >= max_len) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool agent_endpoint_valid(const char *endpoint)
{
    return agent_string_valid(endpoint, SOLAR_OS_AGENT_ENDPOINT_MAX, false) &&
        (strncmp(endpoint, "http://", 7) == 0 ||
         strncmp(endpoint, "https://", 8) == 0);
}

static bool agent_reasoning_effort_valid(const char *effort)
{
    static const char * const values[] = {
        "none",
        "minimal",
        "low",
        "medium",
        "high",
        "xhigh",
        "max",
    };
    if (!agent_string_valid(effort,
                            SOLAR_OS_AGENT_REASONING_EFFORT_MAX,
                            false)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (strcmp(effort, values[i]) == 0) {
            return true;
        }
    }
    return false;
}

const char *solar_os_agent_tool_policy_name(
    solar_os_agent_tool_policy_t policy)
{
    switch (policy) {
    case SOLAR_OS_AGENT_TOOL_POLICY_OFF:
        return "off";
    case SOLAR_OS_AGENT_TOOL_POLICY_READONLY:
        return "readonly";
    case SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM:
        return "confirm";
    case SOLAR_OS_AGENT_TOOL_POLICY_ALL:
        return "all";
    default:
        return "unknown";
    }
}

esp_err_t solar_os_agent_parse_tool_policy(
    const char *name,
    solar_os_agent_tool_policy_t *policy)
{
    if (name == NULL || policy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const struct {
        const char *name;
        solar_os_agent_tool_policy_t policy;
    } values[] = {
        {"off", SOLAR_OS_AGENT_TOOL_POLICY_OFF},
        {"readonly", SOLAR_OS_AGENT_TOOL_POLICY_READONLY},
        {"confirm", SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM},
        {"all", SOLAR_OS_AGENT_TOOL_POLICY_ALL},
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (strcmp(name, values[i].name) == 0) {
            *policy = values[i].policy;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

static void agent_load_config(solar_os_agent_provider_config_t *config,
                               solar_os_agent_tool_policy_t *policy,
                               uint8_t *max_tools)
{
    nvs_handle_t nvs;
    if (nvs_open(AGENT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(config->endpoint);
    if (nvs_get_str(nvs,
                    AGENT_NVS_ENDPOINT_KEY,
                    config->endpoint,
                    &len) != ESP_OK ||
        !agent_endpoint_valid(config->endpoint)) {
        config->endpoint[0] = '\0';
    }
    len = sizeof(config->model);
    if (nvs_get_str(nvs,
                    AGENT_NVS_MODEL_KEY,
                    config->model,
                    &len) != ESP_OK ||
        !agent_string_valid(config->model,
                            sizeof(config->model),
                            false)) {
        config->model[0] = '\0';
    }
    len = sizeof(config->api_key);
    if (nvs_get_str(nvs,
                    AGENT_NVS_API_KEY_KEY,
                    config->api_key,
                    &len) != ESP_OK ||
        !agent_string_valid(config->api_key,
                            sizeof(config->api_key),
                            true)) {
        config->api_key[0] = '\0';
    }
    len = sizeof(config->reasoning_effort);
    if (nvs_get_str(nvs,
                    AGENT_NVS_REASONING_KEY,
                    config->reasoning_effort,
                    &len) != ESP_OK ||
        !agent_reasoning_effort_valid(config->reasoning_effort)) {
        strlcpy(config->reasoning_effort,
                AGENT_DEFAULT_REASONING_EFFORT,
                sizeof(config->reasoning_effort));
    }
    char tool_policy[16];
    len = sizeof(tool_policy);
    if (nvs_get_str(nvs,
                    AGENT_NVS_TOOL_POLICY_KEY,
                    tool_policy,
                    &len) != ESP_OK ||
        solar_os_agent_parse_tool_policy(tool_policy,
                                         policy) != ESP_OK) {
        *policy = AGENT_DEFAULT_TOOL_POLICY;
    }
    if (nvs_get_u8(nvs, AGENT_NVS_MAX_TOOLS_KEY, max_tools) != ESP_OK ||
        *max_tools < SOLAR_OS_AGENT_MAX_TOOLS_MIN ||
        *max_tools > SOLAR_OS_AGENT_MAX_TOOLS_MAX) {
        *max_tools = SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS;
    }
    nvs_close(nvs);
}

static esp_err_t agent_settings_enter(void)
{
    if (!solar_os_lifecycle_enter(&agent_settings)) return ESP_ERR_NO_MEM;
    if (agent.initialized) return ESP_OK;

    solar_os_agent_provider_config_t config = {0};
    solar_os_agent_tool_policy_t policy = AGENT_DEFAULT_TOOL_POLICY;
    uint8_t max_tools = SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS;
    strlcpy(config.reasoning_effort, AGENT_DEFAULT_REASONING_EFFORT,
            sizeof(config.reasoning_effort));
    agent_load_config(&config, &policy, &max_tools);

    portENTER_CRITICAL(&agent_lock);
    agent.config = config;
    agent.tool_policy = policy;
    agent.max_tools = max_tools;
    agent.last_http_status = -1;
    agent.initialized = true;
    portEXIT_CRITICAL(&agent_lock);
    explicit_bzero(config.api_key, sizeof(config.api_key));
    return ESP_OK;
}

esp_err_t solar_os_agent_init(void)
{
    portENTER_CRITICAL(&agent_lock);
    const bool initialized = agent.initialized;
    portEXIT_CRITICAL(&agent_lock);
    if (initialized) return ESP_OK;

    const esp_err_t err = agent_settings_enter();
    if (err == ESP_OK) solar_os_lifecycle_leave(&agent_settings);
    return err;
}

static esp_err_t agent_save_string(const char *key,
                                   const char *value,
                                   char *cached,
                                   size_t cached_len)
{
    esp_err_t err = agent_settings_enter();
    if (err != ESP_OK) return err;

    nvs_handle_t nvs;
    err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, key, value);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&agent_lock);
        strlcpy(cached, value, cached_len);
        portEXIT_CRITICAL(&agent_lock);
    }
    solar_os_lifecycle_leave(&agent_settings);
    return err;
}

esp_err_t solar_os_agent_set_endpoint(const char *endpoint)
{
    if (!agent_endpoint_valid(endpoint)) {
        return ESP_ERR_INVALID_ARG;
    }
    return agent_save_string(AGENT_NVS_ENDPOINT_KEY,
                             endpoint,
                             agent.config.endpoint,
                             sizeof(agent.config.endpoint));
}

esp_err_t solar_os_agent_set_model(const char *model)
{
    if (!agent_string_valid(model, SOLAR_OS_AGENT_MODEL_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    return agent_save_string(AGENT_NVS_MODEL_KEY,
                             model,
                             agent.config.model,
                             sizeof(agent.config.model));
}

esp_err_t solar_os_agent_set_api_key(const char *api_key)
{
    if (!agent_string_valid(api_key, SOLAR_OS_AGENT_API_KEY_MAX, true)) {
        return ESP_ERR_INVALID_ARG;
    }
    return agent_save_string(AGENT_NVS_API_KEY_KEY,
                             api_key,
                             agent.config.api_key,
                             sizeof(agent.config.api_key));
}

esp_err_t solar_os_agent_set_reasoning_effort(const char *effort)
{
    if (!agent_reasoning_effort_valid(effort)) {
        return ESP_ERR_INVALID_ARG;
    }
    return agent_save_string(AGENT_NVS_REASONING_KEY,
                             effort,
                             agent.config.reasoning_effort,
                             sizeof(agent.config.reasoning_effort));
}

esp_err_t solar_os_agent_set_tool_policy(solar_os_agent_tool_policy_t policy)
{
    const char *name = solar_os_agent_tool_policy_name(policy);
    if (strcmp(name, "unknown") == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = agent_settings_enter();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        solar_os_lifecycle_leave(&agent_settings);
        return err;
    }
    err = nvs_set_str(nvs, AGENT_NVS_TOOL_POLICY_KEY, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&agent_lock);
        agent.tool_policy = policy;
        portEXIT_CRITICAL(&agent_lock);
    }
    solar_os_lifecycle_leave(&agent_settings);
    return err;
}

esp_err_t solar_os_agent_set_max_tools(uint8_t max_tools)
{
    if (max_tools < SOLAR_OS_AGENT_MAX_TOOLS_MIN ||
        max_tools > SOLAR_OS_AGENT_MAX_TOOLS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = agent_settings_enter();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        solar_os_lifecycle_leave(&agent_settings);
        return err;
    }
    err = nvs_set_u8(nvs, AGENT_NVS_MAX_TOOLS_KEY, max_tools);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&agent_lock);
        agent.max_tools = max_tools;
        portEXIT_CRITICAL(&agent_lock);
    }
    solar_os_lifecycle_leave(&agent_settings);
    return err;
}

esp_err_t solar_os_agent_forget(void)
{
    esp_err_t err = agent_settings_enter();
    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&agent_lock);
    const bool running = agent.running;
    portEXIT_CRITICAL(&agent_lock);
    if (running) {
        solar_os_lifecycle_leave(&agent_settings);
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs;
    err = nvs_open(AGENT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        solar_os_lifecycle_leave(&agent_settings);
        return err;
    }
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        solar_os_lifecycle_leave(&agent_settings);
        return err;
    }

    portENTER_CRITICAL(&agent_lock);
    memset(&agent.config, 0, sizeof(agent.config));
    strlcpy(agent.config.reasoning_effort,
            AGENT_DEFAULT_REASONING_EFFORT,
            sizeof(agent.config.reasoning_effort));
    agent.tool_policy = AGENT_DEFAULT_TOOL_POLICY;
    agent.max_tools = SOLAR_OS_AGENT_DEFAULT_MAX_TOOLS;
    portEXIT_CRITICAL(&agent_lock);
    solar_os_lifecycle_leave(&agent_settings);
    return ESP_OK;
}

esp_err_t solar_os_agent_get_status(solar_os_agent_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    const esp_err_t err = solar_os_agent_init();
    if (err != ESP_OK) return err;

    portENTER_CRITICAL(&agent_lock);
    status->initialized = agent.initialized;
    status->configured =
        agent.config.endpoint[0] != '\0' && agent.config.model[0] != '\0';
    status->api_key_set = agent.config.api_key[0] != '\0';
    status->running = agent.running;
    strlcpy(status->provider,
            solar_os_agent_openai_provider.name,
            sizeof(status->provider));
    strlcpy(status->endpoint, agent.config.endpoint, sizeof(status->endpoint));
    strlcpy(status->model, agent.config.model, sizeof(status->model));
    strlcpy(status->reasoning_effort,
            agent.config.reasoning_effort,
            sizeof(status->reasoning_effort));
    status->tool_policy = agent.tool_policy;
    status->max_tools = agent.max_tools;
    status->last_tool_call_count = agent.last_tool_call_count;
    status->last_max_tools = agent.last_max_tools;
    status->request_count = agent.request_count;
    status->failure_count = agent.failure_count;
    status->tool_executed_count = agent.tool_executed_count;
    status->tool_denied_count = agent.tool_denied_count;
    status->tool_failed_count = agent.tool_failed_count;
    status->last_http_status = agent.last_http_status;
    status->last_error = agent.last_error;
    status->last_duration_ms = agent.last_duration_ms;
    status->last_bytes_received = agent.last_bytes_received;
    status->last_internal_before = agent.last_internal_before;
    status->last_internal_low = agent.last_internal_low;
    status->last_internal_after = agent.last_internal_after;
    status->last_internal_largest_before = agent.last_internal_largest_before;
    status->last_internal_largest_after = agent.last_internal_largest_after;
    status->last_psram_before = agent.last_psram_before;
    status->last_psram_after = agent.last_psram_after;
    portEXIT_CRITICAL(&agent_lock);
    return ESP_OK;
}

void solar_os_agent_provider_note_memory(void)
{
    const uint32_t current = agent_internal_free();
    portENTER_CRITICAL(&agent_lock);
    if (agent.running &&
        (agent.last_internal_low == 0 || current < agent.last_internal_low)) {
        agent.last_internal_low = current;
    }
    portEXIT_CRITICAL(&agent_lock);
}

bool solar_os_agent_provider_cancel_requested(void)
{
    portENTER_CRITICAL(&agent_lock);
    const bool cancel = agent.cancel_requested;
    solar_os_agent_cancel_fn should_cancel = agent.should_cancel;
    void *user_data = agent.cancel_user_data;
    portEXIT_CRITICAL(&agent_lock);
    return cancel || (should_cancel != NULL && should_cancel(user_data));
}

void solar_os_agent_provider_bind_request(solar_os_http_request_t *request)
{
    if (request == NULL) {
        return;
    }
    static agent_request_handle_t handle;
    handle.request = request;
    handle.refs = 0;
    handle.closing = false;
    portENTER_CRITICAL(&agent_lock);
    agent.active_request = &handle;
    portEXIT_CRITICAL(&agent_lock);
}

void solar_os_agent_provider_unbind_request(solar_os_http_request_t *request)
{
    agent_request_handle_t *handle = NULL;
    portENTER_CRITICAL(&agent_lock);
    if (agent.active_request != NULL &&
        agent.active_request->request == request) {
        handle = agent.active_request;
        handle->closing = true;
        agent.active_request = NULL;
    }
    portEXIT_CRITICAL(&agent_lock);

    if (handle != NULL) {
        while (true) {
            portENTER_CRITICAL(&agent_lock);
            const uint32_t refs = handle->refs;
            portEXIT_CRITICAL(&agent_lock);
            if (refs == 0) {
                break;
            }
            vTaskDelay(1);
        }
        handle->request = NULL;
    }
}

esp_err_t solar_os_agent_cancel(void)
{
    agent_request_handle_t *handle = NULL;
    solar_os_http_request_t *request = NULL;
    portENTER_CRITICAL(&agent_lock);
    agent.cancel_requested = true;
    handle = agent.active_request;
    if (handle != NULL && !handle->closing && handle->request != NULL) {
        handle->refs++;
        request = handle->request;
    }
    portEXIT_CRITICAL(&agent_lock);

    esp_err_t err = ESP_OK;
    if (request != NULL) {
        err = solar_os_http_request_cancel(request);
        portENTER_CRITICAL(&agent_lock);
        handle->refs--;
        portEXIT_CRITICAL(&agent_lock);
    }
    return err;
}

static esp_err_t agent_emit(const solar_os_agent_request_t *request,
                            solar_os_agent_event_type_t type,
                            const char *text,
                            const char *tool_name,
                            bool success)
{
    solar_os_agent_event_t event = {
        .type = type,
        .success = success,
    };
    if (text != NULL) {
        strlcpy(event.text, text, sizeof(event.text));
    }
    if (tool_name != NULL) {
        strlcpy(event.tool_name, tool_name, sizeof(event.tool_name));
    }
    return request->event_handler(&event, request->user_data);
}

static esp_err_t agent_capture_event(const solar_os_agent_event_t *event,
                                     void *user_data)
{
    agent_event_capture_t *capture = user_data;
    if (event == NULL || capture == NULL || capture->request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type == SOLAR_OS_AGENT_EVENT_TEXT_DELTA &&
        capture->assistant != NULL && event->text[0] != '\0') {
        const size_t length = strlen(event->text);
        const size_t remaining =
            AGENT_ASSISTANT_CAPTURE_MAX - capture->assistant_len;
        const size_t copy = length < remaining ? length : remaining;
        memcpy(capture->assistant + capture->assistant_len, event->text, copy);
        capture->assistant_len += copy;
        capture->assistant[capture->assistant_len] = '\0';
        capture->assistant_truncated = copy < length;
    }
    return capture->request->event_handler(event,
                                            capture->request->user_data);
}

static void agent_append_tool_summary(agent_run_turn_state_t *run,
                                      const char *tool_name,
                                      const char *outcome)
{
    if (run == NULL || tool_name == NULL || outcome == NULL ||
        run->tool_summary_len >= sizeof(run->tool_summary) - 1U) {
        return;
    }
    const int written = snprintf(
        run->tool_summary + run->tool_summary_len,
        sizeof(run->tool_summary) - run->tool_summary_len,
        "%s%s: %s",
        run->tool_summary_len == 0 ? "" : "\n",
        tool_name,
        outcome);
    if (written > 0) {
        const size_t available =
            sizeof(run->tool_summary) - run->tool_summary_len;
        run->tool_summary_len +=
            (size_t)written < available ? (size_t)written : available - 1U;
    }
}

static void agent_append_tool_context(char *context,
                                      size_t capacity,
                                      size_t *context_len,
                                      const char *tool_name,
                                      const char *tool_result)
{
    if (context == NULL || capacity == 0U || context_len == NULL ||
        tool_name == NULL || tool_result == NULL || *context_len >= capacity) {
        return;
    }
    const int prefix = snprintf(
        context + *context_len,
        capacity - *context_len,
        "%s%s returned:\n",
        *context_len == 0U ? "" : "\n",
        tool_name);
    if (prefix < 0 || (size_t)prefix >= capacity - *context_len) {
        return;
    }
    *context_len += (size_t)prefix;

    size_t copy = strlen(tool_result);
    if (copy > AGENT_TOOL_CONTEXT_RESULT_MAX) {
        copy = AGENT_TOOL_CONTEXT_RESULT_MAX;
    }
    const size_t available = capacity - *context_len;
    if (copy + 2U > available) {
        copy = available > 2U ? available - 2U : 0U;
    }
    if (copy > 0U) {
        memcpy(context + *context_len, tool_result, copy);
        *context_len += copy;
    }
    if (*context_len + 1U < capacity) {
        context[(*context_len)++] = '\n';
        context[*context_len] = '\0';
    }
}

static uint32_t agent_tool_call_signature(const char *tool_name,
                                          const char *arguments)
{
    uint32_t hash = 2166136261U;
    const char *parts[] = {
        tool_name != NULL ? tool_name : "",
        arguments != NULL ? arguments : "",
    };
    for (size_t part = 0U; part < sizeof(parts) / sizeof(parts[0]); part++) {
        for (const unsigned char *cursor =
                 (const unsigned char *)parts[part];
             *cursor != '\0';
             cursor++) {
            hash ^= *cursor;
            hash *= 16777619U;
        }
        hash ^= 0xffU;
        hash *= 16777619U;
    }
    return hash;
}

static bool agent_tool_signature_seen(const agent_run_turn_state_t *run,
                                      uint32_t signature)
{
    if (run == NULL) {
        return false;
    }
    for (uint8_t i = 0U; i < run->completed_tool_signature_count; i++) {
        if (run->completed_tool_signatures[i] == signature) {
            return true;
        }
    }
    return false;
}

static void agent_record_tool_signature(agent_run_turn_state_t *run,
                                        uint32_t signature)
{
    if (run == NULL ||
        run->completed_tool_signature_count >= AGENT_TOOL_SIGNATURE_MAX) {
        return;
    }
    run->completed_tool_signatures[run->completed_tool_signature_count++] =
        signature;
}

static bool agent_tool_info_by_name(const char *name,
                                    solar_os_agent_tool_info_t *info)
{
    if (name == NULL || info == NULL) {
        return false;
    }
    const size_t count = solar_os_agent_tools_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_agent_tool_info_t candidate;
        if (solar_os_agent_tools_get(i, &candidate) &&
            strcmp(name, candidate.provider.name) == 0) {
            *info = candidate;
            return true;
        }
    }
    return false;
}

static bool agent_tool_is_active(
    const char *name,
    const solar_os_agent_tool_descriptor_t *descriptors,
    size_t descriptor_count)
{
    if (name == NULL || descriptors == NULL) {
        return false;
    }
    for (size_t i = 0U; i < descriptor_count; i++) {
        if (descriptors[i].name != NULL &&
            strcmp(name, descriptors[i].name) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t agent_authorize_tool(
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    const solar_os_agent_tool_info_t *info,
    const char *arguments,
    bool *allowed)
{
    if (request == NULL || info == NULL || arguments == NULL ||
        allowed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_agent_tool_policy_decision_t decision =
        solar_os_agent_tools_policy_decision(policy, info->risk);
    *allowed = decision == SOLAR_OS_AGENT_TOOL_POLICY_ALLOW;
    if (decision != SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE) {
        return ESP_OK;
    }
    if (request->confirm_tool == NULL) {
        return ESP_OK;
    }
    return request->confirm_tool(info->provider.name,
                                 solar_os_agent_tool_risk_name(info->risk),
                                 arguments,
                                 allowed,
                                 request->user_data);
}

esp_err_t solar_os_agent_invoke_tool(const char *name, const char *arguments,
                                     const solar_os_agent_request_t *request,
                                     char *result, size_t capacity)
{
    if (!name || !arguments || !request || !result || !capacity) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = solar_os_agent_init();
    if (err != ESP_OK) return err;

    solar_os_agent_tool_info_t info;
    if (!agent_tool_info_by_name(name, &info)) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&agent_lock);
    const solar_os_agent_tool_policy_t policy = agent.tool_policy;
    portEXIT_CRITICAL(&agent_lock);

    bool allowed;
    const esp_err_t auth = agent_authorize_tool(request, policy, &info, arguments, &allowed);
    if (auth != ESP_OK) return auth;
    if (!allowed) return ESP_ERR_NOT_ALLOWED;
    return solar_os_agent_tools_execute(name, arguments, request, policy, true, result, capacity);
}

static esp_err_t agent_denied_tool_result(
    const solar_os_agent_tool_info_t *info,
    solar_os_agent_tool_policy_t policy,
    char *result,
    size_t result_len)
{
    if (info == NULL || result == NULL || result_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":false,\"error\":\"tool denied\","
        "\"policy\":\"%s\",\"risk\":\"%s\"}",
        solar_os_agent_tool_policy_name(policy),
        solar_os_agent_tool_risk_name(info->risk));
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_failed_tool_result(esp_err_t tool_error,
                                          char *result,
                                          size_t result_len)
{
    if (result == NULL || result_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":false,\"error\":\"%s\",\"recoverable\":true}",
        esp_err_to_name(tool_error));
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_inactive_tool_result(char *result, size_t result_len)
{
    if (result == NULL || result_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":false,\"error\":\"tool not active\","
        "\"recovery\":\"call tool_search with the exact task\"}");
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_activated_tool_result(char *result, size_t result_len)
{
    if (result == NULL || result_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":false,\"error\":\"tool activated\","
        "\"recovery\":\"retry the same tool call using its advertised schema\"}");
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t agent_duplicate_tool_result(char *result, size_t result_len)
{
    if (result == NULL || result_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(
        result,
        result_len,
        "{\"ok\":false,\"error\":\"duplicate tool call\","
        "\"recovery\":\"use the earlier result and finish the task\"}");
    return written >= 0 && (size_t)written < result_len ?
        ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void agent_note_tool_result(bool denied, esp_err_t result)
{
    portENTER_CRITICAL(&agent_lock);
    if (denied) {
        agent.tool_denied_count++;
    } else if (result == ESP_OK) {
        agent.tool_executed_count++;
    } else {
        agent.tool_failed_count++;
    }
    portEXIT_CRITICAL(&agent_lock);
}

typedef struct {
    int http_status;
    uint32_t duration_ms;
    uint32_t internal_before;
    uint32_t internal_low;
    uint32_t internal_after;
} agent_run_metrics_t;

static void agent_finish_request(esp_err_t error, agent_run_metrics_t *metrics)
{
    const uint32_t internal_after = agent_internal_free();
    const uint32_t internal_largest_after = agent_internal_largest();
    const uint32_t psram_after = agent_psram_free();

    portENTER_CRITICAL(&agent_lock);
    agent.running = false;
    agent.cancel_requested = false;
    agent.should_cancel = NULL;
    agent.cancel_user_data = NULL;
    agent.last_error = error;
    agent.last_internal_after = internal_after;
    agent.last_internal_largest_after = internal_largest_after;
    agent.last_psram_after = psram_after;
    if (metrics != NULL) {
        *metrics = (agent_run_metrics_t){
            .http_status = agent.last_http_status,
            .duration_ms = agent.last_duration_ms,
            .internal_before = agent.last_internal_before,
            .internal_low = agent.last_internal_low,
            .internal_after = agent.last_internal_after,
        };
    }
    if (error != ESP_OK) {
        agent.failure_count++;
    }
    portEXIT_CRITICAL(&agent_lock);
}

static esp_err_t agent_begin_run(const solar_os_agent_request_t *request,
                                 solar_os_agent_provider_config_t *config,
                                 solar_os_agent_tool_policy_t *tool_policy,
                                 uint8_t *max_tools)
{
    const esp_err_t err = agent_settings_enter();
    if (err != ESP_OK) return err;

    const uint32_t internal_before = agent_internal_free();
    const uint32_t internal_largest_before = agent_internal_largest();
    const uint32_t psram_before = agent_psram_free();
    portENTER_CRITICAL(&agent_lock);
    if (agent.running || !agent.config.endpoint[0] || !agent.config.model[0]) {
        portEXIT_CRITICAL(&agent_lock);
        solar_os_lifecycle_leave(&agent_settings);
        return ESP_ERR_INVALID_STATE;
    }
    *config = agent.config;
    *tool_policy = agent.tool_policy;
    *max_tools = agent.max_tools;
    agent.running = true;
    agent.cancel_requested = false;
    agent.should_cancel = request->should_cancel;
    agent.cancel_user_data = request->user_data;
    agent.request_count++;
    agent.last_http_status = -1;
    agent.last_error = ESP_OK;
    agent.last_duration_ms = 0;
    agent.last_bytes_received = 0;
    agent.last_tool_call_count = 0U;
    agent.last_max_tools = *max_tools;
    agent.last_internal_before = internal_before;
    agent.last_internal_low = agent.last_internal_before;
    agent.last_internal_largest_before = internal_largest_before;
    agent.last_psram_before = psram_before;
    portEXIT_CRITICAL(&agent_lock);
    solar_os_lifecycle_leave(&agent_settings);
    return ESP_OK;
}

esp_err_t solar_os_agent_run(const solar_os_agent_request_t *request)
{
    if (request == NULL || request->prompt == NULL ||
        request->event_handler == NULL ||
        !agent_string_valid(request->prompt, SOLAR_OS_AGENT_PROMPT_MAX, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_agent_images_validate(request->images, request->image_count);
    if (err != ESP_OK) return err;
    err = solar_os_agent_audio_validate(request->audio);
    if (err != ESP_OK) return err;
    solar_os_agent_provider_config_t config;
    solar_os_agent_tool_policy_t tool_policy;
    uint8_t max_tools;
    err = agent_begin_run(request, &config, &tool_policy, &max_tools);
    if (err != ESP_OK) return err;

    (void)agent_emit(request,
                     SOLAR_OS_AGENT_EVENT_STATUS,
                     "connecting",
                     NULL,
                     false);

    solar_os_agent_tool_descriptor_t
        tool_descriptors[SOLAR_OS_AGENT_TOOL_ACTIVE_MAX];
    size_t tool_count =
        solar_os_agent_tools_collect(request,
                                     tool_policy,
                                     tool_descriptors,
                                     SOLAR_OS_AGENT_TOOL_ACTIVE_MAX);
    agent_run_turn_state_t *run = solar_os_memory_calloc(
        1,
        sizeof(*run),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.turn-state");
    char *tool_result =
        solar_os_memory_calloc(1,
                               SOLAR_OS_AGENT_TOOL_RESULT_MAX,
                               SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                               "agent.tool-result");
    char *assistant = solar_os_memory_calloc(
        1,
        AGENT_ASSISTANT_CAPTURE_MAX + 1U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.assistant-capture");
    if (run == NULL || tool_result == NULL || assistant == NULL) {
        err = ESP_ERR_NO_MEM;
        agent_finish_request(err, NULL);
        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_DONE,
                         esp_err_to_name(err),
                         NULL,
                         false);
        solar_os_memory_free(tool_result);
        solar_os_memory_free(assistant);
        solar_os_memory_free(run);
        explicit_bzero(config.api_key, sizeof(config.api_key));
        return err;
    }

    run->capture = (agent_event_capture_t){
        .request = request,
        .assistant = assistant,
    };
    solar_os_agent_history_t history = {0};
    const solar_os_agent_provider_resume_mode_t resume_mode =
        solar_os_agent_openai_provider.resume_mode != NULL ?
            solar_os_agent_openai_provider.resume_mode(&config) :
            SOLAR_OS_AGENT_PROVIDER_RESUME_LOCAL_HISTORY;
    if (request->conversation_id != NULL &&
        request->conversation_id[0] != '\0') {
        err = solar_os_agent_conversation_load_history(
            request->conversation_id,
            resume_mode == SOLAR_OS_AGENT_PROVIDER_RESUME_LOCAL_HISTORY,
            &history);
        if (err != ESP_OK) {
            agent_finish_request(err, NULL);
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_DONE,
                             esp_err_to_name(err),
                             NULL,
                             false);
            solar_os_memory_free(assistant);
            solar_os_memory_free(tool_result);
            solar_os_memory_free(run);
            explicit_bzero(config.api_key, sizeof(config.api_key));
            return err;
        }
    }
    char *tool_context = NULL;
    size_t tool_context_len = 0U;
    if (resume_mode == SOLAR_OS_AGENT_PROVIDER_RESUME_LOCAL_HISTORY) {
        tool_context = solar_os_memory_calloc(
            1U,
            AGENT_TOOL_CONTEXT_MAX,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
            "agent.tool-context");
        if (tool_context == NULL) {
            err = ESP_ERR_NO_MEM;
            solar_os_agent_conversation_free_history(&history);
            agent_finish_request(err, NULL);
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_DONE,
                             esp_err_to_name(err),
                             NULL,
                             false);
            solar_os_memory_free(assistant);
            solar_os_memory_free(tool_result);
            solar_os_memory_free(run);
            explicit_bzero(config.api_key, sizeof(config.api_key));
            return err;
        }
    }
    solar_os_agent_provider_turn_t turn = {
        .prompt = request->prompt,
        .images = request->images,
        .image_count = request->image_count,
        .audio = request->audio,
        .history = history.messages,
        .history_count = history.message_count,
        .previous_response_id =
            resume_mode == SOLAR_OS_AGENT_PROVIDER_RESUME_REMOTE_ID &&
                    history.provider_response_id[0] != '\0' ?
                history.provider_response_id : NULL,
        .tools = tool_descriptors,
        .tool_count = tool_count,
        .tool_context = tool_context,
    };

    for (uint32_t turn_index = 0;
         turn_index <= (uint32_t)max_tools;
         turn_index++) {
        const size_t assistant_before = run->capture.assistant_len;
        uint8_t empty_retries = 0U;
        do {
            memset(&run->provider_result, 0, sizeof(run->provider_result));
            run->provider_result.http_status = -1;
            err = solar_os_agent_openai_provider.run_turn(
                &config,
                &turn,
                agent_capture_event,
                &run->capture,
                &run->provider_result);
            portENTER_CRITICAL(&agent_lock);
            agent.last_http_status = run->provider_result.http_status;
            agent.last_duration_ms += run->provider_result.duration_ms;
            agent.last_bytes_received += run->provider_result.bytes_received;
            portEXIT_CRITICAL(&agent_lock);
            if (err != ESP_OK || run->provider_result.tool_call ||
                run->capture.assistant_len > assistant_before) {
                break;
            }
            if (resume_mode != SOLAR_OS_AGENT_PROVIDER_RESUME_LOCAL_HISTORY ||
                empty_retries >= AGENT_EMPTY_RESPONSE_RETRY_MAX) {
                err = ESP_ERR_INVALID_RESPONSE;
                (void)agent_emit(request,
                                 SOLAR_OS_AGENT_EVENT_ERROR,
                                 "provider returned an empty response",
                                 NULL,
                                 false);
                break;
            }
            empty_retries++;
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_STATUS,
                             "empty response, retrying",
                             NULL,
                             false);
        } while (true);
        if (err != ESP_OK) {
            break;
        }
        if (!run->provider_result.tool_call) {
            err = ESP_OK;
            break;
        }
        if (tool_context != NULL && run->tool_name[0] != '\0') {
            agent_append_tool_context(tool_context,
                                      AGENT_TOOL_CONTEXT_MAX,
                                      &tool_context_len,
                                      run->tool_name,
                                      tool_result);
        }
        if (turn_index >= (uint32_t)max_tools) {
            char limit_message[64];
            snprintf(limit_message,
                     sizeof(limit_message),
                     "tool call limit reached (max %u)",
                     (unsigned int)max_tools);
            err = ESP_ERR_INVALID_STATE;
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_ERROR,
                             limit_message,
                             run->provider_result.tool_name,
                             false);
            break;
        }
        portENTER_CRITICAL(&agent_lock);
        agent.last_tool_call_count = (uint8_t)(turn_index + 1U);
        portEXIT_CRITICAL(&agent_lock);

        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_TOOL_CALL,
                         run->provider_result.tool_arguments,
                         run->provider_result.tool_name,
                         false);
        solar_os_agent_tool_info_t tool_info;
        const bool active =
            turn.tools != NULL &&
            agent_tool_is_active(run->provider_result.tool_name,
                                 tool_descriptors,
                                 tool_count);
        const bool known =
            agent_tool_info_by_name(run->provider_result.tool_name,
                                    &tool_info);
        const uint32_t tool_signature =
            agent_tool_call_signature(run->provider_result.tool_name,
                                      run->provider_result.tool_arguments);
        const bool duplicate =
            active && known &&
            tool_info.risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY &&
            agent_tool_signature_seen(run, tool_signature);
        bool allowed = false;
        bool denied = false;
        bool activated = false;
        bool tool_succeeded = false;
        if (!active && known && turn.tools != NULL) {
            const size_t activated_count =
                solar_os_agent_tools_activate(
                    run->provider_result.tool_name,
                    request,
                    tool_policy,
                    tool_descriptors,
                    tool_count,
                    SOLAR_OS_AGENT_TOOL_ACTIVE_MAX);
            activated = activated_count > tool_count;
            tool_count = activated_count;
            turn.tool_count = tool_count;
            if (activated) {
                err = agent_activated_tool_result(
                    tool_result,
                    SOLAR_OS_AGENT_TOOL_RESULT_MAX);
            } else {
                agent_note_tool_result(false, ESP_ERR_NOT_SUPPORTED);
                err = agent_inactive_tool_result(
                    tool_result,
                    SOLAR_OS_AGENT_TOOL_RESULT_MAX);
            }
        } else if (!active || !known) {
            agent_note_tool_result(false, ESP_ERR_NOT_SUPPORTED);
            err = agent_inactive_tool_result(
                tool_result,
                SOLAR_OS_AGENT_TOOL_RESULT_MAX);
        } else if (duplicate) {
            run->duplicate_tool_calls++;
            err = agent_duplicate_tool_result(
                tool_result,
                SOLAR_OS_AGENT_TOOL_RESULT_MAX);
        } else {
            err = agent_authorize_tool(request,
                                       tool_policy,
                                       &tool_info,
                                       run->provider_result.tool_arguments,
                                       &allowed);
            if (err != ESP_OK) {
                break;
            }
            if (!allowed) {
                denied = true;
                err = agent_denied_tool_result(
                    &tool_info,
                    tool_policy,
                    tool_result,
                    SOLAR_OS_AGENT_TOOL_RESULT_MAX);
                agent_note_tool_result(true, err);
            } else {
                const esp_err_t tool_error =
                    solar_os_agent_tools_execute(
                        run->provider_result.tool_name,
                        run->provider_result.tool_arguments,
                        request,
                        tool_policy,
                        true,
                        tool_result,
                        SOLAR_OS_AGENT_TOOL_RESULT_MAX);
                agent_note_tool_result(false, tool_error);
                tool_succeeded = tool_error == ESP_OK;
                if (tool_succeeded &&
                    tool_info.risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY) {
                    agent_record_tool_signature(run, tool_signature);
                }
                err = tool_succeeded ?
                    ESP_OK :
                    agent_failed_tool_result(
                        tool_error,
                        tool_result,
                        SOLAR_OS_AGENT_TOOL_RESULT_MAX);
            }
        }
        if (err != ESP_OK) {
            break;
        }
        if (tool_succeeded &&
            solar_os_agent_tools_is_discovery(
                run->provider_result.tool_name)) {
            tool_count = solar_os_agent_tools_collect_discovered(
                run->provider_result.tool_arguments,
                request,
                tool_policy,
                tool_descriptors,
                SOLAR_OS_AGENT_TOOL_ACTIVE_MAX);
            turn.tool_count = tool_count;
        }
        (void)agent_emit(request,
                         SOLAR_OS_AGENT_EVENT_TOOL_RESULT,
                         tool_result,
                         run->provider_result.tool_name,
                         tool_succeeded);
        agent_append_tool_summary(run,
                                  run->provider_result.tool_name,
                                  denied ? "denied" :
                                      (activated ? "activated" :
                                          (duplicate ? "skipped" :
                                              (tool_succeeded ?
                                                  "complete" : "failed"))));
        strlcpy(run->tool_call_id,
                run->provider_result.tool_call_id,
                sizeof(run->tool_call_id));
        strlcpy(run->tool_name,
                run->provider_result.tool_name,
                sizeof(run->tool_name));
        strlcpy(run->tool_arguments,
                run->provider_result.tool_arguments,
                sizeof(run->tool_arguments));
        strlcpy(run->previous_response_id,
                run->provider_result.response_id,
                sizeof(run->previous_response_id));
        turn.continuation = true;
        turn.previous_response_id = run->previous_response_id;
        turn.tool_call_id = run->tool_call_id;
        turn.tool_name = run->tool_name;
        turn.tool_arguments = run->tool_arguments;
        turn.tool_result = tool_result;
        if (turn_index + 1U >= (uint32_t)max_tools ||
            run->duplicate_tool_calls >= AGENT_DUPLICATE_TOOL_LIMIT) {
            turn.tools = NULL;
            turn.tool_count = 0U;
        }
    }

    const bool cancelled = solar_os_agent_provider_cancel_requested();
    if (cancelled) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK && request->next_conversation_id != NULL &&
        request->next_conversation_id_len >=
            SOLAR_OS_AGENT_CONVERSATION_ID_MAX) {
        const char *saved_assistant =
            run->capture.assistant_len > 0 ?
                run->capture.assistant : "[No text response]";
        char *saved_prompt = NULL;
        if (request->image_count || request->audio) {
            saved_prompt = solar_os_memory_alloc(SOLAR_OS_AGENT_PROMPT_MAX + 128,
                SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "agent.image-history");
            if (saved_prompt) {
                if (request->audio) snprintf(saved_prompt, SOLAR_OS_AGENT_PROMPT_MAX + 128,
                    "%s\n[Attachments: %u image(s), 1 audio file; encoded content omitted from local history.]",
                    request->prompt, (unsigned)request->image_count);
                else snprintf(saved_prompt, SOLAR_OS_AGENT_PROMPT_MAX + 128,
                    "%s\n[Image attachments: %u; image content omitted from local history.]",
                    request->prompt, (unsigned)request->image_count);
            }
        }
        err = (request->image_count || request->audio) && !saved_prompt ? ESP_ERR_NO_MEM : solar_os_agent_conversation_commit(
            request->conversation_id,
            solar_os_agent_openai_provider.name,
            config.model,
            saved_prompt ? saved_prompt : request->prompt,
            saved_assistant,
            run->tool_summary,
            run->provider_result.response_id,
            request->next_conversation_id,
            request->next_conversation_id_len);
        solar_os_memory_free(saved_prompt);
        if (err != ESP_OK) {
            (void)agent_emit(request,
                             SOLAR_OS_AGENT_EVENT_ERROR,
                             "conversation could not be saved",
                             NULL,
                             false);
        }
    }
    solar_os_agent_conversation_free_history(&history);
    if (tool_context != NULL) {
        memset(tool_context, 0, AGENT_TOOL_CONTEXT_MAX);
        solar_os_memory_free(tool_context);
    }
    memset(assistant, 0, AGENT_ASSISTANT_CAPTURE_MAX + 1U);
    solar_os_memory_free(assistant);
    memset(tool_result, 0, SOLAR_OS_AGENT_TOOL_RESULT_MAX);
    solar_os_memory_free(tool_result);
    agent_run_metrics_t metrics;
    agent_finish_request(err, &metrics);
    (void)agent_emit(request,
                     SOLAR_OS_AGENT_EVENT_DONE,
                     err == ESP_OK ? "complete" :
                         (cancelled ? "cancelled" : esp_err_to_name(err)),
                     NULL,
                     err == ESP_OK);
    explicit_bzero(config.api_key, sizeof(config.api_key));
    memset(run, 0, sizeof(*run));
    solar_os_memory_free(run);
    SOLAR_OS_LOGI(TAG,
                  "request done: err=%s http=%d duration=%" PRIu32
                  " internal=%" PRIu32 " low=%" PRIu32 " after=%" PRIu32,
                  esp_err_to_name(err),
                  metrics.http_status,
                  metrics.duration_ms,
                  metrics.internal_before,
                  metrics.internal_low,
                  metrics.internal_after);
    return err;
}
