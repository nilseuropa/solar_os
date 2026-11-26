#pragma once

#include "solar_os_native_agent_abi.h"
#include "solar_os_native_agent_run_abi.h"

#define SOLAR_OS_NATIVE_AGENT_TOOLS_SERVICE "agent.tools"
#define SOLAR_OS_NATIVE_AGENT_TOOLS_ABI 1U

#define SOLAR_OS_NATIVE_AGENT_RISK_READ_ONLY 0U
#define SOLAR_OS_NATIVE_AGENT_RISK_SENSITIVE_READ 1U
#define SOLAR_OS_NATIVE_AGENT_RISK_MUTATING 2U
#define SOLAR_OS_NATIVE_AGENT_RISK_DISRUPTIVE 3U
#define SOLAR_OS_NATIVE_AGENT_TOOL_DENY 0U
#define SOLAR_OS_NATIVE_AGENT_TOOL_ALLOW 1U
#define SOLAR_OS_NATIVE_AGENT_TOOL_CONFIRM_ONCE 2U

typedef struct {
    const char *name;
    const char *description;
    const char *domain;
    const char *arguments_json_schema;
    const char *output_json_schema;
    const char *required_capability;
    uint32_t risk;
    bool available, strict;
} solar_os_native_agent_tool_info_v1_t;

typedef struct {
    const char *storage_cwd;
    int (*confirm_tool)(const char *tool_name, const char *risk,
                         const char *arguments, bool *allowed, void *user);
    int (*run_script)(uint32_t language, uint32_t input_type, const char *input,
                       int argc, const char *const *argv, char *output, size_t capacity,
                       solar_os_native_script_result_v1_t *result, void *user);
    uint32_t script_languages;
    void *user;
} solar_os_native_agent_tool_options_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    /* Strings are immutable for the lifetime of the system. Availability is
     * evaluated without request-specific script callbacks. */
    int (*describe)(size_t index, solar_os_native_agent_tool_info_v1_t *info);
    uint32_t (*policy_decision)(uint32_t policy, uint32_t risk);
    const char *(*risk_name)(uint32_t risk);
    /* Execute one registered tool with the configured Agent policy. A tool
     * requiring approval calls confirm_tool synchronously on the caller's
     * worker. Without approval it returns PERMISSION. Arguments and output
     * are JSON. Options are optional and borrowed until the call returns. */
    int (*invoke)(const char *name, const char *arguments,
                  const solar_os_native_agent_tool_options_v1_t *options,
                  char *output, size_t capacity);
} solar_os_native_agent_tools_api_v1_t;
