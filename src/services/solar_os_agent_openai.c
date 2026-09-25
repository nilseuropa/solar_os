#include "solar_os_agent_provider.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_json.h"
#include "solar_os_memory.h"

#define AGENT_OPENAI_BODY_MAX (32U * 1024U)
#define AGENT_OPENAI_HISTORY_MAX (16U * 1024U)
#define AGENT_OPENAI_TOOLS_MAX 6144U
#define AGENT_OPENAI_SSE_LINE_MAX (24U * 1024U)
#define AGENT_OPENAI_ERROR_MAX 512U
#define AGENT_OPENAI_HTTP_TIMEOUT_MS 15000U
#define AGENT_OPENAI_DEADLINE_MS 90000U
#define AGENT_OPENAI_RX_BUFFER 1024U
#define AGENT_OPENAI_TX_BUFFER 1024U
#define AGENT_OPENAI_OUTPUT_MAX (16U * 1024U)
#define AGENT_OPENAI_INSTRUCTIONS                                           \
    "You are the native SolarOS agent. Use tools for device state. Before " \
    "using a storage, display, hardware, GPIO, bus, network, sensor, or "      \
    "script tool that is not visible, call tool_search with the exact task. " \
    "Before writing or running Python or Lua that uses SolarOS APIs, call "  \
    "solaros_reference with exactly one argument shaped "                    \
    "{\\\"query\\\":\\\"lua gfx drawing\\\"}, combining the language and " \
    "task in that query. Treat its guidance "                                \
    "and matched contracts as mandatory. Use documented symbols and "        \
    "constants exactly; never replace them with guessed strings or numbers. " \
    "Make one comprehensive reference query; query again only for a "         \
    "distinctly missing API instead of guessing. "                            \
    "Never invent API names, device names, display targets, bus names, or "   \
    "GPIOs. Use only values supplied by the user or verified through SolarOS "\
    "APIs. Use storage_stat for exact path existence or metadata; "            \
    "storage_search searches file contents only. For nontrivial existing "     \
    "files, use storage_search and "                                            \
    "storage_read_range, then apply storage_patch with the returned SHA-256. " \
    "Run the saved program with script_run_file and use its structured error " \
    "before editing again. Never claim that a file was saved or changed, or "  \
    "that a script ran, unless the corresponding tool returned success in "   \
    "the current turn. Do not repeat a read-only tool with the same arguments; "\
    "use its earlier result and continue the task. Before writing graphics "   \
    "code for an attached "                                                    \
    "display, activate "                                                       \
    "display tools with tool_search, call display_list, and use only a "       \
    "returned ready target. Keep answers concise."

typedef enum {
    AGENT_OPENAI_API_CHAT_COMPLETIONS = 0,
    AGENT_OPENAI_API_RESPONSES,
} agent_openai_api_t;

typedef struct {
    solar_os_agent_event_fn event_handler;
    void *user_data;
    char *line;
    size_t line_len;
    char error_body[AGENT_OPENAI_ERROR_MAX];
    size_t error_len;
    solar_os_agent_provider_result_t *result;
    agent_openai_api_t api;
    size_t output_bytes;
    uint32_t tool_call_index;
    bool saw_done;
    bool saw_finish;
    bool saw_payload;
    bool parse_error;
    bool response_error;
    bool too_many_tools;
    bool tool_call_index_set;
} agent_openai_stream_t;

