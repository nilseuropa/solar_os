#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_motor.h"
#include "solar_os_encoder.h"
#include "solar_os_robot.h"
#include "solar_os_native_motion.h"
#include "solar_os_memory.h"
#include "../../modules/robot/main/robot.c"
#include "robot_controller_stubs.inc"
#ifndef ROBOT_MODEL_FIXTURE
#define ROBOT_MODEL_FIXTURE "../fixtures/solbot.urdf"
#endif

static uint64_t time_value=100000;
static int32_t encoder_count;
static float motor_value;
static bool fail_coast, fail_release;
static unsigned device_allocations, provider_allocations, leases, workers;
static uint32_t motor_status_delay_us;
static const solar_os_robot_api_v3_t *provider;

int64_t esp_timer_get_time(void) { return time_value; }
void vTaskDelay(uint32_t ticks) { time_value+=(uint64_t)ticks*1000; }
static void *provider_alloc(size_t n) { void *p=calloc(1,n); if (p) provider_allocations++; return p; }
static void provider_free(void *p) { if(p) { provider_allocations--;free(p); } }
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t cls,const char *tag)
{ (void)cls;(void)tag;void *p=calloc(n,size);if(p)device_allocations++;return p; }
void solar_os_memory_free(void *p) { if(p) { device_allocations--;free(p); } }
void *solar_os_memory_alloc(size_t n,solar_os_memory_class_t cls,const char *tag)
{ return solar_os_memory_calloc(1,n,cls,tag); }
static int native_mutex_create(void **p,uint32_t recursive)
{ (void)recursive;*p=malloc(sizeof(pthread_mutex_t));return *p?pthread_mutex_init(*p,NULL):1; }
static int native_mutex_lock(void *p) { return pthread_mutex_lock(p); }
static int native_mutex_unlock(void *p) { return pthread_mutex_unlock(p); }
static int native_mutex_destroy(void *p) { int e=pthread_mutex_destroy(p);if(!e)free(p);return e; }
static void time_sleep(uint32_t ms) { time_value+=ms*1000; }
static int worker_start(void **p,void(*fn)(void*),void *arg,uint32_t bytes,uint32_t core)
{ (void)fn;(void)arg;(void)bytes;(void)core;*p=malloc(1);if(*p)workers++;return *p?0:1; }
static int worker_join(void *p) { free(p);workers--;return 0; }
int solar_os_native_library_acquire(const char *name,uint32_t abi,uint32_t bytes,uint32_t *lease,const void **api)
{ assert(!strcmp(name,"robot") && abi==SOLAR_OS_ROBOT_ABI &&
    (bytes==sizeof(*provider) || bytes==SOLAR_OS_ROBOT_JOINT_API_SIZE));*api=provider;*lease=++leases;return 0; }
int solar_os_native_library_release(uint32_t lease)
{ assert(lease && leases);if(fail_release)return STATE;leases--;return 0; }
static esp_err_t hw_status(void *ctx,solar_os_motor_status_t *s)
{ (void)ctx;time_value+=motor_status_delay_us;*s=(solar_os_motor_status_t){.mode=motor_value?SOLAR_OS_MOTOR_MODE_FORWARD:SOLAR_OS_MOTOR_MODE_COAST,.drive_valid=true,.drive=motor_value};return ESP_OK; }
static unsigned drive_calls;
static esp_err_t hw_drive(void *ctx,float value) { (void)ctx;drive_calls++;motor_value=value;return ESP_OK; }
static unsigned coast_calls,fail_coast_at,read_calls,fail_read_from;
static esp_err_t hw_coast(void *ctx)
{ (void)ctx;coast_calls++;if(fail_coast || coast_calls==fail_coast_at)return ESP_FAIL;motor_value=0;return ESP_OK; }
static esp_err_t hw_read(void *ctx,int32_t *value)
{ (void)ctx;read_calls++;if(fail_read_from && read_calls>=fail_read_from)return ESP_FAIL;*value=encoder_count;return ESP_OK; }
static esp_err_t hw_reset(void *ctx) { (void)ctx;encoder_count=0;return ESP_OK; }

