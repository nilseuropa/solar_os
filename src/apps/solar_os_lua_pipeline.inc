#if SOLAR_OS_PACKAGE_SERVICE_PIPELINE
#include "solar_os_pipeline.h"
#include "cJSON.h"
static uint32_t solua_pipeline_uint(lua_State *L, int index)
{
    lua_Integer n = luaL_checkinteger(L, index);
    if (n < 0 || (uint64_t)n > UINT32_MAX) luaL_error(L, "pipeline integer out of range");
    return n;
}
static int solua_pipeline_start(lua_State *L)
{
    size_t length; const char *source = luaL_checklstring(L, 1, &length);
    if (memchr(source, 0, length)) luaL_error(L, "embedded NUL in source");
    const char *kind = luaL_optlstring(L, 2, "qr", &length);
    if (memchr(kind, 0, length)) luaL_error(L, "embedded NUL in processor");
    solar_os_pipeline_config_t c = {.source = source, .processor = kind, .timeout_ms = 10000};
    if (!lua_isnoneornil(L, 3)) c.model = solua_pipeline_uint(L, 3);
    if (!lua_isnoneornil(L, 4)) c.limit = solua_pipeline_uint(L, 4);
    if (!lua_isnoneornil(L, 5)) c.interval_ms = solua_pipeline_uint(L, 5);
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (source[0] == '/' || source[0] == '.') { solua_resolve_path(L, 1, path, sizeof(path)); c.source = path; }
    uint32_t id; (void)solua_check_esp(L, solar_os_pipeline_start(&c, &id));
    lua_pushinteger(L, id); return 1;
}
static int solua_pipeline_list(lua_State *L)
{
    uint32_t ids[SOLAR_OS_PIPELINES_MAX]; size_t count;
    (void)solua_check_esp(L, solar_os_pipeline_list(ids, SOLAR_OS_PIPELINES_MAX, &count));
    lua_newtable(L);
    for (size_t i = 0; i < count; ++i) { lua_pushinteger(L, ids[i]); lua_rawseti(L, -2, i + 1); }
    return 1;
}
static int solua_pipeline_status(lua_State *L)
{
    solar_os_pipeline_status_t s; (void)solua_check_esp(L, solar_os_pipeline_status(solua_pipeline_uint(L, 1), &s));
    lua_newtable(L);
    solua_set_int(L, -1, "id", s.id); solua_set_int(L, -1, "model", s.model);
    solua_set_str(L, -1, "job", s.job); solua_set_str(L, -1, "state", s.state);
    solua_set_str(L, -1, "source", s.source); solua_set_str(L, -1, "processor", s.processor);
    solua_set_int(L, -1, "sequence", s.sequence); solua_set_int(L, -1, "frames", s.frames);
    solua_set_int(L, -1, "busy_frames", s.busy_frames);
    solua_set_int(L, -1, "source_timestamp_us", s.source_timestamp_us);
    solua_set_int(L, -1, "decode_us", s.decode_us); solua_set_int(L, -1, "process_us", s.process_us);
    solua_set_int(L, -1, "frame_us", s.frame_us); solua_set_int(L, -1, "last_error", s.last_error);
    lua_pushboolean(L, s.done); lua_setfield(L, -2, "done"); return 1;
}
static void solua_pipeline_json_value(lua_State *L, const cJSON *value)
{
    if (cJSON_IsObject(value) || cJSON_IsArray(value)) {
        lua_newtable(L); unsigned index = 1;
        for (const cJSON *item = value->child; item; item = item->next) {
            solua_pipeline_json_value(L, item);
            if (cJSON_IsArray(value)) lua_rawseti(L, -2, index++); else lua_setfield(L, -2, item->string);
        }
    } else if (cJSON_IsString(value)) lua_pushstring(L, value->valuestring);
    else if (cJSON_IsNumber(value)) lua_pushnumber(L, value->valuedouble);
    else if (cJSON_IsBool(value)) lua_pushboolean(L, cJSON_IsTrue(value));
    else lua_pushnil(L);
}
static int solua_pipeline_json_callback(lua_State *L)
{ solua_pipeline_json_value(L, lua_touserdata(L, 1)); return 1; }
static int solua_pipeline_result(lua_State *L)
{
    luaL_checkstack(L, 4, "pipeline result");
    char *json = NULL;
    (void)solua_check_esp(L, solar_os_pipeline_result(solua_pipeline_uint(L, 1),
        lua_isnoneornil(L, 2) ? 0 : solua_pipeline_uint(L, 2), &json));
    if (!json) { lua_pushnil(L); return 1; }
    cJSON *value = cJSON_Parse(json); solar_os_memory_free(json);
    if (!value) return luaL_error(L, "pipeline JSON allocation failed");
    lua_pushcfunction(L, solua_pipeline_json_callback); lua_pushlightuserdata(L, value);
    int status = lua_pcall(L, 1, 1, 0); cJSON_Delete(value);
    if (status != LUA_OK) return lua_error(L);
    return 1;
}
static int solua_pipeline_stop(lua_State *L)
{ (void)solua_check_esp(L, solar_os_pipeline_stop(solua_pipeline_uint(L, 1))); return 0; }
static int solua_pipeline_destroy(lua_State *L)
{ (void)solua_check_esp(L, solar_os_pipeline_destroy(solua_pipeline_uint(L, 1))); return 0; }
#endif
