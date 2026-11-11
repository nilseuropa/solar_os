#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
#include "solar_os_usb_serial.h"

static int solua_usb_serial_configure(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    solar_os_port_serial_config_t config = {0};
    uint32_t mask = 0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            luaL_checktype(L, -2, LUA_TSTRING);
            const char *key = lua_tostring(L, -2);
            const char *value = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? "1" : "0") : lua_tostring(L, -1);
            solua_check_esp(L, solar_os_usb_serial_parse_option(key, value, &config, &mask));
            lua_pop(L, 1);
        }
    }
    solua_check_esp(L, solar_os_usb_serial_config(name, &config, mask));
    lua_newtable(L);
    lua_pushinteger(L, config.baud_rate); lua_setfield(L, -2, "baud");
    lua_pushinteger(L, config.data_bits); lua_setfield(L, -2, "bits");
    static const char *parity[] = {"none", "odd", "even", "mark", "space"};
    static const char *stop[] = {"1", "1.5", "2"};
    lua_pushstring(L, parity[config.parity]); lua_setfield(L, -2, "parity");
    lua_pushstring(L, stop[config.stop_bits]); lua_setfield(L, -2, "stop");
    lua_pushboolean(L, config.dtr); lua_setfield(L, -2, "dtr");
    lua_pushboolean(L, config.rts); lua_setfield(L, -2, "rts");
    return 1;
}

static int solua_usb_serial_send_break(lua_State *L)
{
    const lua_Integer duration = luaL_checkinteger(L, 2);
    luaL_argcheck(L, duration >= 0 && duration <= UINT16_MAX, 2, "break duration out of range");
    return solua_check_esp(L, solar_os_usb_serial_send_break(luaL_checkstring(L, 1), duration));
}
#endif
