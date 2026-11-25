/* Package-owned bindings to the OS HID output device. */
static const solar_os_native_hid_device_api_v1_t *solua_hid_cached_api;
static const solar_os_native_hid_device_api_v1_t *solua_hid_api(lua_State *L)
{
    if (!solua_hid_cached_api) {
        solua_hid_cached_api = solar_os_native_hid_device_get(
            solar_os_native_host_v1()->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_hid_device_api_v1_t, release_all));
        if (!solua_hid_cached_api) luaL_error(L, "HID service unavailable");
    }
    return solua_hid_cached_api;
}
static int solua_hid_check(lua_State *L, int result)
{ return result ? luaL_error(L, "HID error %d", result) : 0; }
static void solua_hid_cleanup(void)
{
    /* Cleanup cannot raise VM errors, including with an unavailable service.
     * Preserve the core's queued typing and neutralize every report type. */
    const solar_os_native_hid_device_api_v1_t *api = solua_hid_cached_api;
    if (!api) api = solar_os_native_hid_device_get(solar_os_native_host_v1()->get_service,
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_hid_device_api_v1_t, release_all));
    if (api) api->release_all();
    solua_hid_cached_api = NULL;
}

static int solua_hid_status(lua_State *L)
{
    solar_os_hid_status_t status;
    solua_hid_api(L)->get_status(&status);
    lua_createtable(L, 0, 2);
    solua_set_bool(L, -1, "initialized", status.initialized);
    solua_set_bool(L, -1, "connected", status.connected);
    return 1;
}

static size_t solua_hid_keys(lua_State *L, uint16_t *keys, size_t capacity)
{
    const int count = lua_gettop(L);
    if (count < 1 || (size_t)count > capacity) {
        luaL_error(L, "expected 1..8 keys");
    }
    for (int index = 0; index < count; index++) {
        const lua_Integer value = luaL_checkinteger(L, index + 1);
        if (value < 0 || value > UINT16_MAX) {
            luaL_error(L, "invalid HID key");
        }
        keys[index] = (uint16_t)value;
    }
    return (size_t)count;
}

static int solua_hid_keyboard_press(lua_State *L)
{
    uint16_t keys[8];
    return solua_hid_check(L,
                           solua_hid_api(L)->keyboard_press(
                               keys, solua_hid_keys(L, keys, 8)));
}

static int solua_hid_keyboard_release(lua_State *L)
{
    uint16_t keys[8];
    return solua_hid_check(L,
                           solua_hid_api(L)->keyboard_release(
                               keys, solua_hid_keys(L, keys, 8)));
}

static int solua_hid_keyboard_release_all(lua_State *L)
{
    (void)L;
    return solua_hid_check(L, solua_hid_api(L)->keyboard_release_all());
}

static int solua_hid_mouse_move(lua_State *L)
{
    const lua_Integer x = luaL_checkinteger(L, 1);
    const lua_Integer y = luaL_checkinteger(L, 2);
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
        return luaL_error(L, "mouse delta out of range");
    }
    return solua_hid_check(L, solua_hid_api(L)->mouse_move((int32_t)x, (int32_t)y));
}

static int solua_hid_mouse_button(lua_State *L)
{
    const lua_Integer button = luaL_checkinteger(L, 1);
    if (button < 0 || button > UINT8_MAX) {
        return luaL_error(L, "invalid mouse button");
    }
    return solua_hid_check(L,
                           solua_hid_api(L)->mouse_button((uint8_t)button,
                                                     lua_toboolean(L, 2)));
}

static int solua_hid_gamepad_axis(lua_State *L)
{
    const lua_Integer axis = luaL_checkinteger(L, 1);
    if (axis < SOLAR_OS_HID_AXIS_X || axis >= SOLAR_OS_HID_AXIS_COUNT)
        return luaL_error(L, "invalid gamepad axis");
    const lua_Integer value = luaL_checkinteger(L, 2);
    if (value < INT16_MIN || value > INT16_MAX) {
        return luaL_error(L, "expected axis value -32768..32767");
    }
    return solua_hid_check(L,
                           solua_hid_api(L)->gamepad_axis((solar_os_hid_axis_t)axis,
                                                     (int16_t)value));
}

static int solua_hid_gamepad_button(lua_State *L)
{
    const lua_Integer button = luaL_checkinteger(L, 1);
    if (button < 0 || button > UINT8_MAX) {
        return luaL_error(L, "invalid gamepad button");
    }
    return solua_hid_check(L,
                           solua_hid_api(L)->gamepad_button((uint8_t)button,
                                                       lua_toboolean(L, 2)));
}

static int solua_hid_gamepad_hat(lua_State *L)
{
    const lua_Integer hat = luaL_checkinteger(L, 1);
    if (hat < SOLAR_OS_HID_HAT_CENTERED || hat > SOLAR_OS_HID_HAT_UP_LEFT)
        return luaL_error(L, "invalid gamepad hat");
    return solua_hid_check(L, solua_hid_api(L)->gamepad_hat((solar_os_hid_hat_t)hat));
}

static int solua_hid_gamepad_send(lua_State *L)
{
    (void)L;
    return solua_hid_check(L, solua_hid_api(L)->gamepad_send());
}
