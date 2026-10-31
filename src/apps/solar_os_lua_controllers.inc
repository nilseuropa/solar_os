#if SOLAR_OS_PACKAGE_SERVICE_CONTROLLERS
static void solua_controller_info(lua_State *L, const solar_os_controller_info_t *info)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_str(L, item, "name", info->config.name);
    solua_set_str(L, item, "kind", info->config.algorithm.kind == SOLAR_OS_CONTROLLER_PID ? "pid" : "hysteresis");
    solua_set_str(L, item, "source", info->config.source);
    solua_set_str(L, item, "target", info->config.target);
    solua_set_str(L, item, "last_error_name", esp_err_to_name(info->last_error));
    solua_set_num(L, item, "setpoint", info->config.setpoint);
    solua_set_num(L, item, "setpoint_min", info->config.setpoint_min);
    solua_set_num(L, item, "setpoint_max", info->config.setpoint_max);
    solua_set_num(L, item, "safe_output", info->config.safe_output);
    solua_set_int(L, item, "period_ms", info->config.period_ms);
    solua_set_int(L, item, "timeout_ms", info->config.timeout_ms);
    solua_set_int(L, item, "frequency_hz", info->config.frequency_hz);
    solua_set_num(L, item, "kp", info->config.algorithm.kp);
    solua_set_num(L, item, "ki", info->config.algorithm.ki);
    solua_set_num(L, item, "kd", info->config.algorithm.kd);
    solua_set_num(L, item, "derivative_tau", info->config.algorithm.derivative_tau);
    solua_set_num(L, item, "output_min", info->config.algorithm.output_min);
    solua_set_num(L, item, "output_max", info->config.algorithm.output_max);
    solua_set_num(L, item, "hysteresis", info->config.algorithm.hysteresis);
    solua_set_num(L, item, "max_dt", info->config.algorithm.max_dt);
    solua_set_bool(L, item, "reverse", info->config.algorithm.reverse);
    solua_set_bool(L, item, "enabled", info->enabled);
    solua_set_bool(L, item, "claimed", info->claimed);
    solua_set_bool(L, item, "faulted", info->faulted);
    solua_set_bool(L, item, "has_measurement", info->has_measurement);
    solua_set_bool(L, item, "automatic", info->state.automatic);
    solua_set_bool(L, item, "saturated", info->state.saturated);
    solua_set_num(L, item, "error", info->state.error);
    solua_set_num(L, item, "p", info->state.p);
    solua_set_num(L, item, "i", info->state.i);
    solua_set_num(L, item, "d", info->state.d);
    solua_set_num(L, item, "output", info->state.output);
    solua_set_num(L, item, "measurement", info->measurement);
    solua_set_int(L, item, "sample_ms", info->sample_ms);
    solua_set_int(L, item, "steps", info->steps);
    solua_set_int(L, item, "faults", info->faults);
    solua_set_int(L, item, "last_error", info->last_error);
}
static int solua_controllers_status(lua_State *L)
{
    solar_os_controller_info_t info;
    (void)solua_check_esp(L, solar_os_controller_find(luaL_checkstring(L, 1), &info));
    solua_controller_info(L, &info);
    return 1;
}
static int solua_controllers_list(lua_State *L)
{
    lua_newtable(L);
    int list = lua_gettop(L), out = 1;
    for (size_t i = 0; i < solar_os_controller_count(); i++) {
        solar_os_controller_info_t info;
        if (solar_os_controller_get_info(i, &info)) {
            solua_controller_info(L, &info); lua_rawseti(L, list, out++);
        }
    }
    return 1;
}
static int solua_controllers_create(lua_State *L)
{
    const char *kind = luaL_optstring(L, 2, "pid");
    if (strcmp(kind, "pid") != 0 && strcmp(kind, "hysteresis") != 0)
        return luaL_error(L, "expected pid or hysteresis");
    (void)solua_check_esp(L, solar_os_controller_create(luaL_checkstring(L, 1),
        strcmp(kind, "pid") == 0 ? SOLAR_OS_CONTROLLER_PID : SOLAR_OS_CONTROLLER_HYSTERESIS));
    return solua_controllers_status(L);
}
static int solua_controllers_configure(lua_State *L)
{
    return solua_check_esp(L, solar_os_controller_configure(luaL_checkstring(L, 1),
        luaL_checkstring(L, 2), (float)luaL_checknumber(L, 3)));
}
static int solua_controllers_bind(lua_State *L)
{
    return solua_check_esp(L, solar_os_controller_bind(luaL_checkstring(L, 1),
        luaL_checkstring(L, 2), luaL_checkstring(L, 3)));
}
static int solua_controllers_feed(lua_State *L)
{
    return solua_check_esp(L, solar_os_controller_feed(luaL_checkstring(L, 1),
        (float)luaL_checknumber(L, 2), solar_os_time_uptime_ms()));
}
static int solua_controllers_step(lua_State *L)
{
    float output;
    (void)solua_check_esp(L, solar_os_controller_step(luaL_checkstring(L, 1),
        (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), &output));
    lua_pushnumber(L, output); return 1;
}
static int solua_controllers_manual(lua_State *L)
{
    return solua_check_esp(L, solar_os_controller_manual(luaL_checkstring(L, 1),
        (float)luaL_checknumber(L, 2)));
}
static int solua_controllers_auto(lua_State *L)
{ return solua_check_esp(L, solar_os_controller_auto(luaL_checkstring(L, 1))); }
static int solua_controllers_reset(lua_State *L)
{ return solua_check_esp(L, solar_os_controller_reset(luaL_checkstring(L, 1))); }
static int solua_controllers_start(lua_State *L)
{ return solua_check_esp(L, solar_os_controller_start(luaL_checkstring(L, 1))); }
static int solua_controllers_stop(lua_State *L)
{ return solua_check_esp(L, solar_os_controller_stop(luaL_checkstring(L, 1))); }
static int solua_controllers_delete(lua_State *L)
{ return solua_check_esp(L, solar_os_controller_delete(luaL_checkstring(L, 1))); }
#endif
