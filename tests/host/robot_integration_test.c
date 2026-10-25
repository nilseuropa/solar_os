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

static uint64_t time_value=100000;
static int32_t encoder_count;
static float motor_value;
static bool fail_coast, fail_release;
static unsigned device_allocations, provider_allocations, leases, workers;
static const solar_os_robot_api_v3_t *provider;

int64_t esp_timer_get_time(void) { return time_value; }
static void *provider_alloc(size_t n) { void *p=calloc(1,n); if (p) provider_allocations++; return p; }
static void provider_free(void *p) { if(p) { provider_allocations--;free(p); } }
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t cls,const char *tag)
{ (void)cls;(void)tag;void *p=calloc(n,size);if(p)device_allocations++;return p; }
void solar_os_memory_free(void *p) { if(p) { device_allocations--;free(p); } }
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
{ assert(!strcmp(name,"robot") && abi==SOLAR_OS_ROBOT_ABI && bytes==sizeof(*provider));*api=provider;*lease=++leases;return 0; }
int solar_os_native_library_release(uint32_t lease)
{ assert(lease && leases);if(fail_release)return STATE;leases--;return 0; }
static esp_err_t hw_status(void *ctx,solar_os_motor_status_t *s)
{ (void)ctx;*s=(solar_os_motor_status_t){.mode=motor_value?SOLAR_OS_MOTOR_MODE_FORWARD:SOLAR_OS_MOTOR_MODE_COAST,.drive_valid=true,.drive=motor_value};return ESP_OK; }
static esp_err_t hw_drive(void *ctx,float value) { (void)ctx;motor_value=value;return ESP_OK; }
static esp_err_t hw_coast(void *ctx) { (void)ctx;if(fail_coast)return ESP_FAIL;motor_value=0;return ESP_OK; }
static esp_err_t hw_read(void *ctx,int32_t *value) { (void)ctx;*value=encoder_count;return ESP_OK; }
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
    provider=solar_os_robot_engine_init(&mem,&time,&th,hw);assert(provider);
}
static void teardown(void)
{
    assert(!leases && !workers && !provider_allocations);
    assert(solar_os_robot_engine_shutdown()==0);
    assert(solar_os_motor_unregister("left")==ESP_OK && solar_os_encoder_unregister("left_enc")==ESP_OK);
    assert(!device_allocations);
}
int main(void)
{
    setup();
    assert(solar_os_robot_joint_create("wheel",SOLAR_OS_ROBOT_MOTOR_VELOCITY,"python")==ESP_OK && leases==1);
    assert(solar_os_robot_joint_configure("wheel","ppr",7)==ESP_OK);
    assert(solar_os_robot_joint_configure("wheel","gear_ratio",30)==ESP_OK);
    assert(solar_os_robot_joint_bind_motor("wheel","left","left_enc")==ESP_OK);
    assert(solar_os_robot_joint_enable("wheel")==ESP_OK && workers==1);
    assert(solar_os_motor_drive("left",.3f)==ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_reset("left_enc")==ESP_ERR_INVALID_STATE);
    assert(solar_os_motor_unregister("left")==ESP_ERR_INVALID_STATE);
    assert(solar_os_encoder_unregister("left_enc")==ESP_ERR_INVALID_STATE);
    assert(solar_os_robot_joint_command("wheel",1)==ESP_OK);
    time_value+=20000;encoder_count=1;
    lock();cycle(find(1));unlock();
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
    teardown();
    puts("robot integration: native device ABI, claims, encoder scaling, frontend leases and cleanup retry passed");
    return 0;
}
