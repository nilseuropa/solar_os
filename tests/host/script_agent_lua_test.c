#include "script_agent_test_support.inc"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int solua_check_esp(lua_State *L, esp_err_t error)
{ return error == ESP_OK ? 0 : luaL_error(L, "%s", esp_err_to_name(error)); }
static void solua_set_str(lua_State *L, int index, const char *key, const char *text)
{ index = lua_absindex(L, index); lua_pushstring(L, text); lua_setfield(L, index, key); }
static void solua_set_int(lua_State *L, int index, const char *key, lua_Integer value)
{ index = lua_absindex(L, index); lua_pushinteger(L, value); lua_setfield(L, index, key); }
static bool solua_should_cancel(void *user) { return script_should_cancel(user); }
#define SOLAR_OS_PACKAGE_SERVICE_AGENT 1
#include "solar_os_lua_agent.inc"

static bool fail_lua_alloc;
static void fail_conversion(void) { fail_lua_alloc = true; }
static void *lua_allocator(void *user, void *ptr, size_t old_size, size_t size)
{
    (void)user; (void)old_size;
    if (!size) { free(ptr); return NULL; }
    return fail_lua_alloc ? NULL : realloc(ptr, size);
}

static void check(lua_State *L, const char *source)
{
    int status = luaL_dostring(L, source);
    if (status != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(L, -1));
    assert(status == LUA_OK);
    assert(!allocations);
}

int main(void)
{
    assert(!solar_os_json_init());
    lua_State *L = lua_newstate(lua_allocator, NULL); assert(L);
    luaL_openlibs(L);
    lua_newtable(L);
#define METHOD(name) lua_pushcfunction(L, solua_agent_##name); lua_setfield(L, -2, #name)
    METHOD(ask); METHOD(status); METHOD(set_endpoint); METHOD(set_model); METHOD(set_api_key);
    METHOD(set_reasoning_effort); METHOD(set_tool_policy); METHOD(set_max_tools);
    METHOD(cancel); METHOD(forget); METHOD(conversations); METHOD(conversation_info); METHOD(delete_conversation);
#undef METHOD
    lua_setglobal(L, "agent");
    check(L, "local a = agent.ask('Describe the image.', 'stream:camera0'); assert(a.text == 'image seen' and a.total_tokens == 33 and not a.truncated and a.conversation_id == '')");
    check(L, "local a = agent.ask('Compare pictures.', {'photo.jpg', 'pipeline:1'}, ''); assert(a.conversation_id == 'c1'); assert(agent.ask('Followup.', nil, a.conversation_id).text == 'image seen')");
    check(L, "agent.set_endpoint('https://test/responses'); agent.set_model('vision'); agent.set_api_key('secret'); agent.set_reasoning_effort('low'); agent.set_tool_policy('off'); agent.set_max_tools(32); local s = agent.status(); assert(s.endpoint == 'https://test/responses' and s.model == 'vision' and s.api_key_set and s.api_key == nil and s.max_tools == 32 and s.tool_policy == 'off'); agent.set_api_key(''); assert(not agent.status().api_key_set)");
    check(L, "assert(agent.conversations()[1].id == 'c1'); assert(agent.conversation_info('c1').created_at == 5000000000); agent.delete_conversation('c1')");
    check(L, "for _, value in ipairs({0, -1, 33, 256, 1.5}) do assert(not pcall(agent.set_max_tools, value)) end; assert(not pcall(agent.ask, 'Question.', {'a', 'b', 'c'})); assert(not pcall(agent.ask, 'Question.', {42})); assert(not pcall(agent.ask, 'Question.', 'a' .. string.char(0) .. 'b')); assert(not pcall(agent.ask, 'Question.', {'photo.jpg', 'missing.jpg'})); assert(not pcall(agent.ask, 'Question.', 'unsupported.jpg'))");
    agent_error = ESP_ERR_INVALID_STATE;
    check(L, "assert(not pcall(agent.ask, 'Already running.'))"); agent_error = ESP_OK;
    cancel_during_run = true;
    check(L, "assert(not pcall(agent.ask, 'Cancelled.'))"); cancel_during_run = script_cancel = false;
    check(L, "agent.forget(); assert(agent.status().model == ''); agent.cancel()"); script_cancel = false;

    after_run = fail_conversion;
    lua_getglobal(L, "agent"); lua_getfield(L, -1, "ask"); lua_pushliteral(L, "Conversion OOM.");
    int status = lua_pcall(L, 1, 1, 0);
    fail_lua_alloc = false; after_run = NULL;
    assert(status == LUA_ERRMEM && !allocations);
    lua_close(L);
    puts("Lua agent arguments, image queries, status, history, errors and conversion OOM passed");
}
