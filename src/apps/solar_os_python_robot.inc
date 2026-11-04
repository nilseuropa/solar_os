#if SOLAR_OS_PACKAGE_SERVICE_ROBOT
static mp_obj_t python_joint_info(const solar_os_robot_joint_info_v3_t *s)
{
    mp_obj_t d=mp_obj_new_dict(0);
    python_dict_store_cstr(d,"name",s->config.name);
    python_dict_store_cstr(d,"kind",s->config.kind==SOLAR_OS_ROBOT_MOTOR_VELOCITY?"velocity":"servo");
    python_dict_store_cstr(d,"state",s->state==SOLAR_OS_ROBOT_ACTIVE?"active":s->state==SOLAR_OS_ROBOT_WAITING?"waiting":s->state==SOLAR_OS_ROBOT_FAULT?"fault":"ready");
    python_dict_store_cstr(d,"motor",s->config.motor);
    python_dict_store_cstr(d,"encoder",s->config.encoder);
    python_dict_store_float(d,"ppr",s->config.ppr);
    python_dict_store_float(d,"encoder_counts_per_rev",s->encoder_counts_per_rev);
    python_dict_store_float(d,"gear_ratio",s->config.gear_ratio);
    python_dict_store_float(d,"position_zero",s->config.position_zero);
    python_dict_store_float(d,"position_min",s->config.position_min);
    python_dict_store_float(d,"position_max",s->config.position_max);
    python_dict_store_float(d,"velocity_limit",s->config.velocity_limit);
    python_dict_store_float(d,"acceleration_limit",s->config.acceleration_limit);
    python_dict_store_float(d,"max_drive",s->config.max_drive);
    python_dict_store_float(d,"velocity_tau",s->config.velocity_tau);
    python_dict_store_float(d,"friction_bias_forward",s->config.friction_bias_forward);
    python_dict_store_float(d,"friction_bias_reverse",s->config.friction_bias_reverse);
    python_dict_store_float(d,"velocity_deadband",s->config.velocity_deadband);
    python_dict_store_float(d,"kp",s->config.kp);
    python_dict_store_float(d,"ki",s->config.ki);
    python_dict_store_float(d,"kd",s->config.kd);
    python_dict_store_float(d,"derivative_tau",s->config.derivative_tau);
    python_dict_store_int(d,"motor_polarity",s->config.motor_polarity);
    python_dict_store_int(d,"encoder_polarity",s->config.encoder_polarity);
    python_dict_store_int(d,"encoder_on_output",s->config.encoder_on_output);
    python_dict_store_int(d,"period_ms",s->config.period_ms);
    python_dict_store_int(d,"command_timeout_ms",s->config.command_timeout_ms);
    python_dict_store_int(d,"feedback_timeout_ms",s->config.feedback_timeout_ms);
    python_dict_store_int(d,"max_gap_ms",s->config.max_gap_ms);
    python_dict_store_int(d,"brake",s->config.brake);
    python_dict_store_int(d,"claimed",s->claimed);
    python_dict_store_int(d,"command_valid",s->command_valid);
    python_dict_store_int(d,"position_valid",s->position_valid);
    python_dict_store_int(d,"velocity_valid",s->velocity_valid);
    python_dict_store_int(d,"referenced",s->referenced);
    python_dict_store_int(d,"decoding",s->decoding);
    python_dict_store_int(d,"saturated",s->saturated);
    python_dict_store_int(d,"steps",s->steps);
    python_dict_store_int(d,"faults",s->faults);
    python_dict_store_int(d,"raw_count",s->raw_count);
    python_dict_store_int(d,"last_error",s->last_error);
    python_dict_store_float(d,"target",s->target);
    python_dict_store_float(d,"setpoint",s->setpoint);
    python_dict_store_float(d,"position",s->position);
    python_dict_store_float(d,"velocity",s->velocity);
    python_dict_store_float(d,"commanded_position",s->commanded_position);
    python_dict_store_float(d,"output",s->output);
    python_dict_store_float(d,"feedforward",s->feedforward);
    python_dict_store_float(d,"p",s->p);
    python_dict_store_float(d,"i",s->i);
    python_dict_store_float(d,"d",s->d);
    mp_obj_dict_store(d,python_key("sample_us"),python_u64_to_obj(s->sample_us));
    mp_obj_dict_store(d,python_key("command_expires_us"),python_u64_to_obj(s->command_expires_us));
    return d;
}
static mp_obj_t solaros_robot_joints(void)
{
    mp_obj_t list=mp_obj_new_list(0,NULL);
    for (size_t i=0;i<solar_os_robot_joint_count();i++) {
        solar_os_robot_joint_info_v3_t info;
        if (solar_os_robot_joint_get_info(i,&info)) mp_obj_list_append(list,python_joint_info(&info));
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_robot_joints_obj,solaros_robot_joints);
static mp_obj_t solaros_robot_joint_status(mp_obj_t name)
{
    solar_os_robot_joint_info_v3_t info;
    python_check_esp(solar_os_robot_joint_status(mp_obj_str_get_str(name),&info));
    return python_joint_info(&info);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_robot_joint_status_obj,solaros_robot_joint_status);
static mp_obj_t solaros_robot_joint_create(size_t n_args,const mp_obj_t *args)
{
    const char *kind=n_args>1?mp_obj_str_get_str(args[1]):"velocity";
    if (strcmp(kind,"velocity") && strcmp(kind,"servo")) mp_raise_ValueError(MP_ERROR_TEXT("expected velocity or servo"));
    python_check_esp(solar_os_robot_joint_create(mp_obj_str_get_str(args[0]),
        strcmp(kind,"velocity")==0?SOLAR_OS_ROBOT_MOTOR_VELOCITY:SOLAR_OS_ROBOT_SERVO_POSITION,"python"));
    return solaros_robot_joint_status(args[0]);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_robot_joint_create_obj,1,2,solaros_robot_joint_create);
static mp_obj_t solaros_robot_joint_configure(mp_obj_t name,mp_obj_t key,mp_obj_t value)
{
    python_check_esp(solar_os_robot_joint_configure(mp_obj_str_get_str(name),mp_obj_str_get_str(key),(float)mp_obj_get_float(value)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_robot_joint_configure_obj,solaros_robot_joint_configure);
static mp_obj_t solaros_robot_joint_bind_motor(mp_obj_t name,mp_obj_t motor,mp_obj_t encoder)
{
    python_check_esp(solar_os_robot_joint_bind_motor(mp_obj_str_get_str(name),mp_obj_str_get_str(motor),mp_obj_str_get_str(encoder)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_robot_joint_bind_motor_obj,solaros_robot_joint_bind_motor);
static mp_obj_t solaros_robot_joint_bind_servo(size_t n_args,const mp_obj_t *args)
{
    const char *provider=mp_obj_str_get_str(args[1]);
    if (strlen(provider)>=20) mp_raise_ValueError(MP_ERROR_TEXT("provider is too long"));
    solar_os_native_servo_config_v1_t c={
        .output=python_u32_from_obj(args[2]),.pulse_min_us=python_u32_from_obj(args[3]),
        .pulse_max_us=python_u32_from_obj(args[4]),.angle_min=(float)mp_obj_get_float(args[5]),
        .angle_max=(float)mp_obj_get_float(args[6]),.frequency_hz=n_args>7?python_u32_from_obj(args[7]):50,
    };
    strlcpy(c.provider,provider,sizeof(c.provider));
    python_check_esp(solar_os_robot_joint_bind_servo(mp_obj_str_get_str(args[0]),&c));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_robot_joint_bind_servo_obj,7,8,solaros_robot_joint_bind_servo);
static mp_obj_t solaros_robot_joint_command(mp_obj_t name,mp_obj_t target)
{
    python_check_esp(solar_os_robot_joint_command(mp_obj_str_get_str(name),(float)mp_obj_get_float(target)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_robot_joint_command_obj,solaros_robot_joint_command);
static mp_obj_t solaros_robot_joint_enable(mp_obj_t name)
{
    python_check_esp(solar_os_robot_joint_enable(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_robot_joint_enable_obj,solaros_robot_joint_enable);
static mp_obj_t solaros_robot_joint_disable(mp_obj_t name)
{
    python_check_esp(solar_os_robot_joint_disable(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_robot_joint_disable_obj,solaros_robot_joint_disable);
static mp_obj_t solaros_robot_joint_close(mp_obj_t name)
{
    python_check_esp(solar_os_robot_joint_close(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_robot_joint_close_obj,solaros_robot_joint_close);
static void python_robot_destroy(void)
{
    esp_err_t err=solar_os_robot_release_owner("python");
    if (err!=ESP_OK) SOLAR_OS_LOGW(TAG,"Robot cleanup retained resources: %s",esp_err_to_name(err));
}

/* The resident description is device-wide and explicitly unloaded. Script
 * teardown releases its hardware joints, not a model shared with the shell. */
static mp_obj_t solaros_robot_model_info(void)
{
    solar_os_robot_model_info_t info;
    python_check_esp(solar_os_robot_model_get_info(&info));
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_cstr(d, "name", info.name);
    python_dict_store_cstr(d, "root", info.root);
    python_dict_store_int(d, "known_joints", info.known_joints);
    python_dict_store_int(d, "extension_bytes", info.extension_bytes);
    mp_obj_t links = mp_obj_new_list(0, NULL), joints = mp_obj_new_list(0, NULL);
    for (uint32_t i = 0; i < info.links; ++i) {
        char name[SOLAR_OS_ROBOT_MODEL_NAME_MAX];
        python_check_esp(solar_os_robot_model_get_link(i, name, sizeof(name)));
        mp_obj_list_append(links, mp_obj_new_str(name, strlen(name)));
    }
    for (uint32_t i = 0; i < info.joints; ++i) {
        solar_os_robot_model_joint_info_t j;
        python_check_esp(solar_os_robot_model_get_joint(i, &j));
        mp_obj_t item = mp_obj_new_dict(0);
        python_dict_store_cstr(item, "name", j.name);
        python_dict_store_cstr(item, "parent", j.parent);
        python_dict_store_cstr(item, "child", j.child);
        python_dict_store_int(item, "kind", j.kind);
        python_dict_store_int(item, "position_known", j.position_known);
        python_dict_store_float(item, "position", j.position);
        mp_obj_list_append(joints, item);
    }
    mp_obj_dict_store(d, python_key("links"), links);
    mp_obj_dict_store(d, python_key("joints"), joints);
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_robot_model_info_obj, solaros_robot_model_info);
static mp_obj_t solaros_robot_model_load(mp_obj_t path)
{
    python_check_esp(solar_os_robot_model_load_file(mp_obj_str_get_str(path), NULL, 0));
    return solaros_robot_model_info();
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_robot_model_load_obj, solaros_robot_model_load);
static mp_obj_t solaros_robot_model_unload(void)
{
    python_check_esp(solar_os_robot_model_unload()); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_robot_model_unload_obj, solaros_robot_model_unload);
static mp_obj_t solaros_robot_model_set_joint(mp_obj_t name, mp_obj_t position)
{
    const char *s = mp_obj_str_get_str(name); float value = (float)mp_obj_get_float(position);
    python_check_esp(solar_os_robot_model_update(&s, &value, 1)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_robot_model_set_joint_obj, solaros_robot_model_set_joint);
static mp_obj_t python_robot_float_list(const float *values, unsigned count)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (unsigned i = 0; i < count; ++i) mp_obj_list_append(list, mp_obj_new_float(values[i]));
    return list;
}
static mp_obj_t solaros_robot_model_transform(mp_obj_t target, mp_obj_t source)
{
    solar_os_robot_transform_t t;
    python_check_esp(solar_os_robot_model_transform(mp_obj_str_get_str(target), mp_obj_str_get_str(source), &t));
    mp_obj_t d = mp_obj_new_dict(0);
    mp_obj_dict_store(d, python_key("translation"), python_robot_float_list(t.translation, 3));
    mp_obj_dict_store(d, python_key("rotation"), python_robot_float_list(t.rotation, 4));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_robot_model_transform_obj, solaros_robot_model_transform);
static mp_obj_t python_robot_model_apply(const mp_obj_t *args, bool direction)
{
    float in[3], out[3];
    for (unsigned i = 0; i < 3; ++i) in[i] = (float)mp_obj_get_float(args[2+i]);
    python_check_esp(solar_os_robot_model_apply(mp_obj_str_get_str(args[0]), mp_obj_str_get_str(args[1]), in, direction, out));
    return python_robot_float_list(out, 3);
}
static mp_obj_t solaros_robot_model_point(size_t n_args, const mp_obj_t *args)
{ (void)n_args; return python_robot_model_apply(args, false); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_robot_model_point_obj, 5, 5, solaros_robot_model_point);
static mp_obj_t solaros_robot_model_vector(size_t n_args, const mp_obj_t *args)
{ (void)n_args; return python_robot_model_apply(args, true); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_robot_model_vector_obj, 5, 5, solaros_robot_model_vector);
#else
static void python_robot_destroy(void) {}
#endif
