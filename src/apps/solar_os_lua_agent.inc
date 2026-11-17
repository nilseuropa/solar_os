/* Lua values are converted here; the shared adapter owns request lifetime. */
#if SOLAR_OS_PACKAGE_SERVICE_AGENT
#include "solar_os_script_agent.h"

static const char *solua_agent_string(lua_State *L, int index)
{
    luaL_checktype(L, index, LUA_TSTRING);
    size_t length;
    const char *text = luaL_checklstring(L, index, &length);
    if (strlen(text) != length) luaL_error(L, "embedded NUL in agent argument");
    return text;
}

static int solua_agent_status(lua_State *L)
{
    solar_os_agent_status_t status;
    (void)solua_check_esp(L, solar_os_agent_get_status(&status));
    lua_newtable(L);
    solua_set_str(L, -1, "provider", status.provider);
    solua_set_str(L, -1, "endpoint", status.endpoint);
    solua_set_str(L, -1, "model", status.model);
    solua_set_str(L, -1, "reasoning_effort", status.reasoning_effort);
    lua_pushboolean(L, status.initialized); lua_setfield(L, -2, "initialized");
    lua_pushboolean(L, status.configured); lua_setfield(L, -2, "configured");
    lua_pushboolean(L, status.api_key_set); lua_setfield(L, -2, "api_key_set");
    lua_pushboolean(L, status.running); lua_setfield(L, -2, "running");
    solua_set_int(L, -1, "max_tools", status.max_tools);
    solua_set_int(L, -1, "last_tool_call_count", status.last_tool_call_count);
    solua_set_int(L, -1, "last_max_tools", status.last_max_tools);
    solua_set_int(L, -1, "request_count", status.request_count);
    solua_set_int(L, -1, "failure_count", status.failure_count);
    solua_set_int(L, -1, "tool_executed_count", status.tool_executed_count);
    solua_set_int(L, -1, "tool_denied_count", status.tool_denied_count);
    solua_set_int(L, -1, "tool_failed_count", status.tool_failed_count);
    solua_set_int(L, -1, "last_duration_ms", status.last_duration_ms);
    solua_set_int(L, -1, "last_bytes_received", status.last_bytes_received);
    solua_set_int(L, -1, "last_internal_before", status.last_internal_before);
    solua_set_int(L, -1, "last_internal_low", status.last_internal_low);
    solua_set_int(L, -1, "last_internal_after", status.last_internal_after);
    solua_set_int(L, -1, "last_internal_largest_before", status.last_internal_largest_before);
    solua_set_int(L, -1, "last_internal_largest_after", status.last_internal_largest_after);
    solua_set_int(L, -1, "last_psram_before", status.last_psram_before);
    solua_set_int(L, -1, "last_psram_after", status.last_psram_after);
    solua_set_int(L, -1, "last_http_status", status.last_http_status);
    solua_set_int(L, -1, "last_error", status.last_error);
    solua_set_str(L, -1, "tool_policy", solar_os_agent_tool_policy_name(status.tool_policy));
    return 1;
}

static int solua_agent_push_result(lua_State *L)
{
    const solar_os_script_agent_result_t *response = lua_touserdata(L, 1);
    lua_newtable(L);
    solua_set_str(L, -1, "text", response->text);
    solua_set_str(L, -1, "conversation_id", response->conversation_id);
    lua_pushboolean(L, response->truncated); lua_setfield(L, -2, "truncated");
    solua_set_int(L, -1, "prompt_tokens", response->prompt_tokens);
    solua_set_int(L, -1, "completion_tokens", response->completion_tokens);
    solua_set_int(L, -1, "total_tokens", response->total_tokens);
    return 1;
}

static int solua_agent_request(lua_State *L, const solar_os_script_agent_request_t *request)
{
    /* Push the protected conversion before allocating the native result. */
    lua_pushcfunction(L, solua_agent_push_result);
    solar_os_script_agent_result_t response = {0};
    (void)solua_check_esp(L, solar_os_script_agent_ask(request, &response));
    lua_pushlightuserdata(L, &response);
    const int status = lua_pcall(L, 1, 1, 0);
    solar_os_script_agent_result_clear(&response);
    if (status != LUA_OK) return lua_error(L);
    return 1;
}

