#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../modules/robot/main/robot.c"
#include "robot_controller_stubs.inc"
#include "solar_os_encoder.h"
#include "solar_os_motor.h"
#include "solar_os_memory.h"

static uint64_t fake_time;
static int32_t count;
static uint32_t decoding = 1;
static bool servo_held, fail_stop, fail_read;
static float drive, servo_angle;
static unsigned allocations, joins;
static void *allocate(size_t n) { void *p = calloc(1, n); if (p) allocations++; return p; }
static void deallocate(void *p) { if (p) { allocations--; free(p); } }
static int64_t time_us(void) { return fake_time; }
int64_t esp_timer_get_time(void) { return time_us(); }
void vTaskDelay(uint32_t ticks) { fake_time+=(uint64_t)ticks*1000; }
static void sleep_ms(uint32_t ms) { fake_time += ms * 1000; }
static int mutex_create(void **out, uint32_t recursive)
{ (void)recursive; *out = malloc(sizeof(pthread_mutex_t)); return *out ? pthread_mutex_init(*out, NULL) : 1; }
static int mutex_lock(void *m) { return pthread_mutex_lock(m); }
static int mutex_unlock(void *m) { return pthread_mutex_unlock(m); }
static int mutex_destroy(void *m) { int err = pthread_mutex_destroy(m); if (!err) free(m); return err; }
static int start_worker(void **out, void (*entry)(void *), void *arg, uint32_t stack, uint32_t core)
{ (void)entry; (void)arg; assert(stack == 4096 && core == 0); *out = malloc(1); return *out ? 0 : 1; }
static int join_worker(void *w) { free(w); joins++; return 0; }
static esp_err_t endpoint_status(void *ctx,solar_os_motor_status_t *s)
{ (void)ctx;*s=(solar_os_motor_status_t){.drive_valid=true,.drive=drive};return ESP_OK; }
static esp_err_t endpoint_drive(void *ctx,float value)
{ (void)ctx;drive=value;return ESP_OK; }
static esp_err_t endpoint_stop(void *ctx)
{ (void)ctx;if(fail_stop)return ESP_ERR_INVALID_STATE;drive=0;return ESP_OK; }
static esp_err_t endpoint_read(void *ctx,int32_t *value)
{ (void)ctx;*value=count;return fail_read?ESP_ERR_INVALID_STATE:ESP_OK; }
static bool motor_held(void)
{ solar_os_motor_status_t s;assert(solar_os_motor_status("left",&s)==ESP_OK);return s.claimed; }
static bool encoder_held(void)
{ solar_os_encoder_status_t s;assert(solar_os_encoder_status("enc",&s)==ESP_OK);return s.claimed; }
static void attach_encoder(uint32_t mode)
{
    if(solar_os_encoder_count()) assert(solar_os_encoder_unregister("enc")==ESP_OK);
    const solar_os_encoder_ops_t ops={.read=endpoint_read};
    const solar_os_encoder_registration_t r={.name="enc",.driver="fake",.mode=(solar_os_encoder_mode_t)mode,.ops=&ops};
    assert(solar_os_encoder_register(&r)==ESP_OK);
}
size_t strlcpy(char *dst,const char *src,size_t bytes)
{ size_t n=strlen(src);if(bytes){size_t k=n<bytes-1?n:bytes-1;memcpy(dst,src,k);dst[k]=0;}return n; }
static unsigned device_allocations;
void *solar_os_memory_calloc(size_t n,size_t bytes,solar_os_memory_class_t cls,const char *tag)
{ (void)cls;(void)tag;void *p=calloc(n,bytes);if(p)device_allocations++;return p; }
void solar_os_memory_free(void *p) { if(p){device_allocations--;free(p);} }
static int open_servo(const char *name, const char *owner, const solar_os_native_servo_config_v1_t *c)
{ assert(equal(name,owner) && c->frequency_hz == 50); if (servo_held) return STATE; servo_held=true; return OK; }
static int set_servo(const char *name, const char *owner, float angle)
{ assert(equal(name,owner) && servo_held); servo_angle=angle; return OK; }
static int disable_servo(const char *name, const char *owner)
{ assert(equal(name,owner) && servo_held); return fail_stop ? STATE : OK; }
static int close_servo(const char *name, const char *owner)
{ int ret=disable_servo(name,owner); if (!ret) servo_held=false; return ret; }
static void tick(uint32_t id, uint32_t ms)
{ fake_time += ms*1000; lock(); joint_t *a=find(id); assert(a); cycle(a); unlock(); }
static solar_os_robot_joint_info_v3_t info(uint32_t id)
{ solar_os_robot_joint_info_v3_t s; assert(status(id,&s)==OK); return s; }

