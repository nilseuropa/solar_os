#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_agent.h"
#include "solar_os_agent_app.h"
#include "solar_os_agent_tools.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static const char * const agent_commands[] = {
    "--tts", "help", "new", "list", "resume", "delete", "status", "tools",
    "config", "forget", "ask", "script",
};
static const char * const agent_config_fields[] = {
    "endpoint", "model", "key", "reasoning", "tools", "max-tools",
};

static void agent_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_write_bold(io, "agent");
    solar_os_shell_io_newline(io);
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  agent");
    solar_os_shell_io_writeln(io, "  agent --tts");
    solar_os_shell_io_writeln(io, "  agent new");
    solar_os_shell_io_writeln(io, "  agent --tts new");
    solar_os_shell_io_writeln(io, "  agent list");
    solar_os_shell_io_writeln(io, "  agent resume SLOT");
    solar_os_shell_io_writeln(io, "  agent --tts resume SLOT");
    solar_os_shell_io_writeln(io, "  agent delete SLOT");
    solar_os_shell_io_writeln(io, "  agent help");
    solar_os_shell_io_writeln(io, "  agent status");
    solar_os_shell_io_writeln(io, "  agent tools");
    solar_os_shell_io_writeln(io, "  agent config endpoint URL");
    solar_os_shell_io_writeln(io, "  agent config model MODEL");
    solar_os_shell_io_writeln(io, "  agent config key KEY|clear");
    solar_os_shell_io_writeln(
        io,
        "  agent config reasoning none|minimal|low|medium|high|xhigh|max");
    solar_os_shell_io_writeln(
        io,
        "  agent config tools off|readonly|confirm|all");
    solar_os_shell_io_writeln(io, "  agent config max-tools 1..32");
    solar_os_shell_io_writeln(io, "  agent forget");
    solar_os_shell_io_writeln(io, "  agent ask PROMPT...");
    solar_os_shell_io_writeln(io, "  agent --tts ask PROMPT...");
    solar_os_shell_io_writeln(
        io,
        "  agent script python|lua (-c SOURCE | FILE) [ARGS...]");
}

static void agent_conversations(solar_os_shell_io_t *io)
{
    solar_os_agent_conversation_info_t
        items[SOLAR_OS_AGENT_CONVERSATION_LIST_MAX];
    size_t count = 0;
    const esp_err_t err = solar_os_agent_conversations_list(
        items,
        SOLAR_OS_AGENT_CONVERSATION_LIST_MAX,
        &count);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "agent: list failed: %s\n",
                                 solar_os_shell_error_text(err));
        return;
    }
    if (count == 0) {
        solar_os_shell_io_writeln(io, "No saved conversations.");
        return;
    }
    size_t cols = solar_os_shell_io_cols(io);
    if (cols == 0U) {
        cols = 80U;
    }
    bool slots_only = true;
    for (size_t i = 0; i < count; i++) {
        if (items[i].id[0] < '1' || items[i].id[0] > '8' ||
            items[i].id[1] != '\0') {
            slots_only = false;
            break;
        }
    }
    if (cols < 72U || !slots_only) {
        for (size_t i = 0; i < count; i++) {
            const int value_width = cols > 9U ? (int)(cols - 9U) : 1;
            solar_os_shell_io_printf(io,
                                     "%s  %u turn%s\n"
                                     "  model: %.*s\n"
                                     "  title: %.*s\n",
                                     items[i].id,
                                     (unsigned int)items[i].turn_count,
                                     items[i].turn_count == 1U ? "" : "s",
                                     value_width,
                                     items[i].model,
                                     value_width,
                                     items[i].title);
        }
        return;
    }
    solar_os_shell_io_writeln(io,
                              "SLOT TURNS  MODEL             TITLE");
    const int title_width = (int)(cols - 30U);
    for (size_t i = 0; i < count; i++) {
        solar_os_shell_io_printf(io,
                                 "%-4s %-6u %-17.17s %.*s\n",
                                 items[i].id,
                                 (unsigned int)items[i].turn_count,
                                 items[i].model,
                                 title_width,
                                 items[i].title);
    }
}

static void agent_launch(solar_os_context_t *ctx,
                         solar_os_shell_io_t *io,
                         int argc,
                         char **argv)
{
    const esp_err_t err =
        solar_os_context_request_launch(ctx, &solar_os_agent_app, argc, argv);
    if (err == ESP_OK) {
        solar_os_shell_session_prepare_foreground_launch(ctx, false);
    } else {
        solar_os_shell_io_printf(io,
                                 "agent: launch failed: %s\n",
                                 solar_os_shell_error_text(err));
    }
}

static void agent_tools(solar_os_shell_io_t *io)
{
    solar_os_agent_status_t status = {0};
    (void)solar_os_agent_get_status(&status);
    solar_os_shell_io_printf(
        io,
        "Policy: %s\n",
        solar_os_agent_tool_policy_name(status.tool_policy));
    solar_os_shell_io_writeln(
        io,
        "Name               Domain    Risk            Available  Policy   Requirement");
    const size_t count = solar_os_agent_tools_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_agent_tool_info_t tool;
        if (!solar_os_agent_tools_get(i, &tool)) {
            continue;
        }
        const solar_os_agent_tool_policy_decision_t decision =
            solar_os_agent_tools_policy_decision(status.tool_policy, tool.risk);
        const char *policy = decision == SOLAR_OS_AGENT_TOOL_POLICY_ALLOW ?
            "allow" :
            (decision == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE ?
                "confirm" : "deny");
        solar_os_shell_io_printf(
            io,
            "%-18s %-9s %-15s %-10s %-8s %s\n",
            tool.provider.name,
            tool.domain,
            solar_os_agent_tool_risk_name(tool.risk),
            tool.available ? "yes" : "no",
            policy,
            tool.required_capability != NULL ?
                tool.required_capability : "-");
    }
}

