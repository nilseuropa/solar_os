#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "solar_os_agent_provider.h"

#define SOLAR_OS_AGENT_TOOL_REGISTRY_MAX 21U
#define SOLAR_OS_AGENT_TOOL_ACTIVE_MAX 8U

typedef enum {
    SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY = 0,
    SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ,
    SOLAR_OS_AGENT_TOOL_RISK_MUTATING,
    SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE,
} solar_os_agent_tool_risk_t;

typedef struct {
    solar_os_agent_tool_descriptor_t provider;
    const char *domain;
    const char *output_schema_json;
    const char *required_capability;
    solar_os_agent_tool_risk_t risk;
    bool available;
} solar_os_agent_tool_info_t;

typedef enum {
    SOLAR_OS_AGENT_TOOL_POLICY_DENY = 0,
    SOLAR_OS_AGENT_TOOL_POLICY_ALLOW,
    SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE,
} solar_os_agent_tool_policy_decision_t;

size_t solar_os_agent_tools_collect(
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity);
size_t solar_os_agent_tools_collect_discovered(
    const char *arguments,
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity);
size_t solar_os_agent_tools_activate(
    const char *name,
    const solar_os_agent_request_t *request,
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t count,
    size_t capacity);
bool solar_os_agent_tools_is_discovery(const char *name);
size_t solar_os_agent_tools_count(void);
bool solar_os_agent_tools_get(size_t index, solar_os_agent_tool_info_t *info);
esp_err_t solar_os_agent_tools_execute(const char *name,
                                       const char *arguments,
                                       const solar_os_agent_request_t *request,
                                       solar_os_agent_tool_policy_t policy,
                                       bool confirmed,
                                       char *result,
                                       size_t result_len);
/* Invoke with current settings and the same approval handling as a run. */
esp_err_t solar_os_agent_invoke_tool(const char *name, const char *arguments,
                                     const solar_os_agent_request_t *request,
                                     char *result, size_t capacity);
solar_os_agent_tool_policy_decision_t solar_os_agent_tools_policy_decision(
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_risk_t risk);
const char *solar_os_agent_tool_risk_name(solar_os_agent_tool_risk_t risk);
