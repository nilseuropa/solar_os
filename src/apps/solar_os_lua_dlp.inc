#if SOLAR_OS_PACKAGE_SERVICE_DLP_CLIENT
typedef struct { unsigned slot; uint32_t generation; } solua_dlp_t;
static bool solua_dlp_live(const solua_dlp_t *self)
{
    return solua_runtime_state && self->slot < 4 &&
        solua_runtime_state->dlp_generation[self->slot] == self->generation &&
        solua_runtime_state->dlp_links[self->slot];
}
static solar_os_display_link_t *solua_dlp_link(lua_State *L)
{
    solua_dlp_t *self = luaL_checkudata(L, 1, "solaros.dlp.connection");
    if (!solua_dlp_live(self)) luaL_error(L, "DLP connection is closed");
    return solua_runtime_state->dlp_links[self->slot];
}
static void solua_dlp_destroy(void)
{
    if (!solua_runtime_state) return;
    for (unsigned i = 0; i < 4; i++) {
        solar_os_display_link_destroy(solua_runtime_state->dlp_links[i]);
        solua_runtime_state->dlp_links[i] = NULL;
    }
}
static int solua_dlp_connection_close(lua_State *L)
{
    solua_dlp_t *self = luaL_checkudata(L, 1, "solaros.dlp.connection");
    if (solua_dlp_live(self)) {
        solar_os_display_link_destroy(solua_runtime_state->dlp_links[self->slot]);
        solua_runtime_state->dlp_links[self->slot] = NULL;
    }
    return 0;
}
static int solua_dlp_connection_info(lua_State *L)
{
    solar_os_display_link_t *link = solua_dlp_link(L);
    const sdlp_info_t *info = solar_os_display_link_info(link);
    unsigned format = solar_os_display_link_format(link);
    lua_newtable(L);
    solua_set_int(L, -1, "width", info->width);
    solua_set_int(L, -1, "height", info->height);
    solua_set_int(L, -1, "format", format);
    solua_set_int(L, -1, "stride", sdlp_row_bytes(info->width, format));
    solua_set_int(L, -1, "frame_bytes", sdlp_row_bytes(info->width, format) * info->height);
    solua_set_int(L, -1, "formats", info->formats);
    solua_set_int(L, -1, "capabilities", info->capabilities);
    solua_set_str(L, -1, "model", info->model);
    solua_set_str(L, -1, "firmware", info->firmware);
    return 1;
}
static int solua_dlp_connection_present(lua_State *L)
{
    solar_os_display_link_t *link = solua_dlp_link(L);
    size_t size;
    const char *pixels = luaL_checklstring(L, 2, &size);
    return solua_check_esp(L, solar_os_display_link_present(link, (const uint8_t *)pixels, size));
}
static int solua_dlp_connect(lua_State *L)
{
    const char *stream = luaL_checkstring(L, 1);
    lua_Integer format = luaL_optinteger(L, 2, SDLP_MONO1);
    lua_Integer timeout = luaL_optinteger(L, 3, 5000);
    if (format < -1 || format > 4 || timeout < 100 || timeout > 60000)
        return luaL_error(L, "invalid DLP format or timeout");
    unsigned slot;
    for (slot = 0; slot < 4 && solua_runtime_state->dlp_links[slot]; slot++) {}
    if (slot == 4) return luaL_error(L, "too many DLP connections");
    solua_dlp_t *self = lua_newuserdatauv(L, sizeof(*self), 0);
    self->slot = 4; self->generation = 0;
    if (luaL_newmetatable(L, "solaros.dlp.connection")) {
        solua_set_func(L, -1, "close", solua_dlp_connection_close);
        solua_set_func(L, -1, "info", solua_dlp_connection_info);
        solua_set_func(L, -1, "present", solua_dlp_connection_present);
        solua_set_func(L, -1, "__gc", solua_dlp_connection_close);
        solua_set_func(L, -1, "__close", solua_dlp_connection_close);
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
    solar_os_display_link_options_t options = {.format = format, .timeout_ms = timeout,
        .cancelled = solua_should_cancel};
    solar_os_display_link_t *link = NULL;
    solua_check_esp(L, solar_os_display_link_open_stream(stream, "lua-dlp", &options, &link));
    solua_runtime_state->dlp_links[slot] = link;
    if (!++solua_runtime_state->dlp_generation[slot]) ++solua_runtime_state->dlp_generation[slot];
    self->slot = slot; self->generation = solua_runtime_state->dlp_generation[slot];
    return 1;
}
#else
static void solua_dlp_destroy(void) {}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE_STREAM
static int solua_streams_ble_connect(lua_State *L)
{
    size_t len;
    const char *address = luaL_checklstring(L, 2, &len);
    uint8_t bda[6];
    if (!solar_os_ble_parse_address(address, len, bda)) return luaL_error(L, "expected aa:bb:cc:dd:ee:ff");
    lua_Integer type = luaL_optinteger(L, 3, 0);
    if (type < 0 || type > 3) return luaL_error(L, "address type must be 0..3");
    return solua_check_esp(L, solar_os_ble_stream_connect(luaL_checkstring(L, 1), bda, type));
}
static int solua_streams_ble_listen(lua_State *L)
{ return solua_check_esp(L, solar_os_ble_stream_listen(luaL_checkstring(L, 1))); }
static int solua_streams_ble_remove(lua_State *L)
{ return solua_check_esp(L, solar_os_ble_stream_remove(luaL_checkstring(L, 1))); }
#endif

#if SOLAR_OS_PACKAGE_SERVICE_TCP_STREAM
static int solua_streams_tcp_connect(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 3);
    if (port < 1 || port > UINT16_MAX) return luaL_error(L, "invalid port");
    return solua_check_esp(L, solar_os_tcp_stream_connect(luaL_checkstring(L, 1),
        luaL_checkstring(L, 2), port));
}
static int solua_streams_tcp_listen(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 2);
    if (port < 1 || port > UINT16_MAX) return luaL_error(L, "invalid port");
    return solua_check_esp(L, solar_os_tcp_stream_listen(luaL_checkstring(L, 1), port));
}
static int solua_streams_tcp_remove(lua_State *L)
{
    return solua_check_esp(L, solar_os_tcp_stream_remove(luaL_checkstring(L, 1)));
}
#endif
