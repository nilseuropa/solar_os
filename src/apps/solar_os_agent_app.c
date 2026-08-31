#include "solar_os_agent_app.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_agent.h"
#include "solar_os_config.h"
#include "solar_os_identity.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_script_runner.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_wifi.h"
#if SOLAR_OS_PACKAGE_APP_LUA
#include "solar_os_lua.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PYTHON
#include "solar_os_python.h"
#endif

#define AGENT_APP_TASK_STACK 16384U
#define AGENT_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define AGENT_APP_EVENT_QUEUE_LEN 16U
#define AGENT_APP_SCRIPT_OUTPUT_MAX 4096U
#define AGENT_APP_SCRIPT_TIMEOUT_MS 30000U
#define AGENT_APP_CONFIRM_TIMEOUT_MS 30000U

SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(AGENT_APP_TASK_STACK);

typedef enum {
    AGENT_APP_MODE_CHAT,
    AGENT_APP_MODE_ASK,
    AGENT_APP_MODE_SCRIPT_PYTHON,
    AGENT_APP_MODE_SCRIPT_LUA,
} agent_app_mode_t;

typedef struct {
    QueueHandle_t events;
    TaskHandle_t task;
    solar_os_context_t *ctx;
    char *prompt;
    char *script_output;
    agent_app_mode_t mode;
    solar_os_script_input_t script_input;
    int script_arg_start;
    volatile bool task_done;
    volatile bool stopping;
    volatile int confirm_decision;
    volatile bool confirmation_pending;
    bool running;
    bool suspended;
    bool text_started;
    bool text_segment_started;
    bool turn_finished;
    bool turn_success;
    bool script_reported;
    char turn_message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    char input[SOLAR_OS_AGENT_PROMPT_MAX];
    size_t input_len;
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t total_tokens;
    char username[SOLAR_OS_IDENTITY_USER_MAX];
    char conversation_id[SOLAR_OS_AGENT_CONVERSATION_ID_MAX];
} agent_app_state_t;

static const char *TAG = "agent_app";
typedef struct {
    agent_app_state_t app;
    solar_os_script_run_result_t script_result;
    solar_os_shell_io_t fallback_io;
} agent_app_cold_state_t;

static void *agent_app_state;
#define agent_app (((agent_app_cold_state_t *)agent_app_state)->app)
#define agent_script_result \
    (((agent_app_cold_state_t *)agent_app_state)->script_result)
#define agent_fallback_io \
    (((agent_app_cold_state_t *)agent_app_state)->fallback_io)

static solar_os_shell_io_t *agent_app_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&agent_fallback_io,
                                        solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &agent_fallback_io);
        io = &agent_fallback_io;
    }
    return io;
}

static void agent_app_write_prefix(solar_os_shell_io_t *io)
{
    (void)solar_os_shell_io_write_bold(io, "agent:");
    (void)solar_os_shell_io_write(io, " ");
}

static void agent_app_vprintf(solar_os_shell_io_t *io,
                              const char *fmt,
                              va_list args)
{
    agent_app_write_prefix(io);
    (void)solar_os_shell_io_vprintf(io, fmt, args);
}

static void agent_app_printf(solar_os_shell_io_t *io,
                             const char *fmt,
                             ...)
{
    va_list args;
    va_start(args, fmt);
    agent_app_vprintf(io, fmt, args);
    va_end(args);
}

static void agent_app_writeln(solar_os_shell_io_t *io, const char *text)
{
    agent_app_write_prefix(io);
    (void)solar_os_shell_io_writeln(io, text);
}

static void agent_app_newline_printf(solar_os_shell_io_t *io,
                                     const char *fmt,
                                     ...)
{
    (void)solar_os_shell_io_newline(io);
    va_list args;
    va_start(args, fmt);
    agent_app_vprintf(io, fmt, args);
    va_end(args);
}

static void agent_app_update_token_footer(solar_os_context_t *ctx)
{
    char footer[96];
    solar_os_shell_io_t *io = agent_app_io(ctx);
    if (solar_os_shell_io_cols(io) < 30U) {
        snprintf(footer,
                 sizeof(footer),
                 "I%" PRIu32 " O%" PRIu32 " T%" PRIu32,
                 agent_app.prompt_tokens,
                 agent_app.completion_tokens,
                 agent_app.total_tokens);
    } else {
        snprintf(footer,
                 sizeof(footer),
                 "tokens %" PRIu32 " in | %" PRIu32 " out | %" PRIu32 " total",
                 agent_app.prompt_tokens,
                 agent_app.completion_tokens,
                 agent_app.total_tokens);
    }
    (void)solar_os_shell_io_set_footer(io, footer);
}

static void agent_app_return_to_shell(solar_os_context_t *ctx,
                                      int exit_code,
                                      const char *message)
{
    solar_os_context_finish(ctx, exit_code, message);
}

