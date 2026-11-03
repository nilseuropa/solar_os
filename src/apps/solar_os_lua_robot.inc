#if SOLAR_OS_PACKAGE_SERVICE_ROBOT
static void solua_joint_info(lua_State *L,const solar_os_robot_joint_info_v3_t *s)
{
    lua_newtable(L); int d=lua_gettop(L);
    solua_set_str(L,d,"name",s->config.name);
    solua_set_str(L,d,"kind",s->config.kind==SOLAR_OS_ROBOT_MOTOR_VELOCITY?"velocity":"servo");
    solua_set_str(L,d,"state",s->state==SOLAR_OS_ROBOT_ACTIVE?"active":s->state==SOLAR_OS_ROBOT_WAITING?"waiting":s->state==SOLAR_OS_ROBOT_FAULT?"fault":"ready");
    solua_set_str(L,d,"motor",s->config.motor);
    solua_set_str(L,d,"encoder",s->config.encoder);
    solua_set_num(L,d,"ppr",s->config.ppr);
    solua_set_num(L,d,"encoder_counts_per_rev",s->encoder_counts_per_rev);
    solua_set_num(L,d,"gear_ratio",s->config.gear_ratio);
    solua_set_num(L,d,"position_zero",s->config.position_zero);
    solua_set_num(L,d,"position_min",s->config.position_min);
    solua_set_num(L,d,"position_max",s->config.position_max);
    solua_set_num(L,d,"velocity_limit",s->config.velocity_limit);
    solua_set_num(L,d,"acceleration_limit",s->config.acceleration_limit);
    solua_set_num(L,d,"max_drive",s->config.max_drive);
    solua_set_num(L,d,"velocity_tau",s->config.velocity_tau);
    solua_set_num(L,d,"friction_bias_forward",s->config.friction_bias_forward);
    solua_set_num(L,d,"friction_bias_reverse",s->config.friction_bias_reverse);
    solua_set_num(L,d,"velocity_deadband",s->config.velocity_deadband);
    solua_set_num(L,d,"kp",s->config.kp);
    solua_set_num(L,d,"ki",s->config.ki);
    solua_set_num(L,d,"kd",s->config.kd);
    solua_set_num(L,d,"derivative_tau",s->config.derivative_tau);
    solua_set_int(L,d,"motor_polarity",s->config.motor_polarity);
    solua_set_int(L,d,"encoder_polarity",s->config.encoder_polarity);
    solua_set_int(L,d,"encoder_on_output",s->config.encoder_on_output);
    solua_set_int(L,d,"period_ms",s->config.period_ms);
    solua_set_int(L,d,"command_timeout_ms",s->config.command_timeout_ms);
    solua_set_int(L,d,"feedback_timeout_ms",s->config.feedback_timeout_ms);
    solua_set_int(L,d,"max_gap_ms",s->config.max_gap_ms);
    solua_set_int(L,d,"brake",s->config.brake);
    solua_set_int(L,d,"claimed",s->claimed);
    solua_set_int(L,d,"command_valid",s->command_valid);
    solua_set_int(L,d,"position_valid",s->position_valid);
    solua_set_int(L,d,"velocity_valid",s->velocity_valid);
    solua_set_int(L,d,"referenced",s->referenced);
    solua_set_int(L,d,"decoding",s->decoding);
    solua_set_int(L,d,"saturated",s->saturated);
    solua_set_int(L,d,"steps",s->steps);
    solua_set_int(L,d,"faults",s->faults);
    solua_set_int(L,d,"raw_count",s->raw_count);
    solua_set_int(L,d,"last_error",s->last_error);
    solua_set_num(L,d,"target",s->target);
    solua_set_num(L,d,"setpoint",s->setpoint);
    solua_set_num(L,d,"position",s->position);
    solua_set_num(L,d,"velocity",s->velocity);
    solua_set_num(L,d,"commanded_position",s->commanded_position);
    solua_set_num(L,d,"output",s->output);
    solua_set_num(L,d,"feedforward",s->feedforward);
    solua_set_num(L,d,"p",s->p);
    solua_set_num(L,d,"i",s->i);
    solua_set_num(L,d,"d",s->d);
    solua_set_int(L,d,"sample_us",s->sample_us);
    solua_set_int(L,d,"command_expires_us",s->command_expires_us);
}
static int solua_robot_joints(lua_State *L)
{
    lua_newtable(L); int list=lua_gettop(L),out=1;
    for (size_t i=0;i<solar_os_robot_joint_count();i++) {
        solar_os_robot_joint_info_v3_t info;
        if (solar_os_robot_joint_get_info(i,&info)) { solua_joint_info(L,&info); lua_rawseti(L,list,out++); }
    }
    return 1;
}
static int solua_robot_joint_status(lua_State *L)
{
    solar_os_robot_joint_info_v3_t info;
    (void)solua_check_esp(L,solar_os_robot_joint_status(luaL_checkstring(L,1),&info));
    solua_joint_info(L,&info); return 1;
}
static int solua_robot_joint_create(lua_State *L)
{
    const char *kind=luaL_optstring(L,2,"velocity");
    if (strcmp(kind,"velocity") && strcmp(kind,"servo")) return luaL_error(L,"expected velocity or servo");
    (void)solua_check_esp(L,solar_os_robot_joint_create(luaL_checkstring(L,1),
        strcmp(kind,"velocity")==0?SOLAR_OS_ROBOT_MOTOR_VELOCITY:SOLAR_OS_ROBOT_SERVO_POSITION,"lua"));
    return solua_robot_joint_status(L);
}
static int solua_robot_joint_configure(lua_State *L)
{ return solua_check_esp(L,solar_os_robot_joint_configure(luaL_checkstring(L,1),luaL_checkstring(L,2),(float)luaL_checknumber(L,3))); }
static int solua_robot_joint_bind_motor(lua_State *L)
{ return solua_check_esp(L,solar_os_robot_joint_bind_motor(luaL_checkstring(L,1),luaL_checkstring(L,2),luaL_checkstring(L,3))); }
static int solua_robot_joint_bind_servo(lua_State *L)
{
    const char *provider=luaL_checkstring(L,2);
    if (strlen(provider)>=20) return luaL_error(L,"provider is too long");
    solar_os_native_servo_config_v1_t c={
        .output=solua_optional_u32(L,3,0),.pulse_min_us=solua_optional_u32(L,4,0),
        .pulse_max_us=solua_optional_u32(L,5,0),.angle_min=(float)luaL_checknumber(L,6),
        .angle_max=(float)luaL_checknumber(L,7),.frequency_hz=solua_optional_u32(L,8,50),
    };
    strlcpy(c.provider,provider,sizeof(c.provider));
    return solua_check_esp(L,solar_os_robot_joint_bind_servo(luaL_checkstring(L,1),&c));
}
static int solua_robot_joint_command(lua_State *L)
{ return solua_check_esp(L,solar_os_robot_joint_command(luaL_checkstring(L,1),(float)luaL_checknumber(L,2))); }
static int solua_robot_joint_enable(lua_State *L)
{ return solua_check_esp(L,solar_os_robot_joint_enable(luaL_checkstring(L,1))); }
static int solua_robot_joint_disable(lua_State *L)
{ return solua_check_esp(L,solar_os_robot_joint_disable(luaL_checkstring(L,1))); }
static int solua_robot_joint_close(lua_State *L)
{ return solua_check_esp(L,solar_os_robot_joint_close(luaL_checkstring(L,1))); }
static void solua_robot_destroy(void)
{
    esp_err_t err=solar_os_robot_release_owner("lua");
    if (err!=ESP_OK) SOLAR_OS_LOGW(TAG,"Robot cleanup retained resources: %s",esp_err_to_name(err));
}
#else
static void solua_robot_destroy(void) {}
#endif