static void agent_status(solar_os_shell_io_t *io)
{
    solar_os_agent_status_t status;
    if (solar_os_agent_get_status(&status) != ESP_OK) {
        solar_os_shell_io_writeln(io, "agent: status unavailable");
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
    if (status.request_count == 0) {
        return;
    }

    solar_os_shell_io_printf(
        io,
        "Last request tools: %u/%u used\n",
        (unsigned int)status.last_tool_call_count,
        (unsigned int)status.last_max_tools);
    solar_os_shell_io_printf(
        io,
        "Last: %s, HTTP %d, %" PRIu32 " ms, %" PRIu32 " bytes\n",
        solar_os_shell_error_text(status.last_error),
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

static void agent_configure(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc != 4) {
        solar_os_shell_diag_problem(io, "agent config",
                                    argc < 4 ? "missing field or value" : "too many arguments",
                                    "agent config <endpoint|model|key|reasoning|tools|max-tools> <value>",
                                    NULL);
        return;
    }

    const char *field = argv[2];
    const char *value = argv[3];
    esp_err_t err;
    if (strcmp(field, "endpoint") == 0) {
        err = solar_os_agent_set_endpoint(value);
    } else if (strcmp(field, "model") == 0) {
        err = solar_os_agent_set_model(value);
    } else if (strcmp(field, "key") == 0) {
        err = solar_os_agent_set_api_key(strcmp(value, "clear") == 0 ? "" : value);
    } else if (strcmp(field, "reasoning") == 0) {
        err = solar_os_agent_set_reasoning_effort(value);
    } else if (strcmp(field, "tools") == 0) {
        solar_os_agent_tool_policy_t policy;
        err = solar_os_agent_parse_tool_policy(value, &policy);
        if (err == ESP_OK) {
            err = solar_os_agent_set_tool_policy(policy);
        }
    } else if (strcmp(field, "max-tools") == 0) {
        uint8_t max_tools = 0;
        if (!solar_os_shell_parse_u8(value, &max_tools)) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = solar_os_agent_set_max_tools(max_tools);
        }
    } else {
        solar_os_shell_diag_unknown(
            io,
            "agent config",
            "field",
            field,
            solar_os_shell_suggest(field,
                                   agent_config_fields,
                                   sizeof(agent_config_fields) / sizeof(agent_config_fields[0])),
            "agent config endpoint|model|key|reasoning|tools|max-tools <value>");
        return;
    }

    if (err == ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "agent: %s %s\n",
                                 field,
                                 strcmp(field, "key") == 0 ?
                                     (strcmp(value, "clear") == 0 ? "cleared" : "set") :
                                     "updated");
    } else {
        solar_os_shell_io_printf(io,
                                 "agent: invalid %s: %s\n",
                                 field,
                                 solar_os_shell_error_text(err));
    }
}

void solar_os_shell_cmd_agent(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }

    if (argc >= 2 && strcmp(argv[1], "--tts") == 0) {
        const bool launch = argc == 2 ||
            (argc == 3 && strcmp(argv[2], "new") == 0) ||
            (argc == 4 && strcmp(argv[2], "resume") == 0) ||
            (argc >= 4 && strcmp(argv[2], "ask") == 0);
        if (launch) {
            agent_launch(ctx, io, argc, argv);
        } else if (argc == 3 && strcmp(argv[2], "help") == 0) {
            agent_usage(io);
        } else {
            solar_os_shell_diag_problem(
                io,
                "agent --tts",
                "unsupported command",
                "agent --tts [new|resume SLOT|ask PROMPT...]",
                NULL);
        }
        return;
    }

    if (argc == 1) {
        agent_launch(ctx, io, argc, argv);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "new") == 0) {
        agent_launch(ctx, io, argc, argv);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        agent_conversations(io);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "resume") == 0) {
        agent_launch(ctx, io, argc, argv);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "delete") == 0) {
        const esp_err_t err = solar_os_agent_conversation_delete(argv[2]);
        if (err == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "agent: deleted %s\n",
                                     argv[2]);
        } else {
            solar_os_shell_io_printf(io,
                                     "agent: delete failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "help") == 0) {
        agent_usage(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        agent_status(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "tools") == 0) {
        agent_tools(io);
        return;
    }
    if (strcmp(argv[1], "config") == 0) {
        agent_configure(io, argc, argv);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        const esp_err_t err = solar_os_agent_forget();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "agent: configuration erased");
        } else {
            solar_os_shell_io_printf(io,
                                     "agent: erase failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }
    if (strcmp(argv[1], "ask") == 0 && argc >= 3) {
        agent_launch(ctx, io, argc, argv);
        return;
    }
    if (strcmp(argv[1], "script") == 0 && argc >= 4) {
        agent_launch(ctx, io, argc, argv);
        return;
    }
    solar_os_shell_diag_subcommand(io,
                                   "agent",
                                   argc,
                                   argv,
                                   "agent [--tts]|help|new|list|resume|delete|status|tools|config|forget|ask|script",
                                   agent_commands,
                                   sizeof(agent_commands) / sizeof(agent_commands[0]));
}
