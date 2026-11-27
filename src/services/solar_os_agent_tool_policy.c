#include "solar_os_agent_tools.h"

solar_os_agent_tool_policy_decision_t solar_os_agent_tools_policy_decision(
    solar_os_agent_tool_policy_t policy,
    solar_os_agent_tool_risk_t risk)
{
    switch (policy) {
    case SOLAR_OS_AGENT_TOOL_POLICY_OFF:
        return SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    case SOLAR_OS_AGENT_TOOL_POLICY_READONLY:
        return risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY ?
            SOLAR_OS_AGENT_TOOL_POLICY_ALLOW :
            SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    case SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM:
        return risk == SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY ?
            SOLAR_OS_AGENT_TOOL_POLICY_ALLOW :
            SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE;
    case SOLAR_OS_AGENT_TOOL_POLICY_ALL:
        return SOLAR_OS_AGENT_TOOL_POLICY_ALLOW;
    default:
        return SOLAR_OS_AGENT_TOOL_POLICY_DENY;
    }
}

const char *solar_os_agent_tool_risk_name(solar_os_agent_tool_risk_t risk)
{
    switch (risk) {
    case SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY:
        return "read-only";
    case SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ:
        return "sensitive-read";
    case SOLAR_OS_AGENT_TOOL_RISK_MUTATING:
        return "mutating";
    case SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE:
        return "disruptive";
    default:
        return "unknown";
    }
}