static void agent_app_print_status(solar_os_context_t *ctx)
{
    solar_os_agent_status_t status;
    solar_os_shell_io_t *io = agent_app_io(ctx);
    if (solar_os_agent_get_status(&status) != ESP_OK) {
        agent_app_writeln(io, "status unavailable");
        solar_os_shell_io_flush(io);
        return;
    }

    solar_os_shell_io_printf(io,
                             "Provider: %s\n"
                             "Endpoint: %s\n"
                             "Model: %s\n"
                             "API key: %s\n"
                             "Reasoning (Responses): %s\n"
                             "Tool policy: %s\n"
                             "Max tools/request: %u\n"
                             "State: %s\n",
                             status.provider,
                             status.endpoint[0] != '\0' ?
                                 status.endpoint : "not configured",
                             status.model[0] != '\0' ?
                                 status.model : "not configured",
                             status.api_key_set ? "set" : "not set",
                             status.reasoning_effort,
                             solar_os_agent_tool_policy_name(status.tool_policy),
                             (unsigned int)status.max_tools,
                             status.running ? "running" : "idle");
    solar_os_shell_io_printf(io,
                             "Requests: %" PRIu32 ", failures: %" PRIu32 "\n"
                             "Tools: %" PRIu32 " executed, %" PRIu32
                             " denied, %" PRIu32 " failed\n",
                             status.request_count,
                             status.failure_count,
                             status.tool_executed_count,
                             status.tool_denied_count,
                             status.tool_failed_count);
    if (status.request_count > 0) {
        solar_os_shell_io_printf(
            io,
            "Last request tools: %u/%u used\n",
            (unsigned int)status.last_tool_call_count,
            (unsigned int)status.last_max_tools);
        solar_os_shell_io_printf(
            io,
            "Last: %s, HTTP %d, %" PRIu32 " ms, %" PRIu32 " bytes\n",
            esp_err_to_name(status.last_error),
            status.last_http_status,
            status.last_duration_ms,
            status.last_bytes_received);
        solar_os_shell_io_printf(
            io,
            "Internal: before %" PRIu32 ", low %" PRIu32
            ", request-end %" PRIu32 " bytes\n",
            status.last_internal_before,
            status.last_internal_low,
            status.last_internal_after);
        solar_os_shell_io_printf(
            io,
            "Largest internal: before %" PRIu32
            ", request-end %" PRIu32 " bytes\n",
            status.last_internal_largest_before,
            status.last_internal_largest_after);
        solar_os_shell_io_printf(
            io,
            "PSRAM: before %" PRIu32 ", request-end %" PRIu32 " bytes\n",
            status.last_psram_before,
            status.last_psram_after);
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t agent_app_build_prompt(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 3) {
        return ESP_ERR_INVALID_ARG;
    }
    agent_app.prompt = solar_os_memory_calloc(1,
                                              SOLAR_OS_AGENT_PROMPT_MAX,
                                              SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                              "agent.app.prompt");
    if (agent_app.prompt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    for (int i = 2; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        const size_t len = strlen(arg);
        const size_t separator = used > 0 ? 1U : 0U;
        if (used + separator + len >= SOLAR_OS_AGENT_PROMPT_MAX) {
            solar_os_memory_free(agent_app.prompt);
            agent_app.prompt = NULL;
            return ESP_ERR_INVALID_SIZE;
        }
        if (separator != 0) {
            agent_app.prompt[used++] = ' ';
        }
        memcpy(agent_app.prompt + used, arg, len);
        used += len;
        agent_app.prompt[used] = '\0';
    }
    return used > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t agent_app_build_script(solar_os_context_t *ctx)
{
    if (solar_os_context_argc(ctx) < 4) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *language = solar_os_context_argv(ctx, 2);
    if (strcmp(language, "python") == 0) {
#if SOLAR_OS_PACKAGE_APP_PYTHON
        agent_app.mode = AGENT_APP_MODE_SCRIPT_PYTHON;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else if (strcmp(language, "lua") == 0) {
#if SOLAR_OS_PACKAGE_APP_LUA
        agent_app.mode = AGENT_APP_MODE_SCRIPT_LUA;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const char *input = solar_os_context_argv(ctx, 3);
    esp_err_t err = ESP_OK;
    if (strcmp(input, "-c") == 0) {
        if (solar_os_context_argc(ctx) < 5) {
            return ESP_ERR_INVALID_ARG;
        }
        input = solar_os_context_argv(ctx, 4);
        const size_t input_size = strlen(input) + 1U;
        agent_app.prompt = solar_os_memory_alloc(
            input_size,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "agent.script.source");
        if (agent_app.prompt != NULL) {
            memcpy(agent_app.prompt, input, input_size);
        }
        agent_app.script_input = SOLAR_OS_SCRIPT_INPUT_SOURCE;
        agent_app.script_arg_start = 5;
    } else {
        agent_app.prompt = solar_os_memory_calloc(1,
                                                  SOLAR_OS_STORAGE_PATH_MAX,
                                                  SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                                  "agent.script.path");
        if (agent_app.prompt != NULL) {
            err = solar_os_storage_resolve_path(input,
                                                agent_app.prompt,
                                                SOLAR_OS_STORAGE_PATH_MAX);
        }
        agent_app.script_input = SOLAR_OS_SCRIPT_INPUT_FILE;
        agent_app.script_arg_start = 4;
    }
    if (agent_app.prompt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        return err;
    }

    agent_app.script_output = solar_os_memory_calloc(
        1,
        AGENT_APP_SCRIPT_OUTPUT_MAX,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "agent.script.output");
    return agent_app.script_output != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool agent_app_script_cancel_requested(void *user)
{
    const agent_app_state_t *state = (const agent_app_state_t *)user;
    return state != NULL && state->stopping;
}

static bool agent_app_send_event(const solar_os_agent_event_t *event)
{
    if (event == NULL || agent_app.events == NULL) {
        return false;
    }
    while (!agent_app.task_done && !agent_app.stopping) {
        if (xQueueSend(agent_app.events, event, pdMS_TO_TICKS(100)) == pdPASS) {
            return true;
        }
    }
    return false;
}

static esp_err_t agent_app_confirm_tool(const char *tool_name,
                                        const char *risk,
                                        const char *arguments,
                                        bool *allowed,
                                        void *user_data)
{
    agent_app_state_t *state = (agent_app_state_t *)user_data;
    if (state == NULL || tool_name == NULL || risk == NULL ||
        arguments == NULL || allowed == NULL || state->task == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    state->confirm_decision = -1;
    state->confirmation_pending = true;
    solar_os_agent_event_t event = {
        .type = SOLAR_OS_AGENT_EVENT_TOOL_CONFIRMATION,
    };
    snprintf(event.text,
             sizeof(event.text),
             "confirmation required for %s (%s)\nArguments:\n",
             tool_name,
             risk);
    strlcpy(event.tool_name, tool_name, sizeof(event.tool_name));
    if (!agent_app_send_event(&event)) {
        state->confirmation_pending = false;
        return ESP_ERR_INVALID_STATE;
    }

    const char *remaining = arguments;
    do {
        memset(&event, 0, sizeof(event));
        event.type = SOLAR_OS_AGENT_EVENT_TOOL_CONFIRMATION;
        const size_t length = strlen(remaining);
        const size_t copy = length >= sizeof(event.text) ?
            sizeof(event.text) - 1U : length;
        memcpy(event.text, remaining, copy);
        event.text[copy] = '\0';
        remaining += copy;
        event.success = *remaining == '\0';
        if (!agent_app_send_event(&event)) {
            state->confirmation_pending = false;
            return ESP_ERR_INVALID_STATE;
        }
        if (copy == 0) {
            break;
        }
    } while (*remaining != '\0');

    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(AGENT_APP_CONFIRM_TIMEOUT_MS);
    while (!state->stopping && state->confirm_decision < 0 &&
           xTaskGetTickCount() - started < timeout) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    if (state->stopping) {
        state->confirmation_pending = false;
        return ESP_ERR_INVALID_STATE;
    }
    *allowed = state->confirm_decision > 0;
    state->confirmation_pending = false;
    return ESP_OK;
}

static esp_err_t agent_app_run_script(
    solar_os_agent_script_language_t language,
    solar_os_script_input_t input_type,
    const char *input,
    int argc,
    const char *const *argv,
    char *output,
    size_t output_size,
    solar_os_script_run_result_t *result,
    void *user_data)
{
    agent_app_state_t *state = (agent_app_state_t *)user_data;
    if (state == NULL || input == NULL || argc < 0 ||
        argc > SOLAR_OS_APP_ARG_MAX || (argc > 0 && argv == NULL) ||
        output == NULL || output_size == 0 || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_script_run_request_t request = {
        .context = state->ctx,
        .input_type = input_type,
        .input = input,
        .source_name = input_type == SOLAR_OS_SCRIPT_INPUT_SOURCE
            ? "<agent-tool>"
            : input,
        .argc = argc,
        .argv = argv,
        .timeout_ms = AGENT_APP_SCRIPT_TIMEOUT_MS,
        .cancel_requested = agent_app_script_cancel_requested,
        .cancel_user = state,
        .output = output,
        .output_size = output_size,
    };
#if SOLAR_OS_PACKAGE_APP_PYTHON
    if (language == SOLAR_OS_AGENT_SCRIPT_PYTHON) {
        (void)solar_os_python_run(&request, result);
        return ESP_OK;
    }
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
    if (language == SOLAR_OS_AGENT_SCRIPT_LUA) {
        (void)solar_os_lua_run(&request, result);
        return ESP_OK;
    }
#endif
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t agent_app_service_event(const solar_os_agent_event_t *event,
                                         void *user_data)
{
    (void)user_data;
    return agent_app_send_event(event) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void agent_app_task(void *arg)
{
    (void)arg;
    if (agent_app.mode == AGENT_APP_MODE_CHAT ||
        agent_app.mode == AGENT_APP_MODE_ASK) {
        char storage_cwd[SOLAR_OS_STORAGE_PATH_MAX] = {0};
        (void)solar_os_shell_resolve_path(agent_app.ctx,
                                          ".",
                                          storage_cwd,
                                          sizeof(storage_cwd));
        const solar_os_agent_request_t request = {
            .prompt = agent_app.prompt,
            .conversation_id =
                agent_app.mode == AGENT_APP_MODE_CHAT &&
                        agent_app.conversation_id[0] != '\0' ?
                    agent_app.conversation_id : NULL,
            .storage_cwd =
                storage_cwd[0] != '\0' ? storage_cwd : NULL,
            .next_conversation_id =
                agent_app.mode == AGENT_APP_MODE_CHAT ?
                    agent_app.conversation_id : NULL,
            .next_conversation_id_len =
                agent_app.mode == AGENT_APP_MODE_CHAT ?
                    sizeof(agent_app.conversation_id) : 0U,
            .event_handler = agent_app_service_event,
            .confirm_tool = agent_app_confirm_tool,
            .run_script = agent_app_run_script,
            .script_languages =
#if SOLAR_OS_PACKAGE_APP_PYTHON
                SOLAR_OS_AGENT_SCRIPT_PYTHON |
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
                SOLAR_OS_AGENT_SCRIPT_LUA |
#endif
                0U,
            .user_data = &agent_app,
        };
        (void)solar_os_agent_run(&request);
    } else {
        const char *argv[SOLAR_OS_APP_ARG_MAX] = {
            agent_app.script_input == SOLAR_OS_SCRIPT_INPUT_SOURCE
                ? "<agent>"
                : agent_app.prompt,
        };
        int script_argc = 1;
        const int argc = solar_os_context_argc(agent_app.ctx);
        for (int i = agent_app.script_arg_start;
             i < argc && script_argc < SOLAR_OS_APP_ARG_MAX;
             i++) {
            argv[script_argc++] = solar_os_context_argv(agent_app.ctx, i);
        }
        const solar_os_script_run_request_t request = {
            .context = agent_app.ctx,
            .input_type = agent_app.script_input,
            .input = agent_app.prompt,
            .source_name = agent_app.script_input == SOLAR_OS_SCRIPT_INPUT_SOURCE
                ? "<agent>"
                : agent_app.prompt,
            .argc = script_argc,
            .argv = argv,
            .timeout_ms = AGENT_APP_SCRIPT_TIMEOUT_MS,
            .cancel_requested = agent_app_script_cancel_requested,
            .cancel_user = &agent_app,
            .output = agent_app.script_output,
            .output_size = AGENT_APP_SCRIPT_OUTPUT_MAX,
        };
#if SOLAR_OS_PACKAGE_APP_PYTHON
        if (agent_app.mode == AGENT_APP_MODE_SCRIPT_PYTHON) {
            (void)solar_os_python_run(&request, &agent_script_result);
        }
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
        if (agent_app.mode == AGENT_APP_MODE_SCRIPT_LUA) {
            (void)solar_os_lua_run(&request, &agent_script_result);
        }
#endif
    }
    agent_app.task_done = true;
    solar_os_task_delete_internal(NULL);
}

static void agent_app_cleanup_turn(void)
{
    if (agent_app.prompt != NULL) {
        solar_os_memory_free(agent_app.prompt);
        agent_app.prompt = NULL;
    }
    if (agent_app.script_output != NULL) {
        solar_os_memory_free(agent_app.script_output);
        agent_app.script_output = NULL;
    }
    agent_app.task = NULL;
    agent_app.running = false;
    agent_app.task_done = false;
    agent_app.stopping = false;
    agent_app.confirm_decision = 0;
    agent_app.confirmation_pending = false;
    agent_app.text_started = false;
    agent_app.text_segment_started = false;
    agent_app.turn_finished = false;
}

static void agent_app_cleanup(void)
{
    if (agent_app.ctx != NULL) {
        (void)solar_os_shell_io_clear_footer(agent_app_io(agent_app.ctx));
    }
    agent_app_cleanup_turn();
    if (agent_app.events != NULL) {
        solar_os_queue_delete(agent_app.events);
        agent_app.events = NULL;
    }
}

static void agent_app_state_release_cleanup(void)
{
    agent_app_cleanup_turn();
    if (agent_app.events != NULL) {
        solar_os_queue_delete(agent_app.events);
        agent_app.events = NULL;
    }
}

static void agent_app_print_delta(solar_os_shell_io_t *io,
                                  const char *text)
{
    if (text == NULL) {
        return;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        solar_os_shell_io_put_utf8_byte(io, *p);
    }
}

static esp_err_t agent_app_print_saved_message(
    solar_os_agent_message_role_t role,
    const char *text,
    size_t text_len,
    void *user_data)
{
    solar_os_context_t *ctx = user_data;
    if (ctx == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_shell_io_t *io = agent_app_io(ctx);
    if (role == SOLAR_OS_AGENT_MESSAGE_USER) {
        char prompt[SOLAR_OS_IDENTITY_USER_MAX + 3U];
        snprintf(prompt,
                 sizeof(prompt),
                 "%s: ",
                 agent_app.username[0] != '\0' ?
                    agent_app.username : SOLAR_OS_IDENTITY_DEFAULT_USER);
        solar_os_shell_io_write_bold(io, prompt);
    } else {
        agent_app_write_prefix(io);
        if (role == SOLAR_OS_AGENT_MESSAGE_TOOL) {
            solar_os_shell_io_write(io, "tools: ");
        }
    }
    for (size_t i = 0; i < text_len; i++) {
        solar_os_shell_io_put_utf8_byte(io, (uint8_t)text[i]);
    }
    solar_os_shell_io_newline(io);
    return ESP_OK;
}

static void agent_app_report_script(solar_os_context_t *ctx)
{
    if (agent_app.mode == AGENT_APP_MODE_CHAT ||
        agent_app.mode == AGENT_APP_MODE_ASK || !agent_app.task_done ||
        agent_app.script_reported) {
        return;
    }

    solar_os_shell_io_t *io = agent_app_io(ctx);
    if (agent_script_result.output_len > 0) {
        agent_app_print_delta(io, agent_app.script_output);
        if (agent_app.script_output[agent_script_result.output_len - 1] != '\n') {
            solar_os_shell_io_newline(io);
        }
    }
    if (agent_script_result.output_truncated) {
        agent_app_writeln(io, "script output truncated");
    }
    if (agent_script_result.success) {
        agent_app_writeln(io, "script complete");
    } else {
        agent_app_printf(
            io,
            "script failed: %s%s%s\n",
            esp_err_to_name(agent_script_result.status),
            agent_script_result.error[0] != '\0' ? ", " : "",
            agent_script_result.error);
    }
    solar_os_shell_io_flush(io);
    agent_app.script_reported = true;
    agent_app.running = false;
    char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    if (agent_script_result.success) {
        strlcpy(message, "agent: script complete", sizeof(message));
    } else {
        snprintf(message,
                 sizeof(message),
                 "agent: script failed: %s%s%s",
                 esp_err_to_name(agent_script_result.status),
                 agent_script_result.error[0] != '\0' ? ", " : "",
                 agent_script_result.error);
    }
    agent_app_return_to_shell(
        ctx, agent_script_result.success ? 0 : 1, message);
}

static void agent_app_drain_events(solar_os_context_t *ctx)
{
    if (agent_app.events == NULL) {
        return;
    }
    solar_os_shell_io_t *io = agent_app_io(ctx);
    solar_os_agent_event_t event;
    while (xQueueReceive(agent_app.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case SOLAR_OS_AGENT_EVENT_STATUS:
            agent_app_printf(io, "%s\n", event.text);
            break;
        case SOLAR_OS_AGENT_EVENT_TEXT_DELTA:
            if (!agent_app.text_segment_started) {
                agent_app_write_prefix(io);
            }
            agent_app.text_started = true;
            agent_app.text_segment_started = true;
            agent_app_print_delta(io, event.text);
            break;
        case SOLAR_OS_AGENT_EVENT_TOOL_CALL:
            agent_app_newline_printf(io, "tool %s\n", event.tool_name);
            agent_app.text_segment_started = false;
            break;
        case SOLAR_OS_AGENT_EVENT_TOOL_RESULT: {
            const char *outcome = event.success ? "complete" :
                (strstr(event.text, "\"error\":\"tool denied\"") != NULL ?
                    "denied" :
                    (strstr(event.text, "\"error\":\"tool activated\"") != NULL ?
                        "activated" :
                        (strstr(event.text,
                                "\"error\":\"duplicate tool call\"") != NULL ?
                            "skipped" : "failed")));
            const char *error = strstr(event.text, "\"error\":\"");
            if (!event.success && strcmp(outcome, "failed") == 0 &&
                error != NULL) {
                error += strlen("\"error\":\"");
                const char *error_end = strchr(error, '"');
                const int error_len = error_end != NULL ?
                    (int)(error_end - error) : (int)strlen(error);
                agent_app_printf(io,
                                 "tool %s failed: %.*s\n",
                                 event.tool_name,
                                 error_len,
                                 error);
            } else {
                agent_app_printf(io,
                                 "tool %s %s\n",
                                 event.tool_name,
                                 outcome);
            }
            agent_app.text_segment_started = false;
            break;
        }
        case SOLAR_OS_AGENT_EVENT_TOOL_CONFIRMATION:
            if (event.tool_name[0] != '\0') {
                solar_os_shell_io_newline(io);
                agent_app_write_prefix(io);
            }
            agent_app_print_delta(io, event.text);
            if (event.success) {
                solar_os_shell_io_write(io, "\nAllow once? [y/N] ");
            }
            break;
        case SOLAR_OS_AGENT_EVENT_USAGE:
            agent_app.prompt_tokens = event.prompt_tokens;
            agent_app.completion_tokens = event.completion_tokens;
            agent_app.total_tokens = event.total_tokens;
            agent_app_update_token_footer(ctx);
            break;
        case SOLAR_OS_AGENT_EVENT_ERROR:
            agent_app_newline_printf(io, "%s\n", event.text);
            break;
        case SOLAR_OS_AGENT_EVENT_DONE:
            if (agent_app.text_started) {
                solar_os_shell_io_newline(io);
            }
            if (!event.success) {
                agent_app_printf(io, "%s\n", event.text);
                agent_app_print_status(ctx);
            }
            agent_app.running = false;
            agent_app.turn_finished = true;
            agent_app.turn_success = event.success;
            strlcpy(agent_app.turn_message,
                    event.text,
                    sizeof(agent_app.turn_message));
            break;
        default:
            break;
        }
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t agent_app_start_worker(void)
{
    agent_app.task_done = false;
    agent_app.stopping = false;
    agent_app.text_started = false;
    agent_app.text_segment_started = false;
    agent_app.turn_finished = false;
    agent_app.confirm_decision = 0;
    agent_app.confirmation_pending = false;
    agent_app.running = true;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        agent_app_task,
        "solar_os_agent",
        AGENT_APP_TASK_STACK,
        NULL,
        AGENT_APP_TASK_PRIORITY,
        &agent_app.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        agent_app.task = NULL;
        agent_app.running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void agent_app_chat_prompt(solar_os_context_t *ctx)
{
    char prompt[SOLAR_OS_IDENTITY_USER_MAX + 3U];
    snprintf(prompt,
             sizeof(prompt),
             "%s> ",
             agent_app.username[0] != '\0' ?
                 agent_app.username : SOLAR_OS_IDENTITY_DEFAULT_USER);
    solar_os_shell_io_write_bold(agent_app_io(ctx), prompt);
    solar_os_shell_io_flush(agent_app_io(ctx));
}

static void agent_app_finish_turn(solar_os_context_t *ctx)
{
    if ((agent_app.mode != AGENT_APP_MODE_CHAT &&
         agent_app.mode != AGENT_APP_MODE_ASK) ||
        !agent_app.task_done || !agent_app.turn_finished) {
        return;
    }
    const bool chat_mode = agent_app.mode == AGENT_APP_MODE_CHAT;
    const bool turn_success = agent_app.turn_success;
    char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
    strlcpy(message, agent_app.turn_message, sizeof(message));
    agent_app_cleanup_turn();
    if (chat_mode) {
        agent_app_chat_prompt(ctx);
    } else {
        agent_app_return_to_shell(
            ctx,
            turn_success ? 0 : 1,
            turn_success ? NULL :
                (message[0] != '\0' ? message : "agent: request failed"));
    }
}

static esp_err_t agent_app_submit_chat(solar_os_context_t *ctx)
{
    if (agent_app.input_len == 0 || agent_app.running ||
        agent_app.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t size = agent_app.input_len + 1U;
    agent_app.prompt = solar_os_memory_alloc(
        size,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "agent.app.prompt");
    if (agent_app.prompt == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(agent_app.prompt, agent_app.input, size);
    memset(agent_app.input, 0, sizeof(agent_app.input));
    agent_app.input_len = 0;
    solar_os_shell_io_newline(agent_app_io(ctx));

    const esp_err_t err = agent_app_start_worker();
    if (err != ESP_OK) {
        solar_os_memory_free(agent_app.prompt);
        agent_app.prompt = NULL;
    }
    return err;
}

static esp_err_t agent_app_start(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    const bool new_mode = argc == 2 &&
        strcmp(solar_os_context_argv(ctx, 1), "new") == 0;
    const bool resume_mode = argc == 3 &&
        strcmp(solar_os_context_argv(ctx, 1), "resume") == 0;
    const bool chat_mode = argc == 1 || new_mode || resume_mode;
    const bool ask_mode = argc >= 3 &&
        strcmp(solar_os_context_argv(ctx, 1), "ask") == 0;
    const bool script_mode = argc >= 4 &&
        strcmp(solar_os_context_argv(ctx, 1), "script") == 0;
    if (ask_mode || script_mode || !chat_mode) {
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
    }
    if (agent_app.task != NULL && !agent_app.task_done) {
        agent_app_writeln(agent_app_io(ctx),
                          "previous request is still stopping");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_return_to_shell(
            ctx,
            1,
            solar_os_context_app_class(ctx) == SOLAR_OS_APP_CLASS_COMMAND ?
                NULL : "agent: previous request is still stopping");
        return ESP_OK;
    }
    agent_app_cleanup();
    memset(&agent_app, 0, sizeof(agent_app));
    memset(&agent_script_result, 0, sizeof(agent_script_result));
    agent_app.ctx = ctx;
    solar_os_identity_get_user(agent_app.username,
                               sizeof(agent_app.username));
    (void)solar_os_agent_init();

    if (!chat_mode && !ask_mode && !script_mode) {
        agent_app_writeln(agent_app_io(ctx),
                          "launch with agent, new, resume, ask, or script");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_return_to_shell(
            ctx, 2, "usage: agent [new|resume <id>|ask <prompt...>|script <file> [args...]]");
        return ESP_OK;
    }

    solar_os_agent_status_t status = {0};
    esp_err_t err;
    if (chat_mode || ask_mode) {
        agent_app.mode =
            chat_mode ? AGENT_APP_MODE_CHAT : AGENT_APP_MODE_ASK;
        solar_os_wifi_status_t wifi;
        solar_os_wifi_get_status(&wifi);
        if (!wifi.started || !wifi.connected || !wifi.has_ip) {
            agent_app_writeln(agent_app_io(ctx),
                              "Wi-Fi is not connected");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app_return_to_shell(ctx, 1, "agent: Wi-Fi is not connected");
            return ESP_OK;
        }
        (void)solar_os_agent_get_status(&status);
        if (!status.configured) {
            agent_app_writeln(agent_app_io(ctx),
                              "configure endpoint and model first");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app_return_to_shell(
                ctx, 1, "agent: configure endpoint and model first");
            return ESP_OK;
        }
        if (resume_mode) {
            const char *id = solar_os_context_argv(ctx, 2);
            solar_os_agent_conversation_info_t info;
            err = solar_os_agent_conversation_get(id, &info);
            if (err != ESP_OK) {
                agent_app_printf(agent_app_io(ctx),
                                 "conversation not found: %s\n",
                                 id);
                solar_os_shell_io_flush(agent_app_io(ctx));
                char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
                snprintf(message,
                         sizeof(message),
                         "agent: conversation not found: %s",
                         id);
                agent_app_return_to_shell(ctx, 1, message);
                return ESP_OK;
            }
            strlcpy(agent_app.conversation_id,
                    info.id,
                    sizeof(agent_app.conversation_id));
        }
        err = chat_mode ? ESP_OK : agent_app_build_prompt(ctx);
    } else {
        err = agent_app_build_script(ctx);
    }
    if (err != ESP_OK) {
        agent_app_printf(agent_app_io(ctx),
                         "invalid request: %s\n",
                         esp_err_to_name(err));
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_cleanup();
        char message[SOLAR_OS_CONTEXT_STATUS_MESSAGE_MAX];
        snprintf(message,
                 sizeof(message),
                 "agent: invalid request: %s",
                 esp_err_to_name(err));
        agent_app_return_to_shell(ctx, 2, message);
        return ESP_OK;
    }
    if (chat_mode || ask_mode) {
        agent_app.events = solar_os_queue_create(AGENT_APP_EVENT_QUEUE_LEN,
                                                  sizeof(solar_os_agent_event_t));
        if (agent_app.events == NULL) {
            agent_app_writeln(agent_app_io(ctx), "out of memory");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app_cleanup();
            agent_app_return_to_shell(ctx, 1, "agent: out of memory");
            return ESP_OK;
        }
    }

    if (chat_mode || ask_mode) {
        agent_app_update_token_footer(ctx);
        solar_os_shell_io_printf_bold(agent_app_io(ctx),
                                      "agent (%s)\n",
                                      status.model);
    } else {
        solar_os_shell_io_printf_bold(
            agent_app_io(ctx),
            "agent script (%s)\n",
            agent_app.mode == AGENT_APP_MODE_SCRIPT_PYTHON ? "python" : "lua");
    }
    solar_os_shell_io_flush(agent_app_io(ctx));

    if (chat_mode) {
        solar_os_shell_io_t *io = agent_app_io(ctx);
        solar_os_shell_io_printf(
            io,
            "Enter sends. Page Up/Down scrolls. Esc or %s exits.\n",
            solar_os_shell_io_app_exit_key(io));
        if (resume_mode) {
            solar_os_agent_conversation_info_t info;
            if (solar_os_agent_conversation_get(agent_app.conversation_id,
                                                &info) == ESP_OK) {
                solar_os_shell_io_printf(agent_app_io(ctx),
                                         "Resumed %s — %s\n\n",
                                         info.id,
                                         info.title);
            }
            err = solar_os_agent_conversation_visit(
                agent_app.conversation_id,
                agent_app_print_saved_message,
                ctx);
            if (err != ESP_OK) {
                agent_app_printf(agent_app_io(ctx),
                                 "could not restore transcript: %s\n",
                                 esp_err_to_name(err));
            }
        }
        agent_app_chat_prompt(ctx);
        return ESP_OK;
    }

    err = agent_app_start_worker();
    if (err != ESP_OK) {
        agent_app_writeln(agent_app_io(ctx),
                          "task create failed");
        solar_os_shell_io_flush(agent_app_io(ctx));
        agent_app_cleanup();
        agent_app_return_to_shell(ctx, 1, "agent: task create failed");
    }
    return ESP_OK;
}

static void agent_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (agent_app.running && !agent_app.task_done) {
        agent_app.stopping = true;
        if (agent_app.mode == AGENT_APP_MODE_CHAT ||
            agent_app.mode == AGENT_APP_MODE_ASK) {
            (void)solar_os_agent_cancel();
        }
    }
    if (!solar_os_task_wait_done(agent_app.task,
                                 &agent_app.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "agent task did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
        return;
    }
    agent_app_cleanup();
}

static bool agent_app_state_release_ready(void)
{
    return agent_app.task == NULL || agent_app.task_done;
}

static void agent_app_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    agent_app.suspended = true;
}

static void agent_app_resume(solar_os_context_t *ctx)
{
    agent_app.suspended = false;
    if (agent_app.mode == AGENT_APP_MODE_CHAT ||
        agent_app.mode == AGENT_APP_MODE_ASK) {
        agent_app_drain_events(ctx);
        agent_app_finish_turn(ctx);
    } else {
        agent_app_report_script(ctx);
    }
}

static bool agent_app_event(solar_os_context_t *ctx,
                            const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (agent_app.suspended) {
            return true;
        }
        if (agent_app.mode == AGENT_APP_MODE_CHAT ||
            agent_app.mode == AGENT_APP_MODE_ASK) {
            agent_app_drain_events(ctx);
            agent_app_finish_turn(ctx);
        } else {
            agent_app_report_script(ctx);
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    const bool exit_key =
        ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE;
    if (agent_app.confirmation_pending && !exit_key) {
        if (ch == 'y' || ch == 'Y') {
            agent_app.confirm_decision = 1;
            agent_app.confirmation_pending = false;
            solar_os_shell_io_writeln(agent_app_io(ctx), "yes");
            solar_os_shell_io_flush(agent_app_io(ctx));
            if (agent_app.task != NULL) {
                xTaskNotifyGive(agent_app.task);
            }
        } else if (ch == 'n' || ch == 'N' || ch == '\r' || ch == '\n') {
            agent_app.confirm_decision = 0;
            agent_app.confirmation_pending = false;
            solar_os_shell_io_writeln(agent_app_io(ctx), "no");
            solar_os_shell_io_flush(agent_app_io(ctx));
            if (agent_app.task != NULL) {
                xTaskNotifyGive(agent_app.task);
            }
        }
        return true;
    }
    if (exit_key) {
        if (agent_app.running) {
            solar_os_shell_io_newline(agent_app_io(ctx));
            agent_app_writeln(agent_app_io(ctx), "cancelling");
            solar_os_shell_io_flush(agent_app_io(ctx));
            agent_app.stopping = true;
            if (agent_app.mode == AGENT_APP_MODE_CHAT ||
                agent_app.mode == AGENT_APP_MODE_ASK) {
                (void)solar_os_agent_cancel();
            }
            if (agent_app.task != NULL) {
                xTaskNotifyGive(agent_app.task);
            }
        }
        agent_app_return_to_shell(
            ctx,
            agent_app.running ? 130 : 0,
            agent_app.running ? "agent: cancelled" : NULL);
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_UP) {
        solar_os_terminal_t *terminal =
            solar_os_shell_io_terminal(agent_app_io(ctx));
        if (terminal != NULL) {
            solar_os_terminal_page_up(terminal);
        }
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *terminal =
            solar_os_shell_io_terminal(agent_app_io(ctx));
        if (terminal != NULL) {
            solar_os_terminal_page_down(terminal);
        }
        return true;
    }
    if (agent_app.mode == AGENT_APP_MODE_CHAT && !agent_app.running) {
        solar_os_shell_io_t *io = agent_app_io(ctx);
        if (ch == '\r' || ch == '\n') {
            if (agent_app.input_len > 0) {
                const esp_err_t err = agent_app_submit_chat(ctx);
                if (err != ESP_OK) {
                    agent_app_newline_printf(io,
                                             "request failed to start: %s\n",
                                             esp_err_to_name(err));
                    agent_app_chat_prompt(ctx);
                }
            }
            solar_os_shell_io_flush(io);
            return true;
        }
        if (ch == '\b' || ch == 0x7fU) {
            if (agent_app.input_len > 0) {
                agent_app.input[--agent_app.input_len] = '\0';
                solar_os_shell_io_write(io, "\b \b");
                solar_os_shell_io_flush(io);
            }
            return true;
        }
        if (ch >= 0x20U && ch <= 0x7eU &&
            agent_app.input_len + 1U < sizeof(agent_app.input)) {
            agent_app.input[agent_app.input_len++] = (char)ch;
            agent_app.input[agent_app.input_len] = '\0';
            solar_os_shell_io_put_char(io, (char)ch);
            solar_os_shell_io_flush(io);
        }
        return true;
    }
    return true;
}

const solar_os_app_t solar_os_agent_app = {
    .name = "agent",
    .summary = "native LLM agent",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = agent_app_start,
    .suspend = agent_app_suspend,
    .resume = agent_app_resume,
    .stop = agent_app_stop,
    .event = agent_app_event,
    .state_slot = &agent_app_state,
    .state_size = sizeof(agent_app_cold_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_ready = agent_app_state_release_ready,
    .state_release_cleanup = agent_app_state_release_cleanup,
    .worker_stack_bytes = AGENT_APP_TASK_STACK,
};
