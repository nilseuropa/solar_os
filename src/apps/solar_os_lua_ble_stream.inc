/* Package-owned Bluetooth byte-stream management; definitions live in core. */
static const solar_os_native_ble_stream_api_v1_t *solua_ble_stream_cached_api;
static const solar_os_native_ble_stream_api_v1_t *solua_ble_stream_api(lua_State *L)
{
    if (!solua_ble_stream_cached_api) {
        solua_ble_stream_cached_api = solar_os_native_ble_stream_get(
            SOLAR_OS_SCRIPT_SERVICE_QUERY(ble_stream),
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_ble_stream_api_v1_t, status));
        if (!solua_ble_stream_cached_api) luaL_error(L, "Bluetooth stream service unavailable");
    }
    return solua_ble_stream_cached_api;
}
static void solua_ble_stream_cleanup(void)
{ solua_ble_stream_cached_api = NULL; }
static const char *solua_ble_stream_name(lua_State *L, int index)
{
    size_t length;
    luaL_checktype(L, index, LUA_TSTRING);
    const char *name = luaL_checklstring(L, index, &length);
    if (!length || memchr(name, 0, length)) luaL_error(L, "invalid stream name");
    return name;
}
static int solua_streams_ble_connect(lua_State *L)
{
    size_t len;
    const char *address = luaL_checklstring(L, 2, &len);
    uint8_t bda[6];
    if (!solua_ble_api(L)->parse_address(address, len, bda)) return luaL_error(L, "expected aa:bb:cc:dd:ee:ff");
    lua_Integer type = luaL_optinteger(L, 3, 0);
    if (type < 0 || type > 3) return luaL_error(L, "address type must be 0..3");
    return solua_ble_check(L, solua_ble_stream_api(L)->connect(solua_ble_stream_name(L, 1), bda, type));
}
static int solua_streams_ble_listen(lua_State *L)
{ return solua_ble_check(L, solua_ble_stream_api(L)->listen(solua_ble_stream_name(L, 1))); }
static int solua_streams_ble_remove(lua_State *L)
{ return solua_ble_check(L, solua_ble_stream_api(L)->remove(solua_ble_stream_name(L, 1))); }
