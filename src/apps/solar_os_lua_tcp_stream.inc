/* Package-owned TCP stream bindings; the core owns named endpoints. */
static const solar_os_native_tcp_stream_api_v1_t *solua_tcp_stream_cached_api;
static const solar_os_native_tcp_stream_api_v1_t *solua_tcp_stream_api(lua_State *L)
{
    if (!solua_tcp_stream_cached_api) {
        solua_tcp_stream_cached_api = solar_os_native_tcp_stream_get(solar_os_native_host_v1()->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_tcp_stream_api_v1_t, remove));
        if (!solua_tcp_stream_cached_api) luaL_error(L, "TCP stream service unavailable");
    }
    return solua_tcp_stream_cached_api;
}
static void solua_tcp_stream_cleanup(void) { solua_tcp_stream_cached_api = NULL; }
static const char *solua_tcp_stream_text(lua_State *L, int index)
{
    size_t length;
    luaL_checktype(L, index, LUA_TSTRING);
    const char *value = luaL_checklstring(L, index, &length);
    if (!length || memchr(value, 0, length)) luaL_error(L, "invalid TCP stream name or host");
    return value;
}
static int solua_tcp_stream_check(lua_State *L, int result)
{ return result ? luaL_error(L, "TCP stream error %d", result) : 0; }
static int solua_streams_tcp_connect(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 3);
    if (port < 1 || port > UINT16_MAX) return luaL_error(L, "invalid port");
    return solua_tcp_stream_check(L, solua_tcp_stream_api(L)->connect(
        solua_tcp_stream_text(L, 1), solua_tcp_stream_text(L, 2), (uint32_t)port));
}
static int solua_streams_tcp_listen(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 2);
    if (port < 1 || port > UINT16_MAX) return luaL_error(L, "invalid port");
    return solua_tcp_stream_check(L, solua_tcp_stream_api(L)->listen(solua_tcp_stream_text(L, 1), (uint32_t)port));
}
static int solua_streams_tcp_remove(lua_State *L)
{ return solua_tcp_stream_check(L, solua_tcp_stream_api(L)->remove(solua_tcp_stream_text(L, 1))); }
