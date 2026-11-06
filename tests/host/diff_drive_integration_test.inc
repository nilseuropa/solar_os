typedef struct { int32_t count;float output;bool fail,stop_fail; } drive_hardware_t;
#include <unistd.h>
static drive_hardware_t drive_hw[2];
static esp_err_t drive_hw_status(void *ctx,solar_os_motor_status_t *s)
{ drive_hardware_t *h=ctx;*s=(solar_os_motor_status_t){.drive_valid=true,.drive=h->output};return ESP_OK; }
static esp_err_t drive_hw_write(void *ctx,float value)
{ ((drive_hardware_t *)ctx)->output=value;return ESP_OK; }
static esp_err_t drive_hw_stop(void *ctx)
{ drive_hardware_t *h=ctx;if(h->stop_fail)return ESP_FAIL;h->output=0;return ESP_OK; }
static esp_err_t drive_hw_read(void *ctx,int32_t *value)
{ drive_hardware_t *h=ctx;*value=h->count;return h->fail?ESP_FAIL:ESP_OK; }
static const char *drive_xml=
    "<robot name='drive_test'><link name='base'/><link name='wl'/><link name='wr'/>"
    "<joint name='dl_joint' type='continuous'><parent link='base'/><child link='wl'/></joint>"
    "<joint name='dr_joint' type='continuous'><parent link='base'/><child link='wr'/></joint>"
    "<solar_os version='1'>"
    "<joint name='dl_joint' command='velocity'><motor device='dl'/><encoder device='el' ppr='1000' placement='output'/>"
    "<transmission gear_ratio='1'/><param name='velocity_limit' value='4'/></joint>"
    "<joint name='dr_joint' command='velocity'><motor device='dr'/><encoder device='er' ppr='1000' placement='output'/>"
    "<transmission gear_ratio='1'/><param name='velocity_limit' value='4'/></joint>"
    "<diff_drive name='base' left_joint='dl_joint' right_joint='dr_joint'>"
    "<geometry wheel_radius='.1' wheel_separation='.5'/><odometry frame='odom' base_frame='base'/>"
    "<param name='command_timeout_ms' value='100'/></diff_drive></solar_os></robot>";