static int solua_agent_ask(lua_State *L)
{
    solar_os_script_agent_request_t request = {
        .prompt = solua_agent_string(L, 1), .should_cancel = solua_should_cancel,
    };
    if (!lua_isnoneornil(L, 2)) {
        if (lua_type(L, 2) == LUA_TSTRING) {
            request.image_count = 1;
            request.images[0] = solua_agent_string(L, 2);
        } else {
            luaL_checktype(L, 2, LUA_TTABLE);
            request.image_count = lua_rawlen(L, 2);
            if (request.image_count > SOLAR_OS_AGENT_IMAGES_MAX) return luaL_error(L, "at most two agent images");
            for (size_t i = 0; i < request.image_count; i++) {
                lua_rawgeti(L, 2, (lua_Integer)i + 1);
                request.images[i] = solua_agent_string(L, -1);
                lua_pop(L, 1);
            }
        }
    }
    if (!lua_isnoneornil(L, 3)) request.conversation_id = solua_agent_string(L, 3);
    return solua_agent_request(L, &request);
}

static int solua_agent_ask_audio(lua_State *L)
{
    solar_os_script_agent_request_t request = {
        .prompt = solua_agent_string(L, 1), .audio_file = solua_agent_string(L, 2),
        .should_cancel = solua_should_cancel,
    };
    if (!lua_isnoneornil(L, 3)) request.conversation_id = solua_agent_string(L, 3);
    return solua_agent_request(L, &request);
}

static int solua_agent_set_endpoint(lua_State *L)
{
    return solua_check_esp(L, solar_os_agent_set_endpoint(solua_agent_string(L, 1)));
}

static int solua_agent_set_model(lua_State *L)
{
    return solua_check_esp(L, solar_os_agent_set_model(solua_agent_string(L, 1)));
}

static int solua_agent_set_api_key(lua_State *L)
{
    return solua_check_esp(L, solar_os_agent_set_api_key(solua_agent_string(L, 1)));
}

static int solua_agent_set_reasoning_effort(lua_State *L)
{
    return solua_check_esp(L, solar_os_agent_set_reasoning_effort(solua_agent_string(L, 1)));
}

static int solua_agent_set_tool_policy(lua_State *L)
{
    solar_os_agent_tool_policy_t policy;
    (void)solua_check_esp(L, solar_os_agent_parse_tool_policy(solua_agent_string(L, 1), &policy));
    return solua_check_esp(L, solar_os_agent_set_tool_policy(policy));
}

static int solua_agent_set_max_tools(lua_State *L)
{
    const lua_Integer limit = luaL_checkinteger(L, 1);
    if (limit < SOLAR_OS_AGENT_MAX_TOOLS_MIN || limit > SOLAR_OS_AGENT_MAX_TOOLS_MAX)
        return luaL_error(L, "agent max_tools must be 1..32");
    return solua_check_esp(L, solar_os_agent_set_max_tools((uint8_t)limit));
}

static int solua_agent_cancel(lua_State *L) { return solua_check_esp(L, solar_os_agent_cancel()); }
static int solua_agent_forget(lua_State *L) { return solua_check_esp(L, solar_os_agent_forget()); }

static void solua_agent_push_info(lua_State *L, const solar_os_agent_conversation_info_t *info)
{
    lua_newtable(L);
    solua_set_str(L, -1, "id", info->id);
    solua_set_str(L, -1, "title", info->title);
    solua_set_str(L, -1, "provider", info->provider);
    solua_set_str(L, -1, "model", info->model);
    solua_set_int(L, -1, "turn_count", info->turn_count);
    solua_set_int(L, -1, "stored_bytes", info->stored_bytes);
    solua_set_int(L, -1, "created_at", info->created_at);
    solua_set_int(L, -1, "updated_at", info->updated_at);
}

static int solua_agent_conversations(lua_State *L)
{
    solar_os_agent_conversation_info_t items[SOLAR_OS_AGENT_CONVERSATION_LIST_MAX];
    size_t count;
    (void)solua_check_esp(L, solar_os_agent_conversations_list(items, SOLAR_OS_AGENT_CONVERSATION_LIST_MAX, &count));
    lua_newtable(L);
    for (size_t i = 0; i < count; i++) {
        solua_agent_push_info(L, &items[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

static int solua_agent_conversation_info(lua_State *L)
{
    solar_os_agent_conversation_info_t info;
    (void)solua_check_esp(L, solar_os_agent_conversation_get(solua_agent_string(L, 1), &info));
    solua_agent_push_info(L, &info);
    return 1;
}

static int solua_agent_delete_conversation(lua_State *L)
{
    return solua_check_esp(L, solar_os_agent_conversation_delete(solua_agent_string(L, 1)));
}
#endif