static void setup(void)
{
    const solar_os_motor_ops_t mo={.status=hw_status,.drive=hw_drive,.coast=hw_coast};
    const solar_os_motor_registration_t mr={.name="left",.driver="fake",.ops=&mo};
    const solar_os_encoder_ops_t eo={.read=hw_read,.reset=hw_reset};
    const solar_os_encoder_registration_t er={.name="left_enc",.driver="fake",.mode=SOLAR_OS_ENCODER_MODE_X1,.ops=&eo};
    assert(solar_os_motor_register(&mr)==ESP_OK && solar_os_encoder_register(&er)==ESP_OK);
    static const solar_os_native_memory_api_v1_t mem={.alloc=provider_alloc,.free=provider_free};
    static const solar_os_native_time_api_v1_t time={.now_us=esp_timer_get_time,.sleep_ms=time_sleep};
    static const solar_os_native_thread_api_v1_t th={.mutex_create=native_mutex_create,.mutex_lock=native_mutex_lock,
        .mutex_unlock=native_mutex_unlock,.mutex_destroy=native_mutex_destroy,.worker_start=worker_start,.worker_join=worker_join};
    const solar_os_native_motion_api_v1_t *hw=solar_os_native_motion_get_service("motion",1,sizeof(*hw));
    assert(hw && hw->capabilities==3 && !hw->servo_open);
    const solar_os_native_controllers_api_v1_t *control=solar_os_native_controllers_get_service("controllers",1,sizeof(*control));
    assert(control);
    provider=solar_os_robot_engine_init(&mem,&time,&th,hw,control);assert(provider);
}
static void teardown(void)
{
    assert(!leases && !workers && !provider_allocations);
    assert(!solar_os_controller_count());
    assert(solar_os_robot_engine_shutdown()==0);
    assert(solar_os_motor_unregister("left")==ESP_OK && solar_os_encoder_unregister("left_enc")==ESP_OK);
    assert(!device_allocations);
}
static const char *model_velocity_fixture=
        "<robot name='bound'><link name='root'/><link name='wheel'/>"
        "<joint name='model_wheel' type='continuous'><parent link='root'/><child link='wheel'/></joint>"
        "<solar_os version='1'><joint name='model_wheel' command='velocity'>"
        "<motor device='left'/><encoder device='left_enc' ppr='70'/><transmission gear_ratio='30'/>"
        "<param name='velocity_limit' value='3'/></joint></solar_os></robot>";