static esp_err_t agent_openai_emit(agent_openai_stream_t *stream,
                                   solar_os_agent_event_type_t type,
                                   const char *text)
{
    if (stream == NULL || stream->event_handler == NULL) {
        return ESP_OK;
    }

    if (type == SOLAR_OS_AGENT_EVENT_TEXT_DELTA && text != NULL) {
        const size_t len = strlen(text);
        if (stream->output_bytes + len > AGENT_OPENAI_OUTPUT_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        stream->output_bytes += len;
    }

    const char *remaining = text != NULL ? text : "";
    do {
        solar_os_agent_event_t event = {
            .type = type,
        };
        const size_t len = strlen(remaining);
        const size_t copy = len >= sizeof(event.text) ? sizeof(event.text) - 1U : len;
        memcpy(event.text, remaining, copy);
        event.text[copy] = '\0';
        const esp_err_t err = stream->event_handler(&event, stream->user_data);
        if (err != ESP_OK) {
            return err;
        }
        remaining += copy;
        if (copy == 0) {
            break;
        }
    } while (*remaining != '\0');
    return ESP_OK;
}

static esp_err_t agent_openai_append(char *buffer,
                                     size_t capacity,
                                     const char *fragment)
{
    if (buffer == NULL || capacity == 0 || fragment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t used = strlen(buffer);
    const size_t len = strlen(fragment);
    if (used + len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer + used, fragment, len + 1U);
    return ESP_OK;
}

static esp_err_t agent_openai_append_json_string(
    const solar_os_json_value_t *root,
    const char *path,
    char *buffer,
    size_t capacity)
{
    if (root == NULL || path == NULL || buffer == NULL || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_json_value_t *value = solar_os_json_path_get(root, path);
    if (value == NULL) {
        return ESP_OK;
    }
    const size_t used = strlen(buffer);
    if (used >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_json_get_string(value,
                                    buffer + used,
                                    capacity - used);
}

static esp_err_t agent_openai_parse_usage(agent_openai_stream_t *stream,
                                          const solar_os_json_value_t *root)
{
    uint32_t prompt = 0;
    uint32_t completion = 0;
    uint32_t total = 0;
    if (solar_os_json_get_path_uint32(root, "usage.prompt_tokens", &prompt) != ESP_OK &&
        solar_os_json_get_path_uint32(root, "usage.input_tokens", &prompt) != ESP_OK) {
        return ESP_OK;
    }
    (void)solar_os_json_get_path_uint32(root, "usage.completion_tokens", &completion);
    if (completion == 0) {
        (void)solar_os_json_get_path_uint32(root, "usage.output_tokens", &completion);
    }
    (void)solar_os_json_get_path_uint32(root, "usage.total_tokens", &total);

    solar_os_agent_event_t event = {
        .type = SOLAR_OS_AGENT_EVENT_USAGE,
        .prompt_tokens = prompt,
        .completion_tokens = completion,
        .total_tokens = total != 0 ? total : prompt + completion,
    };
    return stream->event_handler(&event, stream->user_data);
}

static esp_err_t agent_openai_parse_tool_delta(agent_openai_stream_t *stream,
                                                const solar_os_json_value_t *delta)
{
    const solar_os_json_value_t *calls =
        solar_os_json_object_get(delta, "tool_calls");
    if (calls == NULL) {
        return ESP_OK;
    }
    if (!solar_os_json_is_array(calls) || solar_os_json_array_size(calls) != 1U) {
        stream->too_many_tools = true;
        return ESP_ERR_NOT_SUPPORTED;
    }
    const solar_os_json_value_t *call = solar_os_json_array_get(calls, 0);
    if (!solar_os_json_is_object(call)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t call_index = 0;
    if (solar_os_json_get_path_uint32(call, "index", &call_index) != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (stream->tool_call_index_set &&
        stream->tool_call_index != call_index) {
        stream->too_many_tools = true;
        return ESP_ERR_NOT_SUPPORTED;
    }
    stream->tool_call_index = call_index;
    stream->tool_call_index_set = true;

    esp_err_t err =
        agent_openai_append_json_string(
            call,
            "id",
            stream->result->tool_call_id,
            sizeof(stream->result->tool_call_id));
    if (err != ESP_OK) {
        return err;
    }
    err = agent_openai_append_json_string(
        call,
        "function.name",
        stream->result->tool_name,
        sizeof(stream->result->tool_name));
    if (err != ESP_OK) {
        return err;
    }
    err = agent_openai_append_json_string(
        call,
        "function.arguments",
        stream->result->tool_arguments,
        sizeof(stream->result->tool_arguments));
    if (err != ESP_OK) {
        return err;
    }
    stream->result->tool_call = true;
    return ESP_OK;
}

static esp_err_t agent_openai_parse_chat_data(agent_openai_stream_t *stream,
                                              const char *data)
{
    if (strcmp(data, "[DONE]") == 0) {
        stream->saw_done = true;
        return ESP_OK;
    }

    solar_os_json_doc_t *doc = NULL;
    esp_err_t err = solar_os_json_parse_cstr(data, &doc);
    if (err != ESP_OK) {
        stream->parse_error = true;
        return ESP_ERR_INVALID_RESPONSE;
    }

    const solar_os_json_value_t *root = solar_os_json_root(doc);
    stream->saw_payload = true;
    const solar_os_json_value_t *delta =
        solar_os_json_path_get(root, "choices[0].delta");
    if (solar_os_json_is_object(delta)) {
        char content[256];
        if (solar_os_json_get_path_string(delta,
                                          "content",
                                          content,
                                          sizeof(content)) == ESP_OK) {
            err = agent_openai_emit(stream,
                                    SOLAR_OS_AGENT_EVENT_TEXT_DELTA,
                                    content);
        }
        if (err == ESP_OK) {
            err = agent_openai_parse_tool_delta(stream, delta);
        }
    }
    if (err == ESP_OK) {
        char finish_reason[32];
        if (solar_os_json_get_path_string(root,
                                          "choices[0].finish_reason",
                                          finish_reason,
                                          sizeof(finish_reason)) == ESP_OK &&
            finish_reason[0] != '\0') {
            stream->saw_finish = true;
        }
        err = agent_openai_parse_usage(stream, root);
    }
    solar_os_json_free(doc);
    return err;
}

static esp_err_t agent_openai_capture_response_id(
    agent_openai_stream_t *stream,
    const solar_os_json_value_t *root,
    const char *path)
{
    char response_id[SOLAR_OS_AGENT_RESPONSE_ID_MAX];
    if (solar_os_json_get_path_string(root,
                                      path,
                                      response_id,
                                      sizeof(response_id)) != ESP_OK) {
        return ESP_OK;
    }
    if (stream->result->response_id[0] != '\0' &&
        strcmp(stream->result->response_id, response_id) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(stream->result->response_id,
            response_id,
            sizeof(stream->result->response_id));
    return ESP_OK;
}

static esp_err_t agent_openai_capture_response_tool(
    agent_openai_stream_t *stream,
    const solar_os_json_value_t *item)
{
    char type[32];
    if (!solar_os_json_is_object(item) ||
        solar_os_json_get_path_string(item,
                                      "type",
                                      type,
                                      sizeof(type)) != ESP_OK ||
        strcmp(type, "function_call") != 0) {
        return ESP_OK;
    }

    char call_id[SOLAR_OS_AGENT_TOOL_CALL_ID_MAX];
    if (solar_os_json_get_path_string(item,
                                      "call_id",
                                      call_id,
                                      sizeof(call_id)) != ESP_OK ||
        call_id[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (stream->result->tool_call &&
        stream->result->tool_call_id[0] != '\0' &&
        strcmp(stream->result->tool_call_id, call_id) != 0) {
        stream->too_many_tools = true;
        return ESP_ERR_NOT_SUPPORTED;
    }

    strlcpy(stream->result->tool_call_id,
            call_id,
            sizeof(stream->result->tool_call_id));
    (void)solar_os_json_get_path_string(item,
                                        "name",
                                        stream->result->tool_name,
                                        sizeof(stream->result->tool_name));
    if (stream->result->tool_arguments[0] == '\0') {
        (void)solar_os_json_get_path_string(
            item,
            "arguments",
            stream->result->tool_arguments,
            sizeof(stream->result->tool_arguments));
    }
    stream->result->tool_call = true;
    return ESP_OK;
}

static esp_err_t agent_openai_parse_response_error(
    agent_openai_stream_t *stream,
    const solar_os_json_value_t *root)
{
    char message[SOLAR_OS_AGENT_EVENT_TEXT_MAX];
    if (solar_os_json_get_path_string(root,
                                      "response.error.message",
                                      message,
                                      sizeof(message)) != ESP_OK &&
        solar_os_json_get_path_string(root,
                                      "error.message",
                                      message,
                                      sizeof(message)) != ESP_OK &&
        solar_os_json_get_path_string(root,
                                      "message",
                                      message,
                                      sizeof(message)) != ESP_OK) {
        strlcpy(message, "Responses API request failed", sizeof(message));
    }
    stream->response_error = true;
    stream->saw_finish = true;
    return agent_openai_emit(stream, SOLAR_OS_AGENT_EVENT_ERROR, message);
}

static esp_err_t agent_openai_parse_responses_data(agent_openai_stream_t *stream,
                                                   const char *data)
{
    solar_os_json_doc_t *doc = NULL;
    esp_err_t err = solar_os_json_parse_cstr(data, &doc);
    if (err != ESP_OK) {
        stream->parse_error = true;
        return ESP_ERR_INVALID_RESPONSE;
    }

    const solar_os_json_value_t *root = solar_os_json_root(doc);
    char type[64];
    if (solar_os_json_get_path_string(root,
                                      "type",
                                      type,
                                      sizeof(type)) != ESP_OK) {
        solar_os_json_free(doc);
        stream->parse_error = true;
        return ESP_ERR_INVALID_RESPONSE;
    }
    stream->saw_payload = true;

    if (strcmp(type, "response.created") == 0 ||
        strcmp(type, "response.in_progress") == 0) {
        err = agent_openai_capture_response_id(stream, root, "response.id");
    } else if (strcmp(type, "response.output_text.delta") == 0) {
        char delta[256];
        if (solar_os_json_get_path_string(root,
                                          "delta",
                                          delta,
                                          sizeof(delta)) == ESP_OK) {
            err = agent_openai_emit(stream,
                                    SOLAR_OS_AGENT_EVENT_TEXT_DELTA,
                                    delta);
        }
    } else if (strcmp(type, "response.output_item.added") == 0 ||
               strcmp(type, "response.output_item.done") == 0) {
        err = agent_openai_capture_response_tool(
            stream,
            solar_os_json_object_get(root, "item"));
    } else if (strcmp(type, "response.function_call_arguments.delta") == 0) {
        char delta[256];
        if (solar_os_json_get_path_string(root,
                                          "delta",
                                          delta,
                                          sizeof(delta)) == ESP_OK) {
            err = agent_openai_append(stream->result->tool_arguments,
                                      sizeof(stream->result->tool_arguments),
                                      delta);
        }
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        if (stream->result->tool_arguments[0] == '\0') {
            (void)solar_os_json_get_path_string(
                root,
                "arguments",
                stream->result->tool_arguments,
                sizeof(stream->result->tool_arguments));
        }
    } else if (strcmp(type, "response.completed") == 0) {
        stream->saw_finish = true;
        err = agent_openai_capture_response_id(stream, root, "response.id");
        if (err == ESP_OK) {
            const solar_os_json_value_t *response =
                solar_os_json_object_get(root, "response");
            err = agent_openai_parse_usage(stream, response);
        }
    } else if (strcmp(type, "response.failed") == 0 ||
               strcmp(type, "response.incomplete") == 0 ||
               strcmp(type, "error") == 0) {
        err = agent_openai_parse_response_error(stream, root);
    }

    solar_os_json_free(doc);
    return err;
}

static esp_err_t agent_openai_parse_data(agent_openai_stream_t *stream,
                                         const char *data)
{
    if (stream->api == AGENT_OPENAI_API_RESPONSES) {
        return agent_openai_parse_responses_data(stream, data);
    }
    return agent_openai_parse_chat_data(stream, data);
}

static esp_err_t agent_openai_process_line(agent_openai_stream_t *stream)
{
    while (stream->line_len > 0 &&
           (stream->line[stream->line_len - 1U] == '\r' ||
            stream->line[stream->line_len - 1U] == '\n')) {
        stream->line[--stream->line_len] = '\0';
    }
    if (stream->line_len == 0 || stream->line[0] == ':') {
        return ESP_OK;
    }
    if (strncmp(stream->line, "data:", 5) != 0) {
        return ESP_OK;
    }

    const char *data = stream->line + 5;
    while (*data == ' ') {
        data++;
    }
    return agent_openai_parse_data(stream, data);
}

static esp_err_t agent_openai_feed(agent_openai_stream_t *stream,
                                   const uint8_t *data,
                                   size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const char ch = (char)data[i];
        if (ch == '\n') {
            stream->line[stream->line_len] = '\0';
            const esp_err_t err = agent_openai_process_line(stream);
            stream->line_len = 0;
            stream->line[0] = '\0';
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }
        if (stream->line_len + 1U >= AGENT_OPENAI_SSE_LINE_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        stream->line[stream->line_len++] = ch;
    }
    return ESP_OK;
}

static esp_err_t agent_openai_http_event(const solar_os_http_event_t *event,
                                         void *user_data)
{
    agent_openai_stream_t *stream = user_data;
    if (event == NULL || stream == NULL) {
        return ESP_OK;
    }
    solar_os_agent_provider_note_memory();
    if (solar_os_agent_provider_cancel_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA ||
        event->data == NULL ||
        event->data_len == 0) {
        return ESP_OK;
    }
    if (event->status_code < 200 || event->status_code >= 300) {
        const size_t remaining = sizeof(stream->error_body) - 1U - stream->error_len;
        const size_t copy = event->data_len < remaining ? event->data_len : remaining;
        memcpy(stream->error_body + stream->error_len, event->data, copy);
        stream->error_len += copy;
        stream->error_body[stream->error_len] = '\0';
        return ESP_OK;
    }
    return agent_openai_feed(stream, event->data, event->data_len);
}

static esp_err_t agent_openai_escape(const char *source,
                                     char **out,
                                     const char *tag)
{
    if (source == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    const size_t capacity = strlen(source) * 6U + 1U;
    char *escaped = solar_os_memory_alloc(capacity,
                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                          tag);
    if (escaped == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = solar_os_json_escape_string(source, escaped, capacity);
    if (err != ESP_OK) {
        solar_os_memory_free(escaped);
        return err;
    }
    *out = escaped;
    return ESP_OK;
}

static bool agent_openai_is_official_chat_endpoint(const char *endpoint)
{
    static const char official_endpoint[] =
        "https://api.openai.com/v1/chat/completions";
    if (endpoint == NULL) {
        return false;
    }

    const size_t official_len = sizeof(official_endpoint) - 1U;
    if (strncmp(endpoint, official_endpoint, official_len) != 0) {
        return false;
    }
    return endpoint[official_len] == '\0' ||
        (endpoint[official_len] == '/' && endpoint[official_len + 1U] == '\0') ||
        endpoint[official_len] == '?';
}

static bool agent_openai_is_responses_endpoint(const char *endpoint)
{
    static const char suffix[] = "/responses";
    if (endpoint == NULL) {
        return false;
    }

    const char *end = strpbrk(endpoint, "?#");
    if (end == NULL) {
        end = endpoint + strlen(endpoint);
    }
    while (end > endpoint && end[-1] == '/') {
        end--;
    }
    const size_t suffix_len = sizeof(suffix) - 1U;
    return (size_t)(end - endpoint) >= suffix_len &&
        memcmp(end - suffix_len, suffix, suffix_len) == 0;
}

static esp_err_t agent_openai_append_format(char *buffer,
                                            size_t capacity,
                                            size_t *used,
                                            const char *format,
                                            ...)
{
    if (buffer == NULL || capacity == 0 || used == NULL ||
        *used >= capacity || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer + *used,
                                  capacity - *used,
                                  format,
                                  args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *used) {
        return ESP_ERR_INVALID_SIZE;
    }
    *used += (size_t)written;
    return ESP_OK;
}

static esp_err_t agent_openai_build_tools(
    const solar_os_agent_provider_turn_t *turn,
    agent_openai_api_t api,
    char **out_tools)
{
    if (turn == NULL || out_tools == NULL ||
        (turn->tool_count > 0 && turn->tools == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_tools = NULL;

    char *tools = solar_os_memory_alloc(AGENT_OPENAI_TOOLS_MAX,
                                         SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "agent.tools");
    if (tools == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    esp_err_t err = agent_openai_append_format(tools,
                                                AGENT_OPENAI_TOOLS_MAX,
                                                &used,
                                                "[");
    for (size_t i = 0; err == ESP_OK && i < turn->tool_count; i++) {
        const solar_os_agent_tool_descriptor_t *tool = &turn->tools[i];
        if (tool->name == NULL || tool->description == NULL ||
            tool->parameters_json == NULL) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }

        char *name = NULL;
        char *description = NULL;
        err = agent_openai_escape(tool->name, &name, "agent.tool-name");
        if (err == ESP_OK) {
            err = agent_openai_escape(tool->description,
                                      &description,
                                      "agent.tool-description");
        }
        if (err == ESP_OK && api == AGENT_OPENAI_API_RESPONSES) {
            err = agent_openai_append_format(
                tools,
                AGENT_OPENAI_TOOLS_MAX,
                &used,
                "%s{\"type\":\"function\",\"name\":\"%s\","
                "\"description\":\"%s\",\"parameters\":%s,"
                "\"strict\":%s}",
                i == 0 ? "" : ",",
                name,
                description,
                tool->parameters_json,
                tool->strict ? "true" : "false");
        } else if (err == ESP_OK) {
            err = agent_openai_append_format(
                tools,
                AGENT_OPENAI_TOOLS_MAX,
                &used,
                "%s{\"type\":\"function\",\"function\":{"
                "\"name\":\"%s\",\"description\":\"%s\","
                "\"parameters\":%s,\"strict\":%s}}",
                i == 0 ? "" : ",",
                name,
                description,
                tool->parameters_json,
                tool->strict ? "true" : "false");
        }
        solar_os_memory_free(name);
        solar_os_memory_free(description);
    }
    if (err == ESP_OK) {
        err = agent_openai_append_format(tools,
                                         AGENT_OPENAI_TOOLS_MAX,
                                         &used,
                                         "]");
    }
    if (err != ESP_OK) {
        solar_os_memory_free(tools);
        return err;
    }
    *out_tools = tools;
    return ESP_OK;
}

static esp_err_t agent_openai_build_history(
    const solar_os_agent_provider_turn_t *turn,
    char **out_history)
{
    if (turn == NULL || out_history == NULL ||
        (turn->history_count > 0 && turn->history == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_history = solar_os_memory_calloc(1,
                                          AGENT_OPENAI_HISTORY_MAX,
                                          SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                          "agent.history-json");
    if (*out_history == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    for (size_t i = 0; i < turn->history_count; i++) {
        const solar_os_agent_history_message_t *message = &turn->history[i];
        if (message->role == SOLAR_OS_AGENT_MESSAGE_TOOL) {
            continue;
        }
        char *escaped = NULL;
        esp_err_t err = agent_openai_escape(message->text,
                                            &escaped,
                                            "agent.history-message");
        if (err == ESP_OK) {
            err = agent_openai_append_format(
                *out_history,
                AGENT_OPENAI_HISTORY_MAX,
                &used,
                ",{\"role\":\"%s\",\"content\":\"%s\"}",
                message->role == SOLAR_OS_AGENT_MESSAGE_USER ?
                    "user" : "assistant",
                escaped);
        }
        solar_os_memory_free(escaped);
        if (err != ESP_OK) {
            solar_os_memory_free(*out_history);
            *out_history = NULL;
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t agent_openai_build_body(const solar_os_agent_provider_config_t *config,
                                         const solar_os_agent_provider_turn_t *turn,
                                         char **out_body,
                                         size_t *out_len)
{
    if (config == NULL || turn == NULL || out_body == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;
    *out_len = 0;

    char *prompt = NULL;
    char *model = NULL;
    char *call_id = NULL;
    char *tool_name = NULL;
    char *arguments = NULL;
    char *tool_result = NULL;
    char *tool_context = NULL;
    char *previous_response_id = NULL;
    char *reasoning_effort = NULL;
    char *tools = NULL;
    char *history = NULL;
    const agent_openai_api_t api =
        agent_openai_is_responses_endpoint(config->endpoint) ?
            AGENT_OPENAI_API_RESPONSES :
            AGENT_OPENAI_API_CHAT_COMPLETIONS;
    esp_err_t err = agent_openai_escape(turn->prompt, &prompt, "agent.prompt");
    if (err == ESP_OK) {
        err = agent_openai_escape(config->model, &model, "agent.model");
    }
    if (err == ESP_OK && turn->continuation) {
        err = agent_openai_escape(turn->tool_call_id, &call_id, "agent.call-id");
    }
    if (err == ESP_OK && turn->continuation) {
        err = agent_openai_escape(turn->tool_name, &tool_name, "agent.tool-name");
    }
    if (err == ESP_OK && turn->continuation) {
        err = agent_openai_escape(turn->tool_arguments, &arguments, "agent.arguments");
    }
    if (err == ESP_OK && turn->continuation) {
        err = agent_openai_escape(turn->tool_result, &tool_result, "agent.tool-result");
    }
    if (err == ESP_OK && turn->continuation &&
        api == AGENT_OPENAI_API_CHAT_COMPLETIONS) {
        err = agent_openai_escape(
            turn->tool_context != NULL ? turn->tool_context : "",
            &tool_context,
            "agent.tool-context");
    }
    if (err == ESP_OK && api == AGENT_OPENAI_API_RESPONSES &&
        (turn->continuation ||
         (turn->previous_response_id != NULL &&
          turn->previous_response_id[0] != '\0'))) {
        if (turn->previous_response_id == NULL ||
            turn->previous_response_id[0] == '\0') {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            err = agent_openai_escape(turn->previous_response_id,
                                      &previous_response_id,
                                      "agent.response-id");
        }
    }
    if (err == ESP_OK && api == AGENT_OPENAI_API_RESPONSES) {
        const char *effort = config->reasoning_effort[0] != '\0' ?
            config->reasoning_effort : "medium";
        err = agent_openai_escape(effort,
                                  &reasoning_effort,
                                  "agent.reasoning");
    }
    if (err == ESP_OK) {
        err = agent_openai_build_tools(turn, api, &tools);
    }
    if (err == ESP_OK && api == AGENT_OPENAI_API_CHAT_COMPLETIONS) {
        err = agent_openai_build_history(turn, &history);
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    char *body = solar_os_memory_alloc(AGENT_OPENAI_BODY_MAX,
                                        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                        "agent.http-body");
    if (body == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    const char *reasoning_compat =
        agent_openai_is_official_chat_endpoint(config->endpoint) ?
            "\"reasoning_effort\":\"none\"," : "";
    int written;
    if (api == AGENT_OPENAI_API_RESPONSES && !turn->continuation &&
        previous_response_id == NULL) {
        written = snprintf(
            body,
            AGENT_OPENAI_BODY_MAX,
            "{\"model\":\"%s\",\"stream\":true,\"store\":true,"
            "\"instructions\":\"" AGENT_OPENAI_INSTRUCTIONS "\","
            "\"reasoning\":{\"effort\":\"%s\"},"
            "\"input\":[{\"role\":\"user\",\"content\":\"%s\"}],"
            "\"tools\":%s,\"tool_choice\":\"auto\","
            "\"parallel_tool_calls\":false}",
            model,
            reasoning_effort,
            prompt,
            tools);
    } else if (api == AGENT_OPENAI_API_RESPONSES &&
               !turn->continuation) {
        written = snprintf(
            body,
            AGENT_OPENAI_BODY_MAX,
            "{\"model\":\"%s\",\"stream\":true,\"store\":true,"
            "\"instructions\":\"" AGENT_OPENAI_INSTRUCTIONS "\","
            "\"reasoning\":{\"effort\":\"%s\"},"
            "\"previous_response_id\":\"%s\","
            "\"input\":[{\"role\":\"user\",\"content\":\"%s\"}],"
            "\"tools\":%s,\"tool_choice\":\"auto\","
            "\"parallel_tool_calls\":false}",
            model,
            reasoning_effort,
            previous_response_id,
            prompt,
            tools);
    } else if (api == AGENT_OPENAI_API_RESPONSES) {
        written = snprintf(
            body,
            AGENT_OPENAI_BODY_MAX,
            "{\"model\":\"%s\",\"stream\":true,\"store\":true,"
            "\"instructions\":\"" AGENT_OPENAI_INSTRUCTIONS "\","
            "\"reasoning\":{\"effort\":\"%s\"},"
            "\"previous_response_id\":\"%s\","
            "\"input\":[{\"type\":\"function_call_output\","
            "\"call_id\":\"%s\",\"output\":\"%s\"}],"
            "\"tools\":%s,\"tool_choice\":\"auto\","
            "\"parallel_tool_calls\":false}",
            model,
            reasoning_effort,
            previous_response_id,
            call_id,
            tool_result,
            tools);
    } else if (!turn->continuation) {
        written = snprintf(
            body,
            AGENT_OPENAI_BODY_MAX,
            "{\"model\":\"%s\",\"stream\":true,%s"
            "\"stream_options\":{\"include_usage\":true},"
            "\"messages\":["
            "{\"role\":\"system\",\"content\":\"" AGENT_OPENAI_INSTRUCTIONS "\"}"
            "%s,"
            "{\"role\":\"user\",\"content\":\"%s\"}],"
            "\"tools\":%s,\"tool_choice\":\"auto\","
            "\"parallel_tool_calls\":false}",
            model,
            reasoning_compat,
            history,
            prompt,
            tools);
    } else {
        written = snprintf(
            body,
            AGENT_OPENAI_BODY_MAX,
            "{\"model\":\"%s\",\"stream\":true,%s"
            "\"stream_options\":{\"include_usage\":true},"
            "\"messages\":["
            "{\"role\":\"system\",\"content\":\"" AGENT_OPENAI_INSTRUCTIONS "\"}"
            "%s,"
            "{\"role\":\"user\",\"content\":\"%s\"},"
            "{\"role\":\"system\",\"content\":\"Earlier tool results in this "
            "request:\\n%s\"},"
            "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"%s\","
            "\"type\":\"function\",\"function\":{\"name\":\"%s\","
            "\"arguments\":\"%s\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}],"
            "\"tools\":%s,\"tool_choice\":\"auto\","
            "\"parallel_tool_calls\":false}",
            model,
            reasoning_compat,
            history,
            prompt,
            tool_context,
            call_id,
            tool_name,
            arguments,
            call_id,
            tool_result,
            tools);
    }
    if (written < 0 || (size_t)written >= AGENT_OPENAI_BODY_MAX) {
        solar_os_memory_free(body);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    *out_body = body;
    *out_len = (size_t)written;

cleanup:
    solar_os_memory_free(prompt);
    solar_os_memory_free(model);
    solar_os_memory_free(call_id);
    solar_os_memory_free(tool_name);
    solar_os_memory_free(arguments);
    solar_os_memory_free(tool_result);
    solar_os_memory_free(tool_context);
    solar_os_memory_free(previous_response_id);
    solar_os_memory_free(reasoning_effort);
    solar_os_memory_free(tools);
    solar_os_memory_free(history);
    return err;
}

static esp_err_t agent_openai_error_message(agent_openai_stream_t *stream,
                                            int status,
                                            esp_err_t request_error)
{
    char message[SOLAR_OS_AGENT_EVENT_TEXT_MAX];
    if (stream->error_body[0] != '\0') {
        solar_os_json_doc_t *doc = NULL;
        if (solar_os_json_parse_cstr(stream->error_body, &doc) == ESP_OK) {
            if (solar_os_json_get_path_string(solar_os_json_root(doc),
                                              "error.message",
                                              message,
                                              sizeof(message)) != ESP_OK) {
                snprintf(message, sizeof(message), "HTTP %d", status);
            }
            solar_os_json_free(doc);
        } else {
            snprintf(message, sizeof(message), "HTTP %d", status);
        }
    } else if (request_error != ESP_OK) {
        snprintf(message,
                 sizeof(message),
                 "request failed: %s",
                 esp_err_to_name(request_error));
    } else {
        snprintf(message, sizeof(message), "HTTP %d", status);
    }
    return agent_openai_emit(stream, SOLAR_OS_AGENT_EVENT_ERROR, message);
}

static esp_err_t agent_openai_run_turn(const solar_os_agent_provider_config_t *config,
                                       const solar_os_agent_provider_turn_t *turn,
                                       solar_os_agent_event_fn event_handler,
                                       void *user_data,
                                       solar_os_agent_provider_result_t *result)
{
    if (config == NULL || turn == NULL || event_handler == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->http_status = -1;

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = agent_openai_build_body(config, turn, &body, &body_len);
    if (err != ESP_OK) {
        return err;
    }

    agent_openai_stream_t *stream = solar_os_memory_calloc(
        1,
        sizeof(*stream),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "agent.stream");
    if (stream == NULL) {
        solar_os_memory_free(body);
        return ESP_ERR_NO_MEM;
    }
    stream->line = solar_os_memory_alloc(AGENT_OPENAI_SSE_LINE_MAX,
                                         SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "agent.sse-line");
    if (stream->line == NULL) {
        solar_os_memory_free(stream);
        solar_os_memory_free(body);
        return ESP_ERR_NO_MEM;
    }
    stream->event_handler = event_handler;
    stream->user_data = user_data;
    stream->result = result;
    stream->api = agent_openai_is_responses_endpoint(config->endpoint) ?
        AGENT_OPENAI_API_RESPONSES :
        AGENT_OPENAI_API_CHAT_COMPLETIONS;

    char authorization[SOLAR_OS_AGENT_API_KEY_MAX + 8U];
    solar_os_http_header_t headers[3];
    size_t header_count = 0;
    headers[header_count++] = (solar_os_http_header_t){
        .name = "Content-Type",
        .value = "application/json",
    };
    headers[header_count++] = (solar_os_http_header_t){
        .name = "Accept",
        .value = "text/event-stream",
    };
    if (config->api_key[0] != '\0') {
        snprintf(authorization, sizeof(authorization), "Bearer %s", config->api_key);
        headers[header_count++] = (solar_os_http_header_t){
            .name = "Authorization",
            .value = authorization,
        };
    }

    const solar_os_http_request_options_t options = {
        .url = config->endpoint,
        .method = SOLAR_OS_HTTP_METHOD_POST,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_len = body_len,
        .user_agent = "SolarOS-agent/0.1",
        .timeout_ms = AGENT_OPENAI_HTTP_TIMEOUT_MS,
        .deadline_ms = AGENT_OPENAI_DEADLINE_MS,
        .receive_buffer_size = AGENT_OPENAI_RX_BUFFER,
        .transmit_buffer_size = AGENT_OPENAI_TX_BUFFER,
        .event_handler = agent_openai_http_event,
        .user_data = stream,
    };

    solar_os_http_request_t *request = NULL;
    err = solar_os_http_request_create(&options, &request);
    if (err == ESP_OK) {
        solar_os_agent_provider_bind_request(request);
        solar_os_http_response_t response;
        err = solar_os_http_request_perform(request, &response);
        result->http_status = response.status_code;
        result->duration_ms = response.duration_ms;
        result->bytes_received =
            response.bytes_received > UINT32_MAX ? UINT32_MAX : (uint32_t)response.bytes_received;
        solar_os_agent_provider_unbind_request(request);
        (void)solar_os_http_request_destroy(request);
    }

    if (err == ESP_OK && stream->line_len > 0) {
        stream->line[stream->line_len] = '\0';
        err = agent_openai_process_line(stream);
    }
    if (err == ESP_OK &&
        (result->http_status < 200 || result->http_status >= 300)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && stream->parse_error) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && stream->response_error) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK &&
        (!stream->saw_payload || (!stream->saw_done && !stream->saw_finish))) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && stream->too_many_tools) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK && result->tool_call &&
        (result->tool_call_id[0] == '\0' ||
         result->tool_name[0] == '\0' ||
         result->tool_arguments[0] == '\0')) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK &&
        stream->api == AGENT_OPENAI_API_RESPONSES &&
        result->tool_call &&
        result->response_id[0] == '\0') {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK && !stream->response_error &&
        !solar_os_agent_provider_cancel_requested()) {
        (void)agent_openai_error_message(stream, result->http_status, err);
    }

    memset(authorization, 0, sizeof(authorization));
    solar_os_memory_free(stream->line);
    solar_os_memory_free(stream);
    solar_os_memory_free(body);
    return err;
}

static solar_os_agent_provider_resume_mode_t agent_openai_resume_mode(
    const solar_os_agent_provider_config_t *config)
{
    return config != NULL &&
        agent_openai_is_responses_endpoint(config->endpoint) ?
            SOLAR_OS_AGENT_PROVIDER_RESUME_REMOTE_ID :
            SOLAR_OS_AGENT_PROVIDER_RESUME_LOCAL_HISTORY;
}

const solar_os_agent_provider_t solar_os_agent_openai_provider = {
    .name = "openai-compatible",
    .capabilities = SOLAR_OS_AGENT_PROVIDER_CAP_STREAMING |
                    SOLAR_OS_AGENT_PROVIDER_CAP_TOOLS |
                    SOLAR_OS_AGENT_PROVIDER_CAP_USAGE,
    .resume_mode = agent_openai_resume_mode,
    .run_turn = agent_openai_run_turn,
};
