#if SOLAR_OS_PACKAGE_SERVICE_CONTROLLERS
static mp_obj_t python_controller_info(const solar_os_controller_info_t *info)
{
    mp_obj_t dict = mp_obj_new_dict(0);
    python_dict_store_cstr(dict, "name", info->config.name);
    python_dict_store_cstr(dict, "owner", info->owner);
    python_dict_store_bool(dict, "waiting", info->waiting);
    python_dict_store_float(dict, "feedforward", info->feedforward);
    python_dict_store_uint(dict,"command_timeout_ms",info->config.command_timeout_ms);
    python_dict_store_float(dict,"acceleration_limit",info->config.acceleration_limit);
    python_dict_store_float(dict,"velocity_deadband",info->config.velocity_deadband);
    python_dict_store_float(dict,"friction_bias_forward",info->config.friction_bias_forward);
    python_dict_store_float(dict,"friction_bias_reverse",info->config.friction_bias_reverse);
    python_dict_store_float(dict,"active_setpoint",info->active_setpoint);
    python_dict_store_bool(dict,"command_valid",info->command_valid);
    const solar_os_controller_tune_result_t *t=&info->tuning;
    python_dict_store_cstr(dict,"tune_state",solar_os_controller_tune_state_name(t->state));
    python_dict_store_cstr(dict,"tune_reason",solar_os_controller_tune_reason_name(t->reason));
    python_dict_store_uint(dict,"tune_cycles",t->cycles);
    python_dict_store_uint(dict,"tune_required_cycles",t->required_cycles);
    python_dict_store_float(dict,"tune_elapsed",t->elapsed);
    python_dict_store_float(dict,"tune_period",t->period);
    python_dict_store_float(dict,"tune_amplitude",t->amplitude);
    python_dict_store_float(dict,"tune_ultimate_gain",t->ultimate_gain);
    python_dict_store_float(dict,"tune_kp",t->kp);
    python_dict_store_float(dict,"tune_ki",t->ki);
    python_dict_store_float(dict,"tune_kd",t->kd);
    python_dict_store_float(dict,"tune_tau",t->derivative_tau);
    python_dict_store_cstr(dict, "kind", info->config.algorithm.kind == SOLAR_OS_CONTROLLER_PID ? "pid" : "hysteresis");
    python_dict_store_cstr(dict, "source", info->config.source);
    python_dict_store_cstr(dict, "target", info->config.target);
    python_dict_store_cstr(dict, "last_error_name", esp_err_to_name(info->last_error));
    python_dict_store_float(dict, "setpoint", info->config.setpoint);
    python_dict_store_float(dict, "setpoint_min", info->config.setpoint_min);
    python_dict_store_float(dict, "setpoint_max", info->config.setpoint_max);
    python_dict_store_float(dict, "safe_output", info->config.safe_output);
    python_dict_store_uint(dict, "period_ms", info->config.period_ms);
    python_dict_store_uint(dict, "timeout_ms", info->config.timeout_ms);
    python_dict_store_uint(dict, "frequency_hz", info->config.frequency_hz);
    python_dict_store_float(dict, "kp", info->config.algorithm.kp);
    python_dict_store_float(dict, "ki", info->config.algorithm.ki);
    python_dict_store_float(dict, "kd", info->config.algorithm.kd);
    python_dict_store_float(dict, "derivative_tau", info->config.algorithm.derivative_tau);
    python_dict_store_float(dict, "output_min", info->config.algorithm.output_min);
    python_dict_store_float(dict, "output_max", info->config.algorithm.output_max);
    python_dict_store_float(dict, "hysteresis", info->config.algorithm.hysteresis);
    python_dict_store_float(dict, "max_dt", info->config.algorithm.max_dt);
    python_dict_store_bool(dict, "reverse", info->config.algorithm.reverse);
    python_dict_store_bool(dict, "enabled", info->enabled);
    python_dict_store_bool(dict, "claimed", info->claimed);
    python_dict_store_bool(dict, "faulted", info->faulted);
    python_dict_store_bool(dict, "has_measurement", info->has_measurement);
    python_dict_store_bool(dict, "automatic", info->state.automatic);
    python_dict_store_bool(dict, "saturated", info->state.saturated);
    python_dict_store_float(dict, "error", info->state.error);
    python_dict_store_float(dict, "p", info->state.p);
    python_dict_store_float(dict, "i", info->state.i);
    python_dict_store_float(dict, "d", info->state.d);
    python_dict_store_float(dict, "output", info->state.output+info->feedforward);
    python_dict_store_float(dict, "measurement", info->measurement);
    mp_obj_dict_store(dict, python_key("sample_ms"), python_u64_to_obj(info->sample_ms));
    python_dict_store_uint(dict, "steps", info->steps);
    python_dict_store_uint(dict, "faults", info->faults);
    python_dict_store_int(dict, "last_error", info->last_error);
    return dict;
}
static mp_obj_t solaros_controllers_status(mp_obj_t name)
{
    solar_os_controller_info_t info;
    python_check_esp(solar_os_controller_find(mp_obj_str_get_str(name), &info));
    return python_controller_info(&info);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_controllers_status_obj, solaros_controllers_status);