/* No robot handles, provider calls or worker are involved in this loop. */
static void standalone_controllers_test(void)
{
    assert(!leases && !workers && !provider_allocations);
    assert(solar_os_encoder_configure("left_enc","ppr",70)==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","gear_ratio",30)==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","velocity_tau",0)==ESP_OK);
    assert(solar_os_motor_configure("left","max_drive",.2f)==ESP_OK);
    assert(solar_os_controller_create("standalone",SOLAR_OS_CONTROLLER_PID)==ESP_OK);
    for(unsigned i=0;i<2;++i) {
        assert(solar_os_controller_configure("standalone",i?"output_max":"output_min",i?.2f:-.2f)==ESP_OK);
    }
    assert(solar_os_controller_configure("standalone","command_timeout_ms",40)==ESP_OK);
    assert(solar_os_controller_configure("standalone","setpoint",1)==ESP_OK);
    assert(solar_os_controller_bind("standalone","enc.left_enc","stream:mot.left")==ESP_OK);
    encoder_count=0;
    assert(solar_os_controller_start("standalone")==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","ppr",20)==ESP_ERR_INVALID_STATE);
    assert(solar_os_parameter_set("enc.left_enc.gear_ratio",2)==ESP_ERR_INVALID_STATE);
    assert(solar_os_motor_configure("left","polarity",-1)==ESP_ERR_INVALID_STATE);
    assert(solar_os_motor_drive("left",0)==ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_reset("left_enc")==ESP_ERR_INVALID_STATE);
    time_value+=20000;encoder_count=7;solar_os_controllers_tick(time_value/1000);
    solar_os_controller_info_t s;
    assert(solar_os_controller_find("standalone",&s)==ESP_OK && s.enabled && !s.owner[0]);
    assert(fabsf(s.measurement-(float)(6.283185307*7/(2100*.02)))<.0001f && motor_value<0);
    time_value+=40000;solar_os_controllers_tick(time_value/1000);
    assert(solar_os_controller_find("standalone",&s)==ESP_OK && s.waiting && !s.faulted && !motor_value);
    assert(solar_os_controller_configure("standalone","setpoint",1)==ESP_OK);
    time_value+=20000;solar_os_controllers_tick(time_value/1000);
    assert(solar_os_controller_find("standalone",&s)==ESP_OK && !s.waiting && motor_value>0);
    assert(solar_os_controller_stop("standalone")==ESP_OK);
    /* Reuse the bound endpoints for automatic multi-speed identification.
     * At this low speed, filter quantized 2100-count/rev feedback as on device. */
    assert(solar_os_encoder_configure("left_enc","velocity_tau",.1f)==ESP_OK);
    assert(solar_os_controller_configure("standalone","setpoint",0)==ESP_OK);
    assert(solar_os_controller_configure("standalone","command_timeout_ms",0)==ESP_OK);
    assert(solar_os_controller_tune("standalone",.01f,3,120000)==ESP_OK);
    float delay[18]={0},velocity=0;double position=0;unsigned cursor=0;
    int32_t baseline=encoder_count;
    for(unsigned i=0;i<6000;++i) {
        velocity+=(delay[cursor]-velocity)*.02f/.1f;
        position+=velocity*.02;
        encoder_count=baseline+(int32_t)llround(position*2100/6.283185307);
        time_value+=20000;solar_os_controllers_tick(time_value/1000);
        assert(solar_os_controller_find("standalone",&s)==ESP_OK);
        if(s.tuning.state!=SOLAR_OS_CONTROLLER_TUNE_RUNNING) break;
        assert(fabsf(motor_value)<=.200001f);
        delay[cursor]=motor_value;cursor=(cursor+1)%18;
    }
    if(s.tuning.state!=SOLAR_OS_CONTROLLER_TUNE_COMPLETE) fprintf(stderr,
        "standalone autotune: reason=%s phase=%s elapsed=%g point=%u measured=%g breakaway=%g\n",
        solar_os_controller_tune_reason_name(s.tuning.reason),solar_os_controller_tune_phase_name(s.tuning.phase),
        s.tuning.elapsed,s.tuning.point,s.measurement,s.tuning.breakaway);
    assert(s.tuning.state==SOLAR_OS_CONTROLLER_TUNE_COMPLETE && !s.claimed && !s.enabled && !motor_value);
    assert(s.config.algorithm.kp==1 && s.config.algorithm.ki==.1f && s.tuning.kp>0);
    assert(solar_os_controller_start("standalone")==ESP_OK);
    fail_coast_at=coast_calls+2; /* Safe write succeeds, final output close fails. */
    assert(solar_os_controller_stop("standalone")==ESP_FAIL);
    assert(solar_os_encoder_reset("left_enc")==ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_configure("left_enc","ppr",10)==ESP_ERR_INVALID_STATE);
    assert(solar_os_controller_find("standalone",&s)==ESP_OK && s.claimed);
    fail_coast_at=0;
    assert(solar_os_controller_stop("standalone")==ESP_OK);
    assert(solar_os_controller_delete("standalone")==ESP_OK);
    const char *long_name="long_01234567890123456";
    assert(strlen(long_name)==22);
    assert(solar_os_controller_create(long_name,SOLAR_OS_CONTROLLER_PID)==ESP_OK);
    assert(solar_os_controller_configure(long_name,"output_min",-.2f)==ESP_OK);
    assert(solar_os_controller_configure(long_name,"output_max",.2f)==ESP_OK);
    assert(solar_os_controller_bind(long_name,"enc.left_enc","stream:mot.left")==ESP_OK);
    assert(solar_os_controller_start(long_name)==ESP_OK);
    solar_os_stream_info_t stream;
    assert(solar_os_stream_get_info("mot.left",&stream)==ESP_OK && !strcmp(stream.owner,long_name));
    assert(solar_os_controller_stop(long_name)==ESP_OK && solar_os_controller_delete(long_name)==ESP_OK);
    assert(!leases && !workers && !provider_allocations);
    assert(solar_os_encoder_configure("left_enc","ppr",0)==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","gear_ratio",1)==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","velocity_tau",.1f)==ESP_OK);
    assert(solar_os_motor_configure("left","max_drive",1)==ESP_OK);
    encoder_count=0;
}

