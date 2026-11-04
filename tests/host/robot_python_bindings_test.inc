/* Exercise the production binding functions and native service. Only the
 * MicroPython object primitives are substituted; firmware builds compile the
 * same functions against the real MicroPython runtime. */
#define main robot_registry_suite
#include "robot_integration_test.c"
#undef main
#include <setjmp.h>

typedef struct object *mp_obj_t;
enum { OBJECT_NONE, OBJECT_NUMBER, OBJECT_STRING, OBJECT_DICT, OBJECT_LIST };
struct object {
    int type;
    double number;
    char text[80];
    const char *keys[64];
    mp_obj_t values[64];
    size_t count;
};
static struct object pool[4096], none;
static unsigned used;
static mp_obj_t mp_const_none = &none;
static jmp_buf error_jump;
static esp_err_t last_error;
static void python_check_esp(esp_err_t error)
{ if (error != ESP_OK) { last_error = error; longjmp(error_jump, 1); } }
static mp_obj_t object(int type)
{ assert(used < 4096); mp_obj_t o = &pool[used++]; o->type = type; return o; }
static mp_obj_t number(double value) { mp_obj_t o = object(OBJECT_NUMBER); o->number = value; return o; }
static mp_obj_t string(const char *value)
{ mp_obj_t o = object(OBJECT_STRING); assert(strlen(value) < sizeof(o->text)); strcpy(o->text, value); return o; }
static mp_obj_t mp_obj_new_str(const char *value, size_t bytes)
{ char text[80]; assert(bytes<sizeof(text)); memcpy(text,value,bytes);text[bytes]=0;return string(text); }
static const char *mp_obj_str_get_str(mp_obj_t value)
{ if (value->type != OBJECT_STRING) python_check_esp(ESP_ERR_INVALID_ARG); return value->text; }
static double mp_obj_get_float(mp_obj_t value)
{ if (value->type != OBJECT_NUMBER) python_check_esp(ESP_ERR_INVALID_ARG); return value->number; }
static mp_obj_t mp_obj_new_dict(size_t capacity) { (void)capacity; return object(OBJECT_DICT); }
static mp_obj_t mp_obj_new_list(size_t n, const mp_obj_t *items)
{ (void)n; (void)items; return object(OBJECT_LIST); }
static void mp_obj_list_append(mp_obj_t list, mp_obj_t value)
{ assert(list->count < 64); list->values[list->count++] = value; }
static void store(mp_obj_t dict, const char *key, mp_obj_t value)
{ assert(dict->count < 64); dict->keys[dict->count] = key; dict->values[dict->count++] = value; }
static void python_dict_store_cstr(mp_obj_t d, const char *k, const char *v) { store(d, k, string(v)); }
static void python_dict_store_bool(mp_obj_t d, const char *k, bool v) { store(d, k, number(v)); }
static void python_dict_store_uint(mp_obj_t d, const char *k, uint32_t v) { store(d, k, number(v)); }
static void python_dict_store_float(mp_obj_t d, const char *k, float v) { store(d, k, number(v)); }
static mp_obj_t field(mp_obj_t dict, const char *key)
{ for (size_t i = 0; i < dict->count; i++) if (!strcmp(dict->keys[i], key)) return dict->values[i]; assert(false); return NULL; }

/* MOTOR_BINDINGS */

static void python_dict_store_int(mp_obj_t d, const char *k, int v) { store(d, k, number(v)); }
static mp_obj_t mp_obj_new_float(double v) { return number(v); }
static mp_obj_t mp_obj_new_int_from_ull(unsigned long long v) { return number((double)v); }
static const char *python_key(const char *k) { return k; }
static void mp_obj_dict_store(mp_obj_t d, const char *k, mp_obj_t v) { store(d, k, v); }
#define MP_ERROR_TEXT(text) text
#define mp_raise_ValueError(text) ((void)(text), python_check_esp(ESP_ERR_INVALID_ARG))
#define MP_DEFINE_CONST_FUN_OBJ_0(...)
#define MP_DEFINE_CONST_FUN_OBJ_1(...)
#define MP_DEFINE_CONST_FUN_OBJ_2(...)
#define MP_DEFINE_CONST_FUN_OBJ_3(...)
#define MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(...)