static mp_obj_t solaros_controllers_list(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < solar_os_controller_count(); i++) {
        solar_os_controller_info_t info;
        if (solar_os_controller_get_info(i, &info)) mp_obj_list_append(list, python_controller_info(&info));
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_controllers_list_obj, solaros_controllers_list);
static mp_obj_t solaros_controllers_create(size_t n_args, const mp_obj_t *args)
{
    const char *kind = n_args > 1 ? mp_obj_str_get_str(args[1]) : "pid";
    if (strcmp(kind, "pid") != 0 && strcmp(kind, "hysteresis") != 0)
        mp_raise_ValueError(MP_ERROR_TEXT("expected pid or hysteresis"));
    python_check_esp(solar_os_controller_create(mp_obj_str_get_str(args[0]),
        strcmp(kind, "pid") == 0 ? SOLAR_OS_CONTROLLER_PID : SOLAR_OS_CONTROLLER_HYSTERESIS));
    return solaros_controllers_status(args[0]);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_controllers_create_obj, 1, 2, solaros_controllers_create);
static mp_obj_t solaros_controllers_configure(mp_obj_t name, mp_obj_t key, mp_obj_t value)
{
    python_check_esp(solar_os_controller_configure(mp_obj_str_get_str(name),
        mp_obj_str_get_str(key), (float)mp_obj_get_float(value)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_controllers_configure_obj, solaros_controllers_configure);
static mp_obj_t solaros_controllers_bind(mp_obj_t name, mp_obj_t source, mp_obj_t target)
{
    python_check_esp(solar_os_controller_bind(mp_obj_str_get_str(name),
        mp_obj_str_get_str(source), mp_obj_str_get_str(target)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_controllers_bind_obj, solaros_controllers_bind);
static mp_obj_t solaros_controllers_feed(mp_obj_t name, mp_obj_t value)
{
    python_check_esp(solar_os_controller_feed(mp_obj_str_get_str(name),
        (float)mp_obj_get_float(value), solar_os_time_uptime_ms()));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_controllers_feed_obj, solaros_controllers_feed);
static mp_obj_t solaros_controllers_step(mp_obj_t name, mp_obj_t measurement, mp_obj_t dt)
{
    float output;
    python_check_esp(solar_os_controller_step(mp_obj_str_get_str(name),
        (float)mp_obj_get_float(measurement), (float)mp_obj_get_float(dt), &output));
    return mp_obj_new_float(output);
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_controllers_step_obj, solaros_controllers_step);
static mp_obj_t solaros_controllers_manual(mp_obj_t name, mp_obj_t output)
{
    python_check_esp(solar_os_controller_manual(mp_obj_str_get_str(name), (float)mp_obj_get_float(output)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_controllers_manual_obj, solaros_controllers_manual);
static mp_obj_t solaros_controllers_auto(mp_obj_t name)
{
    python_check_esp(solar_os_controller_auto(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_controllers_auto_obj, solaros_controllers_auto);
static mp_obj_t solaros_controllers_reset(mp_obj_t name)
{
    python_check_esp(solar_os_controller_reset(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_controllers_reset_obj, solaros_controllers_reset);
static mp_obj_t solaros_controllers_start(mp_obj_t name)
{
    python_check_esp(solar_os_controller_start(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_controllers_start_obj, solaros_controllers_start);
static mp_obj_t solaros_controllers_stop(mp_obj_t name)
{
    python_check_esp(solar_os_controller_stop(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_controllers_stop_obj, solaros_controllers_stop);
static mp_obj_t solaros_controllers_tune(size_t n_args, const mp_obj_t *args)
{
    float band=n_args>1?(float)mp_obj_get_float(args[1]):0;
    float cycles=n_args>2?(float)mp_obj_get_float(args[2]):6;
    float timeout=n_args>3?(float)mp_obj_get_float(args[3]):30000;
    if(!isfinite(cycles) || cycles<3 || cycles>12 || (float)(uint32_t)cycles!=cycles ||
        !isfinite(timeout) || timeout<1000 || timeout>300000 || (float)(uint32_t)timeout!=timeout)
        python_check_esp(ESP_ERR_INVALID_ARG);
    python_check_esp(solar_os_controller_tune(mp_obj_str_get_str(args[0]),band,(uint32_t)cycles,(uint32_t)timeout));
    return solaros_controllers_status(args[0]);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_controllers_tune_obj,1,4,solaros_controllers_tune);
static mp_obj_t solaros_controllers_delete(mp_obj_t name)
{
    python_check_esp(solar_os_controller_delete(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_controllers_delete_obj, solaros_controllers_delete);
#endif