static void model_parameter_transaction_test(void)
{
    char error[128];
    assert(solar_os_encoder_configure("left_enc","ppr",17)==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","gear_ratio",5)==ESP_OK);
    assert(solar_os_motor_configure("left","max_drive",.7f)==ESP_OK);
    assert(solar_os_robot_model_load_xml(model_velocity_fixture,strlen(model_velocity_fixture),error,sizeof(error))==ESP_OK);
    assert(solar_os_parameter_claim("enc.left_enc.ppr","other")==ESP_OK);
    assert(solar_os_robot_start()==ESP_ERR_INVALID_STATE);
    assert(!solar_os_controller_count() && !workers);
    solar_os_encoder_config_t enc;solar_os_motor_config_t motor;
    assert(solar_os_encoder_config_get("left_enc",&enc)==ESP_OK && enc.ppr==17 && enc.gear_ratio==5);
    assert(solar_os_parameter_release("enc.left_enc.ppr","other")==ESP_OK);
    fail_read_from=read_calls+2; /* Fail calibration after stream acquisition. */
    assert(solar_os_robot_start()!=ESP_OK);
    assert(solar_os_robot_stop()!=ESP_OK && solar_os_controller_count()==1);
    assert(solar_os_motor_drive("left",0)==ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_configure("left_enc","ppr",99)==ESP_ERR_INVALID_STATE);
    fail_read_from=0;
    assert(solar_os_robot_stop()==ESP_OK && !solar_os_controller_count());
    assert(solar_os_encoder_config_get("left_enc",&enc)==ESP_OK && enc.ppr==17 && enc.gear_ratio==5);
    assert(solar_os_robot_start()==ESP_OK);
    assert(solar_os_encoder_config_get("left_enc",&enc)==ESP_OK && enc.ppr==70 && enc.gear_ratio==30);
    assert(solar_os_motor_config_get("left",&motor)==ESP_OK && motor.max_drive==.2f);
    assert(solar_os_parameter_set("enc.left_enc.ppr",12)==ESP_ERR_INVALID_STATE);
    fail_coast=true;
    assert(solar_os_robot_stop()!=ESP_OK);
    assert(solar_os_encoder_config_get("left_enc",&enc)==ESP_OK && enc.ppr==70 && enc.gear_ratio==30);
    assert(solar_os_motor_configure("left","max_drive",1)==ESP_ERR_INVALID_STATE);
    fail_coast=false;
    assert(solar_os_robot_stop()==ESP_OK);
    assert(solar_os_encoder_config_get("left_enc",&enc)==ESP_OK && enc.ppr==17 && enc.gear_ratio==5);
    assert(solar_os_motor_config_get("left",&motor)==ESP_OK && motor.max_drive==.7f);
    assert(solar_os_robot_model_unload()==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","ppr",0)==ESP_OK);
    assert(solar_os_encoder_configure("left_enc","gear_ratio",1)==ESP_OK);
    assert(solar_os_motor_configure("left","max_drive",1)==ESP_OK);
}

static void model_controllers_test(void)
{
    char error[128];
    const char *single=model_velocity_fixture;
    assert(solar_os_robot_start()==ESP_ERR_NOT_FOUND);
    assert(solar_os_robot_stop()==ESP_OK);
    assert(solar_os_robot_model_load_xml(single,strlen(single),error,sizeof(error))==ESP_OK);
    unsigned loaded=provider_allocations;
    assert(leases==1 && !workers && !solar_os_robot_joint_count());
    assert(solar_os_motor_drive("left",0)==ESP_OK && solar_os_encoder_reset("left_enc")==ESP_OK);
    assert(solar_os_robot_start()==ESP_OK && leases==2 && workers==1);
    solar_os_robot_joint_info_v3_t s;
    assert(solar_os_robot_joint_status("model_wheel",&s)==ESP_OK);
    assert(s.state==SOLAR_OS_ROBOT_WAITING && !s.command_valid && !motor_value);
    solar_os_controller_info_t controller;
    assert(solar_os_controller_count()==1 && solar_os_controller_find("model_wheel",&controller)==ESP_OK);
    assert(!strcmp(controller.owner,"robot") && controller.enabled && controller.waiting);
    assert(s.config.ppr==70 && s.config.gear_ratio==30 && s.encoder_counts_per_rev==2100);
    assert(solar_os_robot_start()==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_model_unload()==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_model_load_xml(single,strlen(single),error,sizeof(error))==ESP_ERR_INVALID_STATE && *error);
    assert(solar_os_robot_joint_configure("model_wheel","kp",1)==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_joint_bind_motor("model_wheel","left","left_enc")==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_joint_close("model_wheel")==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_joint_enable("model_wheel")==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_release_owner("robot")==ESP_OK && workers==1);
    assert(solar_os_robot_joint_command("model_wheel",1)==ESP_OK);
    time_value+=20000;
    lock();
    for(unsigned i=0;i<SOLAR_OS_ROBOT_JOINT_MAX;++i) if(joints[i]) cycle(joints[i]);
    unlock();
    assert(solar_os_robot_joint_status("model_wheel",&s)==ESP_OK && s.output>0);
    assert(solar_os_controller_find("model_wheel",&controller)==ESP_OK && controller.steps==1);
    assert(controller.state.p==s.p && controller.state.i==s.i && !controller.waiting);
    motor_status_delay_us=120000; time_value+=20000;
    unsigned writes_before=drive_calls;
    lock();
    for(unsigned i=0;i<SOLAR_OS_ROBOT_JOINT_MAX;++i) if(joints[i]) cycle(joints[i]);
    unlock(); motor_status_delay_us=0;
    /* Feedback ages past 100ms, while command freshness and max_gap still pass. */
    assert(solar_os_robot_joint_status("model_wheel",&s)==ESP_OK && s.state==SOLAR_OS_ROBOT_FAULT && !motor_value);
    assert(s.last_error==TIMEOUT);
    assert(drive_calls==writes_before); /* Stale feedback never reaches actuation. */
    assert(solar_os_robot_stop()==ESP_OK);
    assert(solar_os_robot_start()==ESP_OK && solar_os_robot_joint_command("model_wheel",1)==ESP_OK);
    assert(solar_os_controller_stop("model_wheel")==ESP_OK);
    time_value+=20000;
    lock();
    for(unsigned i=0;i<SOLAR_OS_ROBOT_JOINT_MAX;++i) if(joints[i]) cycle(joints[i]);
    unlock();
    assert(solar_os_robot_joint_status("model_wheel",&s)==ESP_OK && s.state==SOLAR_OS_ROBOT_FAULT && !motor_value);
    assert(solar_os_robot_stop()==ESP_OK && !solar_os_controller_count());
    assert(solar_os_robot_start()==ESP_OK && solar_os_controller_count()==1);
    /* Failed stop and failed lease release both retain ownership for retry. */
    fail_coast=true;
    assert(solar_os_robot_stop()!=ESP_OK && !workers && leases==2);
    assert(solar_os_controller_find("model_wheel",&controller)==ESP_OK && controller.faulted && controller.last_error==ESP_FAIL);
    assert(solar_os_robot_joint_command("model_wheel",1)==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_model_unload()==ESP_ERR_INVALID_STATE);
    fail_coast=false; fail_release=true;
    assert(solar_os_robot_stop()==ESP_ERR_INVALID_STATE && leases==2 && provider_allocations==loaded);
    assert(solar_os_robot_model_unload()==ESP_ERR_INVALID_STATE);
    fail_release=false;
    assert(solar_os_robot_stop()==ESP_OK && leases==1 && !solar_os_robot_joint_count());
    assert(!solar_os_controller_count());
    assert(solar_os_robot_start()==ESP_OK && workers==1);
    assert(solar_os_robot_stop()==ESP_OK && !workers);
    /* The OS registry is shared: conflicts/exhaustion preserve its other owners. */
    assert(solar_os_controller_create("model_wheel",SOLAR_OS_CONTROLLER_PID)==ESP_OK);
    assert(solar_os_robot_start()==ESP_ERR_INVALID_STATE && !workers && solar_os_controller_count()==1);
    assert(solar_os_controller_find("model_wheel",&controller)==ESP_OK && !*controller.owner);
    assert(solar_os_controller_delete("model_wheel")==ESP_OK);
    for(unsigned i=0;i<SOLAR_OS_CONTROLLER_MAX;++i) {
        char name[20];snprintf(name,sizeof(name),"existing_%u",i);
        assert(solar_os_controller_create(name,SOLAR_OS_CONTROLLER_PID)==ESP_OK);
    }
    assert(solar_os_robot_start()==ESP_ERR_NO_MEM && !workers && solar_os_controller_count()==SOLAR_OS_CONTROLLER_MAX);
    assert(!solar_os_robot_joint_count());
    for(unsigned i=0;i<SOLAR_OS_CONTROLLER_MAX;++i) {
        char name[20];snprintf(name,sizeof(name),"existing_%u",i);
        assert(solar_os_controller_delete(name)==ESP_OK);
    }
    /* Name conflicts leave independent controllers untouched. */
    assert(solar_os_robot_joint_create("model_wheel",SOLAR_OS_ROBOT_MOTOR_VELOCITY,"python")==ESP_OK);
    assert(solar_os_robot_start()==ESP_ERR_INVALID_STATE && leases==2);
    assert(solar_os_robot_joint_status("model_wheel",&s)==ESP_OK);
    assert(solar_os_robot_stop()==ESP_OK && solar_os_robot_joint_count()==1);
    assert(solar_os_robot_release_owner("python")==ESP_OK);
    assert(solar_os_robot_model_unload()==ESP_OK && !leases);
    /* Left enables, right fails resolution: join and release left as well. */
    assert(solar_os_robot_model_load_file(ROBOT_MODEL_FIXTURE,error,sizeof(error))==ESP_OK);
    assert(solar_os_robot_start()==ESP_ERR_NOT_FOUND);
    assert(!workers && leases==1 && !solar_os_robot_joint_count());
    assert(!solar_os_controller_count());
    assert(solar_os_motor_drive("left",0)==ESP_OK && solar_os_encoder_reset("left_enc")==ESP_OK);
    const solar_os_motor_ops_t mo={.status=hw_status,.drive=hw_drive,.coast=hw_coast};
    const solar_os_motor_registration_t mr={.name="right",.driver="fake",.ops=&mo};
    const solar_os_encoder_ops_t eo={.read=hw_read,.reset=hw_reset};
    const solar_os_encoder_registration_t er={.name="right_enc",.driver="fake",.mode=SOLAR_OS_ENCODER_MODE_X1,.ops=&eo};
    assert(solar_os_motor_register(&mr)==ESP_OK && solar_os_encoder_register(&er)==ESP_OK);
    assert(solar_os_robot_joint_create("independent",SOLAR_OS_ROBOT_MOTOR_VELOCITY,"python")==ESP_OK);
    assert(solar_os_robot_start()==ESP_OK && workers==2 && leases==4 && solar_os_robot_joint_count()==3);
    assert(solar_os_controller_count()==2);
    assert(solar_os_robot_joint_status("right_wheel_joint",&s)==ESP_OK && s.state==SOLAR_OS_ROBOT_WAITING);
    assert(s.config.velocity_limit==3 && s.config.command_timeout_ms==150 && s.config.kp==0.04f);
    assert(solar_os_robot_stop()==ESP_OK && !workers && leases==2 && solar_os_robot_joint_count()==1);
    assert(!solar_os_controller_count());
    assert(solar_os_robot_joint_status("independent",&s)==ESP_OK);
    assert(solar_os_robot_release_owner("python")==ESP_OK && leases==1);
    assert(solar_os_motor_unregister("right")==ESP_OK && solar_os_encoder_unregister("right_enc")==ESP_OK);
    assert(solar_os_robot_model_unload()==ESP_OK && !leases);
}

int main(void)
{
    setup();
    standalone_controllers_test();
    model_parameter_transaction_test();
    char error[128];
    uint32_t handles[SOLAR_OS_ROBOT_MODEL_MAX], extra = 0;
    const char *minimal="<robot name='minimal'><link name='root'/></robot>";
    for (unsigned i=0;i<SOLAR_OS_ROBOT_MODEL_MAX;++i)
        assert(provider->model_load(minimal,strlen(minimal),&handles[i],error,sizeof(error))==OK);
    unsigned held_allocations=provider_allocations;
    assert(provider->model_load(minimal,strlen(minimal),&extra,error,sizeof(error))==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(!extra && provider_allocations==held_allocations);
    for (unsigned i=0;i<SOLAR_OS_ROBOT_MODEL_MAX;++i) {
        assert(provider->model_close(handles[i])==OK);
        assert(provider->model_close(handles[i])==MISSING);
    }
    assert(!provider_allocations);
    fail_release=true;
    assert(solar_os_robot_model_load_xml("<bad/>",6,error,sizeof(error))!=ESP_OK && leases==1 && !provider_allocations);
    fail_release=false;
    assert(solar_os_robot_model_unload()==ESP_OK && !leases);
    assert(solar_os_robot_model_load_file(ROBOT_MODEL_FIXTURE,error,sizeof(error))==ESP_OK && leases==1);
    assert(solar_os_robot_engine_shutdown()==STATE);
    solar_os_robot_model_info_t mi;
    assert(solar_os_robot_model_get_info(&mi)==ESP_OK && mi.links==5 && mi.joints==4);
    assert(solar_os_robot_model_load_xml("<bad/>",6,error,sizeof(error))!=ESP_OK && *error);
    assert(solar_os_robot_model_get_info(&mi)==ESP_OK && !strcmp(mi.name,"solbot") && leases==1);
    const char *positions[]={"left_wheel_joint","right_wheel_joint"};float values[]={0,0};
    assert(solar_os_robot_model_update(positions,values,2)==ESP_OK);
    solar_os_robot_transform_t transform;
    assert(solar_os_robot_model_transform("right_wheel_link","left_wheel_link",&transform)==ESP_OK);
    assert(fabsf(transform.translation[1]-.0701282f)<1e-6f);
    /* Reload leaves no old model allocations or extra leases. */
    unsigned model_allocations=provider_allocations;
    assert(solar_os_robot_model_load_file(ROBOT_MODEL_FIXTURE,error,sizeof(error))==ESP_OK);
    assert(provider_allocations==model_allocations && leases==1 && !workers);
    fail_release=true;
    assert(solar_os_robot_model_unload()==ESP_ERR_INVALID_STATE && leases==1 && !provider_allocations);
    fail_release=false;
    assert(solar_os_robot_model_unload()==ESP_OK && !leases);
    assert(solar_os_robot_model_transform("base_link","base_link",&transform)==ESP_ERR_NOT_FOUND);
    assert(solar_os_robot_joint_create("wheel",SOLAR_OS_ROBOT_MOTOR_VELOCITY,"python")==ESP_OK && leases==1);
    assert(solar_os_robot_joint_configure("wheel","ppr",7)==ESP_OK);
    assert(solar_os_robot_joint_configure("wheel","gear_ratio",30)==ESP_OK);
    assert(solar_os_robot_joint_bind_motor("wheel","left","left_enc")==ESP_OK);
    assert(solar_os_robot_joint_enable("wheel")==ESP_OK && workers==1);
    /* Loading a description never touches active joint claims/workers. */
    assert(solar_os_robot_model_load_file(ROBOT_MODEL_FIXTURE,error,sizeof(error))==ESP_OK && workers==1 && leases==2);
    assert(solar_os_robot_model_unload()==ESP_OK && workers==1 && leases==1);
    assert(solar_os_motor_drive("left",.3f)==ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_reset("left_enc")==ESP_ERR_INVALID_STATE);
    assert(solar_os_motor_unregister("left")==ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_unregister("left_enc")==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_joint_command("wheel",1)==ESP_OK);
    time_value+=20000;encoder_count=1;
    lock();for(unsigned i=0;i<SOLAR_OS_ROBOT_JOINT_MAX;++i) if(joints[i]) cycle(joints[i]);unlock();
    solar_os_robot_joint_info_v3_t s;
    assert(solar_os_robot_joint_status("wheel",&s)==ESP_OK && s.velocity_valid && s.sample_us==time_value);
    fail_coast=true;
    assert(solar_os_robot_release_owner("python")!=ESP_OK && leases==1 && !workers);
    assert(solar_os_motor_drive("left",.3f)==ESP_ERR_INVALID_STATE);
    fail_coast=false;fail_release=true;
    assert(solar_os_robot_joint_close("wheel")==ESP_ERR_INVALID_STATE && leases==1 && !provider_allocations);
    fail_release=false;
    assert(solar_os_robot_joint_close("wheel")==ESP_OK && !leases);
    assert(solar_os_motor_drive("left",0)==ESP_OK && solar_os_encoder_reset("left_enc")==ESP_OK);
    model_controllers_test();
    teardown();
    puts("robot integration: native device ABI, claims, encoder scaling, frontend leases and cleanup retry passed");
    return 0;
}