static joint_t *drive_joint(const char *name)
{
    for(unsigned i=0;i<SOLAR_OS_ROBOT_JOINT_MAX;++i) if(joints[i] && equal(joints[i]->info.config.name,name))return joints[i];
    assert(false);return NULL;
}
static void drive_tick(void)
{
    time_value+=20000;lock();joint_t *l=drive_joint("dl_joint"),*r=drive_joint("dr_joint");
    drive_cycle(l);if(l->info.state!=SOLAR_OS_ROBOT_FAULT)cycle(l);
    if(r->info.state!=SOLAR_OS_ROBOT_FAULT)cycle(r);
    drive_notify(l);drive_notify(r);
    unlock();
}
static solar_os_robot_diff_drive_status_t drive_info(void)
{
    solar_os_robot_diff_drive_status_t s;
    assert(solar_os_robot_diff_drive_status("base",&s)==ESP_OK);return s;
}
static esp_err_t drive_collision_sample(void *user,const solar_os_stream_read_options_t *options,solar_os_stream_scalar_sample_t *sample)
{ (void)user;(void)options;*sample=(solar_os_stream_scalar_sample_t){.uptime_ms=time_value/1000};return ESP_OK; }
static void (*drive_binding_check)(void);
static void diff_drive_integration_test(void)
{
    const solar_os_motor_ops_t mo={.status=drive_hw_status,.drive=drive_hw_write,.coast=drive_hw_stop};
    const solar_os_encoder_ops_t eo={.read=drive_hw_read};
    for(unsigned i=0;i<2;++i) {
        solar_os_motor_registration_t m={.name=i?"dr":"dl",.driver="test",.ops=&mo,.ctx=&drive_hw[i]};
        solar_os_encoder_registration_t e={.name=i?"er":"el",.driver="test",.ops=&eo,.ctx=&drive_hw[i],.mode=SOLAR_OS_ENCODER_MODE_X1};
        assert(solar_os_motor_register(&m)==ESP_OK && solar_os_encoder_register(&e)==ESP_OK);
    }
    char error[128];
    assert(solar_os_robot_model_load_xml(drive_xml,strlen(drive_xml),error,sizeof(error))==ESP_OK);
    assert(!solar_os_robot_joint_count());
    solar_os_stream_driver_t collision={.info={.id="diff_drive.base.odom_x",.provider="foreign",.format="f32",
        .type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_SOURCE,.sharing=SOLAR_OS_STREAM_SHARING_SHARED},
        .read_scalar_sample=drive_collision_sample};
    assert(solar_os_stream_register(&collision)==ESP_OK);
    assert(solar_os_robot_start()==ESP_ERR_INVALID_STATE && !solar_os_robot_joint_count());
    solar_os_stream_info_t collision_info;
    assert(solar_os_stream_get_info(collision.info.id,&collision_info)==ESP_OK && !strcmp(collision_info.provider,"foreign"));
    assert(solar_os_stream_unregister(collision.info.id)==ESP_OK);
    assert(solar_os_robot_start()==ESP_OK && solar_os_robot_joint_count()==2);
    assert(drive_info().state==SOLAR_OS_ROBOT_WAITING);
    assert(solar_os_robot_joint_command("dl_joint",1)==ESP_ERR_INVALID_STATE);
    float radius;assert(solar_os_parameter_get("diff_drive.base.wheel_radius",&radius)==ESP_OK && radius==.1f);
    assert(solar_os_parameter_set("diff_drive.base.wheel_radius",.2f)==ESP_ERR_INVALID_STATE);
    assert(solar_os_parameter_claim("diff_drive.base.wheel_radius","foreign")==ESP_ERR_INVALID_STATE);
    drive_tick();drive_tick();assert(drive_info().odometry_valid);
    /* A generic controller must be able to consume odometry and write a
     * body-command stream while the robot worker holds its provider mutex.
     * This reproduces the lock ordering that previously could deadlock. */
    assert(solar_os_robot_diff_drive_command("base",0,0)==ESP_OK);
    assert(solar_os_controller_create("outer",SOLAR_OS_CONTROLLER_PID)==ESP_OK);
    assert(solar_os_controller_configure("outer","output_max",.3f)==ESP_OK);
    assert(solar_os_controller_configure("outer","setpoint",.1f)==ESP_OK);
    assert(solar_os_controller_bind("outer","diff_drive.base.odom_x","stream:diff_drive.base.cmd_linear")==ESP_OK);
    assert(solar_os_controller_start("outer")==ESP_OK);
    time_value+=20000;alarm(3);lock();solar_os_controllers_tick(time_value/1000);unlock();alarm(0);
    solar_os_controller_info_t outer;
    assert(solar_os_controller_find("outer",&outer)==ESP_OK && outer.steps==1 && !outer.faulted);
    assert(solar_os_controller_stop("outer")==ESP_OK && solar_os_controller_delete("outer")==ESP_OK);
    if(drive_binding_check)drive_binding_check();
    assert(solar_os_robot_diff_drive_command("base",.2f,.4f)==ESP_OK);
    drive_tick();solar_os_robot_diff_drive_status_t s=drive_info();
    assert(fabsf(s.left_command-1)<1e-5 && fabsf(s.right_command-3)<1e-5);
    assert(solar_os_robot_diff_drive_command("base",.4f,1.6f)==ESP_OK);
    drive_tick();s=drive_info();assert(fabsf(s.left_command)<1e-5 && fabsf(s.right_command-4)<1e-5);
    /* Equal wheel increments give straight measured odometry, independent of command. */
    drive_hw[0].count+=10;drive_hw[1].count+=10;drive_tick();drive_tick();s=drive_info();
    assert(fabs(s.x-2*PI*.1*.01)<1e-7 && fabs(s.y)<1e-7 && fabs(s.yaw)<1e-7);
    double x=s.x;
    drive_hw[0].count-=5;drive_hw[1].count+=5;drive_tick();drive_tick();s=drive_info();
    assert(fabs(s.x-x)<1e-7 && fabs(s.yaw-2*PI*.1*.01/.5)<1e-7);
    double start_x=s.x,start_y=s.y,start_yaw=s.yaw;
    drive_hw[0].count+=5;drive_hw[1].count+=10;drive_tick();drive_tick();s=drive_info();
    double angle=2*PI*.1*.005/.5,distance=2*PI*.1*.0075;
    assert(fabs(s.x-start_x-distance/angle*(sin(start_yaw+angle)-sin(start_yaw)))<1e-7);
    assert(fabs(s.y-start_y+distance/angle*(cos(start_yaw+angle)-cos(start_yaw)))<1e-7);
    for(unsigned i=0;i<7;++i)drive_tick();
    assert(drive_info().state==SOLAR_OS_ROBOT_WAITING && !drive_hw[0].output && !drive_hw[1].output);
    solar_os_stream_handle_t input=SOLAR_OS_STREAM_HANDLE_INIT,other=SOLAR_OS_STREAM_HANDLE_INIT,reader=SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_stream_open_options_t sink={.direction=SOLAR_OS_STREAM_DIRECTION_SINK};
    assert(solar_os_stream_open_ex("diff_drive.base.cmd_linear","test",&sink,&input)==ESP_OK);
    assert(solar_os_stream_open_ex("diff_drive.base.cmd_linear","competing",&sink,&other)==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_diff_drive_command("base",0,0)==ESP_ERR_INVALID_STATE);
    float value=.1f;size_t written;
    assert(solar_os_stream_write(&input,&value,sizeof(value),0,&written)==ESP_OK);
    solar_os_stream_info_t command_info;
    assert(solar_os_stream_get_info("diff_drive.base.cmd_linear",&command_info)==ESP_OK);
    assert(command_info.has_scalar_range && command_info.scalar_minimum==-.4f && command_info.scalar_maximum==.4f);
    solar_os_stream_scalar_sample_t before_bad,after_bad;
    assert(solar_os_stream_observe_scalar_sample(input.id,NULL,&before_bad)==ESP_OK);
    time_value+=1000;value=.5f;
    assert(solar_os_stream_write(&input,&value,sizeof(value),0,&written)==ESP_ERR_INVALID_ARG && !written);
    assert(solar_os_stream_observe_scalar_sample(input.id,NULL,&after_bad)==ESP_OK);
    assert(before_bad.value==after_bad.value && before_bad.uptime_ms==after_bad.uptime_ms);
    drive_tick();assert(drive_info().state==SOLAR_OS_ROBOT_WAITING); /* angular is stale. */
    assert(solar_os_stream_open_ex("diff_drive.base.cmd_angular","test",&sink,&other)==ESP_OK);
    value=0;assert(solar_os_stream_write(&other,&value,sizeof(value),0,&written)==ESP_OK);
    drive_tick();assert(drive_info().state==SOLAR_OS_ROBOT_ACTIVE);
    assert(solar_os_stream_close_ex(&input)==ESP_OK && solar_os_stream_close_ex(&other)==ESP_OK);
    assert(solar_os_stream_open("diff_drive.base.odom_x","observer",&reader)==ESP_OK);
    solar_os_stream_scalar_sample_t sample;solar_os_stream_read_options_t options={0};
    assert(solar_os_stream_read_scalar_sample(&reader,&options,&sample)==ESP_OK);
    assert(sample.uptime_ms==drive_info().sample_us/1000 && fabsf(sample.value-(float)drive_info().x)<1e-6f);
    assert(solar_os_robot_stop()==ESP_ERR_INVALID_STATE); /* Observer pins endpoint cleanup, after hardware stops. */
    assert(!drive_hw[0].output && !drive_hw[1].output && !solar_os_robot_joint_count());
    assert(solar_os_robot_model_unload()==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_close_ex(&reader)==ESP_OK && solar_os_robot_stop()==ESP_OK);
    assert(solar_os_robot_model_unload()==ESP_OK);
    drive_hw[0].count=drive_hw[1].count=INT32_MAX-5;
    assert(solar_os_robot_model_load_xml(drive_xml,strlen(drive_xml),error,sizeof(error))==ESP_OK && solar_os_robot_start()==ESP_OK);
    drive_tick();drive_tick();assert(fabs(drive_info().x)<1e-9); /* Restart seeds current counts. */
    for(unsigned i=0;i<2;++i)drive_hw[i].count=(int32_t)((uint32_t)drive_hw[i].count+10);
    drive_tick();drive_tick();assert(fabs(drive_info().x-2*PI*.1*.01)<1e-7);
    assert(solar_os_robot_diff_drive_command("base",.1f,0)==ESP_OK);drive_tick();
    drive_hw[1].fail=true;drive_tick();drive_tick();
    assert(drive_info().state==SOLAR_OS_ROBOT_FAULT && !drive_hw[0].output && !drive_hw[1].output);
    drive_hw[1].fail=false;
    drive_hw[0].stop_fail=true;
    assert(solar_os_robot_stop()!=ESP_OK && solar_os_robot_model_unload()==ESP_ERR_INVALID_STATE);
    drive_hw[0].stop_fail=false;
    assert(solar_os_robot_stop()==ESP_OK && solar_os_robot_model_unload()==ESP_OK);
    for(unsigned i=0;i<2;++i) {
        assert(solar_os_motor_unregister(i?"dr":"dl")==ESP_OK);
        assert(solar_os_encoder_unregister(i?"er":"el")==ESP_OK);
    }
    puts("diff_drive: mixing, limits, measured odometry, timeout, streams, ownership, fault stop and cleanup retry passed");
}
