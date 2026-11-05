#if SOLAR_OS_PACKAGE_SERVICE_CONTROLLERS
static void solua_controller_info(lua_State *L, const solar_os_controller_info_t *info)
{
    lua_newtable(L);
    const int item = lua_gettop(L);
    solua_set_str(L, item, "name", info->config.name);
    solua_set_str(L, item, "owner", info->owner);
    solua_set_bool(L, item, "waiting", info->waiting);
    solua_set_num(L, item, "feedforward", info->feedforward);
    solua_set_int(L,item,"command_timeout_ms",info->config.command_timeout_ms);
    solua_set_num(L,item,"acceleration_limit",info->config.acceleration_limit);
    solua_set_num(L,item,"velocity_deadband",info->config.velocity_deadband);
    solua_set_num(L,item,"friction_bias_forward",info->config.friction_bias_forward);
    solua_set_num(L,item,"friction_bias_reverse",info->config.friction_bias_reverse);
    solua_set_num(L,item,"active_setpoint",info->active_setpoint);
    solua_set_bool(L,item,"command_valid",info->command_valid);
    const solar_os_controller_tune_result_t *t=&info->tuning;
    solua_set_bool(L,item,"input_claimed",info->input_claimed);
    solua_set_str(L,item,"tune_state",solar_os_controller_tune_state_name(t->state));
    solua_set_str(L,item,"tune_reason",solar_os_controller_tune_reason_name(t->reason));
    solua_set_int(L,item,"tune_cycles",t->cycles);
    solua_set_int(L,item,"tune_required_cycles",t->required_cycles);
    solua_set_num(L,item,"tune_elapsed",t->elapsed);
    solua_set_num(L,item,"tune_period",t->period);
    solua_set_num(L,item,"tune_amplitude",t->amplitude);
    solua_set_num(L,item,"tune_ultimate_gain",t->ultimate_gain);
    solua_set_num(L,item,"tune_kp",t->kp);
    solua_set_num(L,item,"tune_ki",t->ki);
    solua_set_num(L,item,"tune_kd",t->kd);
    solua_set_num(L,item,"tune_tau",t->derivative_tau);
    solua_set_str(L,item,"tune_phase",solar_os_controller_tune_phase_name(t->phase));
    solua_set_int(L,item,"tune_point",t->point);
    solua_set_int(L,item,"tune_points",t->points);
    solua_set_num(L,item,"tune_test_setpoint",t->test_setpoint);
    solua_set_num(L,item,"tune_drive",t->drive);
    solua_set_num(L,item,"tune_breakaway",t->breakaway);
    solua_set_num(L,item,"tune_noise",t->noise);
    solua_set_num(L,item,"tune_process_gain",t->process_gain);
    solua_set_num(L,item,"tune_dead_time",t->dead_time);
    solua_set_num(L,item,"tune_time_constant",t->time_constant);
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
    solua_set_num(L, item, "output", info->state.output+info->feedforward);
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
static int solua_controllers_tune(lua_State *L)
{
    lua_Number cycles=luaL_optnumber(L,3,4), timeout=luaL_optnumber(L,4,120000);
    if(!isfinite(cycles) || cycles<3 || cycles>12 || floor(cycles)!=cycles ||
        !isfinite(timeout) || timeout<1000 || timeout>300000 || floor(timeout)!=timeout)
        return luaL_error(L,"invalid tuning point count or timeout");
    (void)solua_check_esp(L,solar_os_controller_tune(luaL_checkstring(L,1),
        (float)luaL_optnumber(L,2,0),(uint32_t)cycles,(uint32_t)timeout));
    return solua_controllers_status(L);
}
static int solua_controllers_delete(lua_State *L)
{ return solua_check_esp(L, solar_os_controller_delete(luaL_checkstring(L, 1))); }
#endif
