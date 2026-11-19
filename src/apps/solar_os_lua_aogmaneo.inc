#if SOLAR_OS_PACKAGE_SERVICE_AOGMANEO
#define LUA_AON_CONTEXT (&solua_runtime_state->aogmaneo)
static bool solua_aogmaneo_active(void)
{ return solua_runtime_state && solar_os_aogmaneo_active(LUA_AON_CONTEXT); }
static void solua_aogmaneo_cleanup(void)
{
    if (solua_runtime_state)
        while (solar_os_aogmaneo_close_all(LUA_AON_CONTEXT)) vTaskDelay(pdMS_TO_TICKS(20));
}
static void solua_aogmaneo_check(lua_State *L, int result)
{
    if (result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND)
        luaL_error(L, "AOgmaNeo library unavailable; run pkg install aogmaneo");
    if (result) luaL_error(L, "AOgmaNeo error %d", result);
}
#define LUA_AON_CALL(expression) do { \
    if (solua_should_cancel(NULL)) luaL_error(L, "AOgmaNeo interrupted"); \
    int aon_result = (expression); \
    if (solua_should_cancel(NULL)) luaL_error(L, "AOgmaNeo interrupted"); \
    solua_aogmaneo_check(L, aon_result); \
} while (0)
static int32_t solua_aogmaneo_int(lua_State *L, int index, int32_t low, int32_t high, int32_t fallback)
{
    lua_Integer value = luaL_optinteger(L, index, fallback);
    if (value < low || value > high) luaL_error(L, "AOgmaNeo integer out of range");
    return (int32_t)value;
}
static size_t solua_aogmaneo_array(lua_State *L, int index, size_t expected, size_t maximum)
{
    luaL_checktype(L, index, LUA_TTABLE);
    size_t count = lua_rawlen(L, index);
    if (!count || count > maximum || (expected && count != expected))
        luaL_error(L, "invalid AOgmaNeo array size");
    return count;
}
static void solua_aogmaneo_fields(lua_State *L, int index, const char *const *names, size_t count)
{
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "invalid AOgmaNeo field");
        const char *key = lua_tostring(L, -2);
        size_t i = 0;
        for (; i < count && strcmp(key, names[i]); ++i) {}
        if (i == count) luaL_error(L, "unknown AOgmaNeo field");
        lua_pop(L, 1);
    }
}
static int32_t solua_aogmaneo_field(lua_State *L, int index, const char *key,
    int32_t low, int32_t high, int32_t fallback)
{
    lua_getfield(L, index, key);
    int32_t value = solua_aogmaneo_int(L, -1, low, high, fallback);
    lua_pop(L, 1);
    return value;
}
static solar_os_aogmaneo_size_v1_t solua_aogmaneo_size(lua_State *L, int index)
{
    index = lua_absindex(L, index);
    solua_aogmaneo_array(L, index, 3, 3);
    int32_t size[3];
    for (int i = 0; i < 3; ++i) {
        lua_rawgeti(L, index, i + 1);
        size[i] = solua_aogmaneo_int(L, -1, 1, i == 2 ? 256 : 64, 0);
        lua_pop(L, 1);
    }
    return (solar_os_aogmaneo_size_v1_t){size[0], size[1], size[2]};
}
static solar_os_aogmaneo_size_v1_t solua_aogmaneo_shape_field(lua_State *L, int index)
{
    lua_getfield(L, index, "size");
    solar_os_aogmaneo_size_v1_t value = solua_aogmaneo_size(L, -1);
    lua_pop(L, 1);
    return value;
}
static uint32_t solua_aogmaneo_handle(lua_State *L, bool image, solar_os_aogmaneo_info_t *info)
{
    uint32_t token = solua_aogmaneo_int(L, 1, 1, 0x3fffffff, 0);
    const solar_os_aogmaneo_info_t *found = solar_os_aogmaneo_info(LUA_AON_CONTEXT, token);
    if (!found || found->image != image) luaL_error(L, "invalid AOgmaNeo model");
    if (info) *info = *found;
    return token;
}
static size_t solua_aogmaneo_columns(solar_os_aogmaneo_size_v1_t size)
{ return (size_t)size.x * size.y; }
static int32_t *solua_aogmaneo_codes(lua_State *L, int index, size_t expected, int depth)
{
    index = lua_absindex(L, index);
    size_t count = solua_aogmaneo_array(L, index, expected, 4096);
    int32_t *codes = lua_newuserdatauv(L, count * sizeof(*codes), 0);
    for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(L, index, i + 1);
        codes[i] = solua_aogmaneo_int(L, -1, 0, depth - 1, -1);
        lua_pop(L, 1);
    }
    return codes;
}
static void solua_aogmaneo_code_list(lua_State *L, const int32_t *codes, size_t count)
{
    lua_createtable(L, count, 0);
    for (size_t i = 0; i < count; ++i) {
        lua_pushinteger(L, codes[i]);
        lua_rawseti(L, -2, i + 1);
    }
}
static int solua_aogmaneo_create(lua_State *L)
{
    size_t ni = solua_aogmaneo_array(L, 1, 0, 8), nl = solua_aogmaneo_array(L, 2, 0, 8);
    uint32_t seed = solua_optional_u32(L, 3, 12345), token = 0;
    solar_os_aogmaneo_io_v1_t inputs[8];
    solar_os_aogmaneo_layer_v1_t layers[8];
    const char *io_names[] = {"size", "type", "dendrites", "up_radius", "down_radius",
        "value_size", "value_dendrites", "history_capacity"};
    for (size_t i = 0; i < ni; ++i) {
        lua_rawgeti(L, 1, i + 1);
        int d = lua_gettop(L);
        solua_aogmaneo_fields(L, d, io_names, 8);
        inputs[i] = (solar_os_aogmaneo_io_v1_t){solua_aogmaneo_shape_field(L, d),
            solua_aogmaneo_field(L, d, "type", 0, 2, 1), solua_aogmaneo_field(L, d, "dendrites", 2, 16, 4),
            solua_aogmaneo_field(L, d, "up_radius", 0, 8, 2), solua_aogmaneo_field(L, d, "down_radius", 0, 8, 2),
            solua_aogmaneo_field(L, d, "value_size", 2, 256, 128), solua_aogmaneo_field(L, d, "value_dendrites", 2, 16, 2),
            solua_aogmaneo_field(L, d, "history_capacity", 2, 512, 512)};
        lua_pop(L, 1);
    }
    const char *layer_names[] = {"size", "dendrites", "up_radius", "recurrent_radius", "down_radius"};
    for (size_t i = 0; i < nl; ++i) {
        lua_rawgeti(L, 2, i + 1);
        int d = lua_gettop(L);
        solua_aogmaneo_fields(L, d, layer_names, 5);
        layers[i] = (solar_os_aogmaneo_layer_v1_t){solua_aogmaneo_shape_field(L, d),
            solua_aogmaneo_field(L, d, "dendrites", 2, 16, 4), solua_aogmaneo_field(L, d, "up_radius", 0, 8, 2),
            solua_aogmaneo_field(L, d, "recurrent_radius", -1, 8, 0), solua_aogmaneo_field(L, d, "down_radius", 0, 8, 2)};
        lua_pop(L, 1);
    }
    LUA_AON_CALL(solar_os_aogmaneo_create(LUA_AON_CONTEXT, inputs, ni, layers, nl, seed, &token));
    lua_pushinteger(L, token);
    return 1;
}
static int solua_aogmaneo_destroy(lua_State *L)
{
    uint32_t token = solua_aogmaneo_int(L, 1, 1, 0x3fffffff, 0);
    LUA_AON_CALL(solar_os_aogmaneo_destroy(LUA_AON_CONTEXT, token));
    return 0;
}
static int solua_aogmaneo_step(lua_State *L)
{
    solar_os_aogmaneo_info_t info;
    uint32_t token = solua_aogmaneo_handle(L, false, &info);
    size_t n = solua_aogmaneo_array(L, 2, info.input_count, 8);
    bool learn = lua_isnoneornil(L, 3) || lua_toboolean(L, 3);
    float reward = luaL_optnumber(L, 4, 0), mimic = luaL_optnumber(L, 5, 0);
    solar_os_aogmaneo_codes_v1_t inputs[8];
    luaL_checkstack(L, 24, "AOgmaNeo inputs");
    for (size_t i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        inputs[i].count = solua_aogmaneo_columns(info.inputs[i]);
        inputs[i].data = solua_aogmaneo_codes(L, -1, inputs[i].count, info.inputs[i].z);
    }
    LUA_AON_CALL(solar_os_aogmaneo_step(LUA_AON_CONTEXT, token, inputs, n, learn, reward, mimic));
    return 0;
}
static int solua_aogmaneo_read(lua_State *L, unsigned kind)
{
    solar_os_aogmaneo_info_t info;
    uint32_t token = solua_aogmaneo_handle(L, false, &info);
    unsigned limit = kind == SOLAR_OS_AOGMANEO_READ_HIDDEN ? info.layer_count : info.input_count;
    unsigned index = solua_aogmaneo_int(L, 2, 0, limit - 1, 0);
    solar_os_aogmaneo_size_v1_t size = kind == SOLAR_OS_AOGMANEO_READ_HIDDEN ? info.layers[index] : info.inputs[index];
    size_t n = solua_aogmaneo_columns(size);
    if (kind == SOLAR_OS_AOGMANEO_READ_ACTIVATIONS) n *= size.z;
    bool floats = kind == SOLAR_OS_AOGMANEO_READ_ACTIVATIONS || kind == SOLAR_OS_AOGMANEO_READ_VALUES;
    void *output = lua_newuserdatauv(L, n * (floats ? sizeof(float) : sizeof(int32_t)), 0);
    LUA_AON_CALL(solar_os_aogmaneo_read(LUA_AON_CONTEXT, token, kind, index, output, n));
    lua_createtable(L, n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (floats) lua_pushnumber(L, ((float *)output)[i]);
        else lua_pushinteger(L, ((int32_t *)output)[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
static int solua_aogmaneo_prediction(lua_State *L) { return solua_aogmaneo_read(L, SOLAR_OS_AOGMANEO_READ_PREDICTION); }
static int solua_aogmaneo_activations(lua_State *L) { return solua_aogmaneo_read(L, SOLAR_OS_AOGMANEO_READ_ACTIVATIONS); }
static int solua_aogmaneo_values(lua_State *L) { return solua_aogmaneo_read(L, SOLAR_OS_AOGMANEO_READ_VALUES); }
static int solua_aogmaneo_hidden(lua_State *L) { return solua_aogmaneo_read(L, SOLAR_OS_AOGMANEO_READ_HIDDEN); }
static int solua_aogmaneo_clear(lua_State *L)
{
    uint32_t token = solua_aogmaneo_handle(L, false, NULL);
    LUA_AON_CALL(solar_os_aogmaneo_clear(LUA_AON_CONTEXT, token));
    return 0;
}
static int solua_aogmaneo_image_create(lua_State *L)
{
    solar_os_aogmaneo_size_v1_t hidden = solua_aogmaneo_size(L, 1);
    size_t n = solua_aogmaneo_array(L, 2, 0, 8);
    uint32_t seed = solua_optional_u32(L, 3, 12345), token = 0;
    solar_os_aogmaneo_visible_v1_t visible[8];
    const char *names[] = {"size", "radius"};
    for (size_t i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        int d = lua_gettop(L);
        solua_aogmaneo_fields(L, d, names, 2);
        visible[i] = (solar_os_aogmaneo_visible_v1_t){solua_aogmaneo_shape_field(L, d),
            solua_aogmaneo_field(L, d, "radius", 0, 8, 2)};
        lua_pop(L, 1);
    }
    LUA_AON_CALL(solar_os_aogmaneo_image_create(LUA_AON_CONTEXT, hidden, visible, n, seed, &token));
    lua_pushinteger(L, token);
    return 1;
}
static int solua_aogmaneo_image_step(lua_State *L)
{
    solar_os_aogmaneo_info_t info;
    uint32_t token = solua_aogmaneo_handle(L, true, &info);
    size_t n = solua_aogmaneo_array(L, 2, info.input_count, 8);
    bool learn = lua_isnoneornil(L, 3) || lua_toboolean(L, 3), recon = lua_toboolean(L, 4);
    solar_os_aogmaneo_bytes_v1_t inputs[8];
    luaL_checkstack(L, 16, "AOgmaNeo images");
    for (size_t i = 0; i < n; ++i) {
        lua_rawgeti(L, 2, i + 1);
        luaL_checktype(L, -1, LUA_TSTRING);
        size_t bytes;
        const char *data = lua_tolstring(L, -1, &bytes);
        if (bytes != solua_aogmaneo_columns(info.inputs[i]) * info.inputs[i].z)
            return luaL_error(L, "invalid AOgmaNeo image size");
        inputs[i] = (solar_os_aogmaneo_bytes_v1_t){(const uint8_t *)data, bytes};
    }
    size_t count = solua_aogmaneo_columns(info.layers[0]);
    int32_t *output = lua_newuserdatauv(L, count * sizeof(*output), 0);
    LUA_AON_CALL(solar_os_aogmaneo_image_step(LUA_AON_CONTEXT, token, inputs, n, learn, recon, output, count));
    solua_aogmaneo_code_list(L, output, count);
    return 1;
}
static int solua_aogmaneo_image_reconstruct(lua_State *L)
{
    solar_os_aogmaneo_info_t info;
    uint32_t token = solua_aogmaneo_handle(L, true, &info);
    unsigned visible = solua_aogmaneo_int(L, 3, 0, info.input_count - 1, 0);
    size_t count = solua_aogmaneo_columns(info.layers[0]);
    int32_t *codes = solua_aogmaneo_codes(L, 2, count, info.layers[0].z);
    size_t bytes = solua_aogmaneo_columns(info.inputs[visible]) * info.inputs[visible].z;
    uint8_t *output = lua_newuserdatauv(L, bytes, 0);
    LUA_AON_CALL(solar_os_aogmaneo_image_reconstruct(LUA_AON_CONTEXT, token, codes, count, visible, output, bytes));
    lua_pushlstring(L, (const char *)output, bytes);
    return 1;
}
#undef LUA_AON_CALL
#undef LUA_AON_CONTEXT
#else
static bool solua_aogmaneo_active(void) { return false; }
static void solua_aogmaneo_cleanup(void) {}
#endif