static mp_obj_t python_u64_to_obj(uint64_t v) { return number((double)v); }
static uint32_t python_u32_from_obj(mp_obj_t v) { double n=mp_obj_get_float(v);if(!isfinite(n)||n<0||n>UINT32_MAX||floor(n)!=n)python_check_esp(ESP_ERR_INVALID_ARG);return (uint32_t)n; }
#define SOLAR_OS_PACKAGE_SERVICE_ROBOT 1
#define SOLAR_OS_LOGW(...) ((void)0)
#define TAG "test"
const char *esp_err_to_name(esp_err_t e) { (void)e;return "error"; }
#include "solar_os_python_robot.inc"
int main(void)
{
    setup();
    mp_obj_t model_info=solaros_robot_model_load(string(ROBOT_MODEL_FIXTURE));
    assert(!strcmp(field(model_info,"name")->text,"solbot"));
    assert(field(model_info,"links")->count==5 && field(model_info,"joints")->count==4);
    assert(field(model_info,"known_joints")->number==2 && !workers);
    if (!setjmp(error_jump)) {
        (void)solaros_robot_model_transform(string("base_link"),string("left_wheel_link"));assert(false);
    } else assert(last_error==ESP_ERR_INVALID_STATE);
    solaros_robot_model_set_joint(string("left_wheel_joint"),number(0));
    mp_obj_t transform=solaros_robot_model_transform(string("base_link"),string("left_wheel_link"));
    assert(fabs(field(transform,"translation")->values[1]->number-.0350641)<1e-6);
    mp_obj_t point_args[]={string("base_link"),string("left_wheel_link"),number(0),number(0),number(0)};
    assert(fabs(solaros_robot_model_point(5,point_args)->values[1]->number-.0350641)<1e-6);
    assert(solaros_robot_model_vector(5,point_args)->values[1]->number==0);
    python_robot_destroy();assert(leases==1); /* Shared model survives interpreter cleanup. */
    solaros_robot_model_unload();assert(!leases);
    mp_obj_t name=string("wheel"),args[]={name};
    mp_obj_t s=solaros_robot_joint_create(1,args);
    assert(!strcmp(field(s,"state")->text,"ready"));
    solaros_robot_joint_configure(name,string("ppr"),number(70));
    solaros_robot_joint_configure(name,string("gear_ratio"),number(30));
    solaros_robot_joint_configure(name,string("motor_polarity"),number(-1));
    solaros_robot_joint_configure(name,string("friction_bias_forward"),number(.15));
    solaros_robot_joint_configure(name,string("friction_bias_reverse"),number(.12));
    solaros_robot_joint_configure(name,string("velocity_deadband"),number(.05));
    solaros_robot_joint_bind_motor(name,string("left"),string("left_enc"));
    solaros_robot_joint_enable(name);
    assert(!strcmp(field(solaros_robot_joint_status(name),"state")->text,"waiting"));
    solaros_robot_joint_command(name,number(1));
    s=solaros_robot_joint_status(name);assert(!strcmp(field(s,"state")->text,"active"));
    assert(field(s,"motor_polarity")->number==-1);
    assert(field(s,"encoder_counts_per_rev")->number==2100);
    assert(fabs(field(s,"friction_bias_forward")->number-.15)<1e-6);
    assert(fabs(field(s,"friction_bias_reverse")->number-.12)<1e-6);
    solaros_robot_joint_command(name,number(.01));
    assert(field(solaros_robot_joint_status(name),"target")->number==0);
    assert(solaros_robot_joints()->count==1);
    solaros_robot_joint_disable(name);solaros_robot_joint_close(name);
    mp_obj_t servo[]={string("pan"),string("servo")};
    solaros_robot_joint_create(2,servo);
    mp_obj_t calibration[]={servo[0],string("native"),number(2),number(1000),number(2000),number(-1),number(1)};
    solaros_robot_joint_bind_servo(7,calibration);
    python_robot_destroy();
    assert(!solar_os_robot_joint_count() && !leases);
    assert(solar_os_robot_model_load_xml(model_velocity_fixture,strlen(model_velocity_fixture),NULL,0)==ESP_OK);
    solaros_robot_start();assert(workers==1);
    python_robot_destroy();assert(workers==1); /* Model controllers are shared. */
    solaros_robot_stop();assert(!workers && !solar_os_robot_joint_count());
    solaros_robot_model_unload();
    teardown();puts("Python robot wrappers and interpreter cleanup passed");return 0;
}