int main(void)
{
    const solar_os_native_memory_api_v1_t mem={.alloc=allocate,.free=deallocate};
    const solar_os_native_time_api_v1_t clk={.now_us=time_us,.sleep_ms=sleep_ms};
    const solar_os_native_thread_api_v1_t th={.mutex_create=mutex_create,.mutex_lock=mutex_lock,
        .mutex_unlock=mutex_unlock,.mutex_destroy=mutex_destroy,.worker_start=start_worker,.worker_join=join_worker};
    const solar_os_native_motion_api_v1_t hw={.capabilities=7,
        .servo_open=open_servo,.servo_set=set_servo,.servo_disable=disable_servo,.servo_close=close_servo};
    const solar_os_native_controllers_api_v1_t *control=solar_os_native_controllers_get_service("controllers",1,sizeof(*control));
    assert(control);
    const solar_os_robot_api_v3_t *api=solar_os_robot_engine_init(&mem,&clk,&th,&hw,control); assert(api);
    const solar_os_motor_ops_t ops={.status=endpoint_status,.drive=endpoint_drive,.coast=endpoint_stop,.brake=endpoint_stop};
    const solar_os_motor_registration_t motor={.name="left",.driver="fake",.ops=&ops};
    assert(solar_os_motor_register(&motor)==ESP_OK);attach_encoder(1);
    uint32_t id;
    assert(api->create("wheel",SOLAR_OS_ROBOT_MOTOR_VELOCITY,&id)==OK);
    assert(!motor_held() && !encoder_held());
    assert(info(id).config.ppr==0 && info(id).config.gear_ratio==0);
    assert(api->configure(id,"ppr",0)==ARG && api->configure(id,"gear_ratio",0)==ARG);
    assert(api->bind_motor(id,"left","enc")==OK);
    assert(api->enable(id)==STATE); /* PPR and ratio must be explicit. */
    assert(api->configure(id,"ppr",10)==OK && api->configure(id,"gear_ratio",2)==OK);
    assert(api->configure(id,"velocity_tau",0)==OK);
    count=0; assert(api->enable(id)==OK);
    for (int i=0;i<50;i++) { count++; tick(id,20); }
    assert(info(id).state==SOLAR_OS_ROBOT_WAITING && !info(id).command_valid);
    assert(info(id).command_expires_us==0 && info(id).velocity>0 && drive==0);
    assert(api->command(id,0)==OK); /* Even zero explicitly arms the watchdog. */
    for (int i=0;i<15;i++) tick(id,20);
    assert(info(id).state==SOLAR_OS_ROBOT_WAITING && drive==0 && info(id).faults==0);
    assert(api->command(id,1)==OK && api->configure(id,"kp",1)==STATE);
    tick(id,20); assert(info(id).state==SOLAR_OS_ROBOT_ACTIVE && drive>0);
    assert(api->disable(id)==OK);
    assert(api->command(id,1)==OK);
    fake_time+=280000;
    assert(api->enable(id)==OK);tick(id,20);
    assert(info(id).state==SOLAR_OS_ROBOT_WAITING && !info(id).command_valid && drive==0);
    assert(api->disable(id)==OK);
    assert(api->enable(id)==OK && api->command(id,1)==OK);
    fail_stop=true;
    for (int i=0;i<15;i++) tick(id,20);
    assert(info(id).state==SOLAR_OS_ROBOT_FAULT && motor_held() && encoder_held());
    assert(api->command(id,1)==STATE); /* Failed stopping is never auto-resumable. */
    fail_stop=false;assert(api->disable(id)==OK);
    assert(api->command(id,1)==OK); fake_time+=400000; /* A queued command aged while disabled. */
    assert(api->enable(id)==OK); tick(id,20);
    assert(info(id).state==SOLAR_OS_ROBOT_WAITING && !info(id).command_valid && info(id).target==0 && drive==0);
    fail_read=true; tick(id,20); /* Idle enable must still monitor feedback. */
    assert(info(id).state==SOLAR_OS_ROBOT_FAULT && drive==0);
    fail_read=false;
    assert(api->disable(id)==OK);
    assert(api->command(id,1)==OK && api->enable(id)==OK); tick(id,20);
    assert(info(id).command_valid && info(id).target==1 && drive>0); /* Fresh pre-enable commands still work. */
    assert(api->disable(id)==OK);
    for (uint32_t mode=1; mode<=4; mode*=2) {
        decoding=mode;attach_encoder(mode); count=0;
        assert(api->enable(id)==OK);
        count=10*(int)mode; tick(id,20);
        solar_os_robot_joint_info_v3_t s=info(id);
        assert(fabsf(s.position-(float)PI)<0.0001f);
        assert(fabsf(s.velocity-(float)(PI/.02))<0.001f);
        assert(!s.position_valid && !s.referenced && s.velocity_valid);
        assert(api->configure(id,"ppr",20)==STATE);
        assert(api->disable(id)==OK && !motor_held() && !encoder_held() && drive==0);
    }
    assert(api->configure(id,"encoder_polarity",-1)==OK);
    assert(api->configure(id,"motor_polarity",-1)==OK);
    count=INT32_MAX-1; assert(api->enable(id)==OK);
    assert(api->command(id,0)==OK);
    count=INT32_MIN+2; tick(id,20); /* Four counts across signed wrap. */
    assert(info(id).velocity < 0 && drive < 0);
    assert(api->disable(id)==OK);
    assert(api->configure(id,"encoder_on_output",1)==OK);
    count=0; assert(api->enable(id)==OK); count=40; tick(id,20);
    assert(fabsf(info(id).position + (float)(2*PI))<0.0001f); /* Ratio applied once, and omitted on output. */
    fail_stop=true; tick(id,400); assert(info(id).state==SOLAR_OS_ROBOT_FAULT);
    assert(api->close(id)==STATE && motor_held() && encoder_held());
    fail_stop=false; assert(api->disable(id)==OK);
    assert(api->configure(id,"encoder_polarity",1)==OK && api->configure(id,"motor_polarity",1)==OK);
    assert(api->configure(id,"encoder_on_output",0)==OK);
    /* Per-direction friction compensation belongs to the joint. Verify that
     * it participates in anti-windup, honors polarity/caps, and vanishes at
     * zero command without hiding encoder feedback. */
    assert(api->configure(id,"friction_bias_forward",.15f)==OK);
    assert(api->configure(id,"friction_bias_reverse",.12f)==OK);
    assert(api->configure(id,"friction_bias_forward",.21f)==ARG);
    assert(api->configure(id,"max_drive",.1f)==ARG);
    assert(api->configure(id,"velocity_deadband",.1f)==OK);
    assert(api->configure(id,"velocity_deadband",-1)==ARG);
    assert(api->configure(id,"velocity_deadband",10)==ARG);
    assert(api->configure(id,"acceleration_limit",1000)==OK);
    assert(api->configure(id,"kp",1)==OK && api->configure(id,"ki",1)==OK);
    count=0; assert(api->enable(id)==OK);
    assert(api->command(id,1)==OK); tick(id,20);
    assert(fabsf(drive-.2f)<1e-6f && info(id).feedforward==.15f);
    assert(info(id).saturated && info(id).i==0); /* Total output already capped. */
    assert(api->command(id,-1)==OK); tick(id,20);
    assert(fabsf(drive+.2f)<1e-6f && info(id).feedforward==-.12f && info(id).i==0);
    assert(api->command(id,.05f)==OK); tick(id,20);
    assert(info(id).target==0 && info(id).setpoint==0 && info(id).feedforward==0 && drive==0);
    count++; tick(id,20); assert(info(id).velocity>0 && drive<0); /* Feedback still live at zero. */
    assert(api->disable(id)==OK && info(id).feedforward==0);
    assert(api->configure(id,"motor_polarity",-1)==OK);
    assert(api->enable(id)==OK && api->command(id,1)==OK); tick(id,20);
    assert(drive<0 && info(id).feedforward==.15f);
    assert(api->disable(id)==OK);
    assert(api->configure(id,"motor_polarity",1)==OK);
    assert(api->configure(id,"friction_bias_forward",0)==OK && api->configure(id,"friction_bias_reverse",0)==OK);
    assert(api->configure(id,"velocity_deadband",0)==OK);
    assert(api->configure(id,"ppr",1000)==OK && api->configure(id,"gear_ratio",1)==OK);
    assert(api->configure(id,"kp",.1f)==OK && api->configure(id,"ki",.2f)==OK);
    assert(api->configure(id,"velocity_limit",5)==OK && api->configure(id,"max_drive",.5f)==OK);
    assert(api->configure(id,"acceleration_limit",100)==OK);
    decoding=1;attach_encoder(1); count=0; assert(api->enable(id)==OK);
    float velocity=0; double position=0;
    for (int i=0;i<1000;i++) {
        float load=i>=500?.5f:0;
        assert(api->command(id,2)==OK);
        velocity+=(drive*10-load-velocity)*.02f/.1f;
        position+=velocity*.02;
        count=(int32_t)llround(position*1000/(2*PI));
        tick(id,20);
        if (i==499 || i==999) assert(fabsf(velocity-2)<.1f);
    }
    assert(api->disable(id)==OK);
    assert(api->configure(id,"max_drive",.2f)==OK);
    assert(api->configure(id,"friction_bias_forward",.15f)==OK && api->configure(id,"friction_bias_reverse",.12f)==OK);
    assert(api->configure(id,"ppr",10000)==OK && api->configure(id,"velocity_tau",.05f)==OK);
    assert(api->configure(id,"kp",.04f)==OK && api->configure(id,"ki",.1f)==OK);
    count=0; position=velocity=0; assert(api->enable(id)==OK);
    for (int i=0;i<1200;i++) {
        float target=i<400?.5f:i<800?-.5f:0;
        float effort=drive>.15f?drive-.15f:drive<-.12f?drive+.12f:0;
        velocity+=(effort*20-velocity)*.02f/.1f;
        position+=velocity*.02;
        count=(int32_t)llround(position*10000/(2*PI));
        assert(api->command(id,target)==OK); tick(id,20);
        assert(fabsf(drive)<=.200001f);
        if (i==399 || i==799 || i==1199) assert(fabsf(velocity-target)<.05f);
    }
    assert(info(id).feedforward==0);
    fail_read=true; tick(id,20); assert(info(id).state==SOLAR_OS_ROBOT_FAULT && drive==0);
    fail_read=false; assert(api->close(id)==OK && !allocations);
    assert(api->status(id,&(solar_os_robot_joint_info_v3_t){0})==MISSING);
    /* Counts per output revolution are derived, never independently writable. */
    assert(api->create("derived",SOLAR_OS_ROBOT_MOTOR_VELOCITY,&id)==OK);
    assert(api->bind_motor(id,"left","enc")==OK);
    assert(api->configure(id,"encoder_counts_per_rev",2098)==ARG);
    assert(api->configure(id,"ppr",70)==OK && api->configure(id,"gear_ratio",30)==OK);
    assert(api->configure(id,"velocity_tau",0)==OK);
    for (uint32_t mode=1;mode<=4;mode*=2) {
        decoding=mode;attach_encoder(mode);count=0;assert(api->enable(id)==OK);
        count=70*30*(int)mode;tick(id,20);
        assert(info(id).encoder_counts_per_rev==(float)count);
        assert(fabsf(info(id).position-(float)(2*PI))<.0001f);
        assert(api->disable(id)==OK);
    }
    assert(api->configure(id,"gear_ratio",10)==OK && info(id).encoder_counts_per_rev==2800);
    assert(api->configure(id,"ppr",35)==OK && info(id).encoder_counts_per_rev==1400);
    assert(api->configure(id,"ppr",70)==OK && api->configure(id,"gear_ratio",30)==OK);
    assert(api->configure(id,"encoder_on_output",1)==OK);
    count=0;assert(api->enable(id)==OK);count=280;tick(id,20);
    assert(info(id).encoder_counts_per_rev==280 && fabsf(info(id).position-(float)(2*PI))<.0001f);
    assert(api->disable(id)==OK && api->configure(id,"encoder_on_output",0)==OK);
    assert(api->configure(id,"kp",1)==OK && api->configure(id,"ki",0)==OK);
    assert(api->configure(id,"acceleration_limit",1000)==OK);
    decoding=1;attach_encoder(1);count=0;assert(api->enable(id)==OK && api->command(id,1)==OK);
    count=7;tick(id,20);
    assert(fabsf(info(id).velocity-(float)(2*PI*7/(2100*.02)))<.0001f);
    assert(fabsf(info(id).p-(1-info(id).velocity))<.0001f && !info(id).saturated);
    assert(api->close(id)==OK);
    assert(api->create("pan",SOLAR_OS_ROBOT_SERVO_POSITION,&id)==OK);
    assert(!solar_os_controller_count());
    solar_os_native_servo_config_v1_t servo={.provider="native",.output=2,.frequency_hz=50,
        .pulse_min_us=1000,.pulse_max_us=2000,.angle_min=-2,.angle_max=2};
    assert(api->bind_servo(id,&servo)==OK);
    assert(api->configure(id,"gear_ratio",2)==OK && api->configure(id,"motor_polarity",-1)==OK);
    assert(api->command(id,.5f)==OK && api->enable(id)==OK);
    assert(!solar_os_controller_count()); /* Servo conversion has no PID. */
    tick(id,20); assert(servo_angle==-1 && !info(id).position_valid && !info(id).velocity_valid);
    assert(api->command(id,2)==ARG);
    for (int i=0;i<15;i++) tick(id,20);
    assert(info(id).state==SOLAR_OS_ROBOT_WAITING && servo_held);
    assert(api->command(id,.25f)==OK);tick(id,20);assert(servo_angle==-.5f);
    tick(id,400); assert(info(id).state==SOLAR_OS_ROBOT_FAULT);
    assert(api->close(id)==OK && !servo_held && !allocations);
    assert(api->create("failed_enable",SOLAR_OS_ROBOT_MOTOR_VELOCITY,&id)==OK);
    assert(api->configure(id,"ppr",70)==OK && api->configure(id,"gear_ratio",30)==OK);
    assert(api->bind_motor(id,"left","enc")==OK);
    fail_stop=true;
    assert(api->enable(id)==STATE && motor_held() && solar_os_controller_count()==1);
    solar_os_controller_info_t controller;
    assert(solar_os_controller_find("failed_enable",&controller)==ESP_OK && controller.faulted && !controller.enabled);
    assert(api->close(id)==STATE && solar_os_controller_count()==1);
    fail_stop=false;
    assert(api->close(id)==OK && !solar_os_controller_count() && !allocations);
    assert(joins>=7); assert(solar_os_robot_engine_shutdown()==OK);
    assert(solar_os_motor_unregister("left")==ESP_OK && solar_os_encoder_unregister("enc")==ESP_OK && !device_allocations);
    puts("robot joints: calibration, polarity, PPR decoding, wrap, deadband/feedforward, load control, deadlines and cleanup passed");
    return 0;
}
