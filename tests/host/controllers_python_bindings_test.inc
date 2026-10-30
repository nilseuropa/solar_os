/* Exercise the production binding functions and native service. Only the
 * MicroPython object primitives are substituted; firmware builds compile the
 * same functions against the real MicroPython runtime. */
#define main controllers_registry_suite
#include "controllers_test.c"
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
static mp_obj_t python_u64_to_obj(uint64_t v) { return number((double)v); }
static const char *python_key(const char *k) { return k; }
static void mp_obj_dict_store(mp_obj_t d, const char *k, mp_obj_t v) { store(d, k, v); }
#define MP_ERROR_TEXT(text) text
#define mp_raise_ValueError(text) ((void)(text), python_check_esp(ESP_ERR_INVALID_ARG))
#define MP_DEFINE_CONST_FUN_OBJ_0(...)
#define MP_DEFINE_CONST_FUN_OBJ_1(...)
#define MP_DEFINE_CONST_FUN_OBJ_2(...)
#define MP_DEFINE_CONST_FUN_OBJ_3(...)
#define MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(...)
#define SOLAR_OS_PACKAGE_SERVICE_CONTROLLERS 1
#include "solar_os_python_controllers.inc"

int main(void)
{
    register_output();
    mp_obj_t name = string("loop"), args[] = {name};
    mp_obj_t record = solaros_controllers_create(1, args);
    assert(strcmp(field(record, "kind")->text, "pid") == 0);
    assert(field(record, "ki")->number > 0);
    assert(solaros_controllers_list()->count == 1);
    solaros_controllers_configure(name, string("setpoint"), number(1));
    mp_obj_t output = solaros_controllers_step(name, number(0.5), number(0.02));
    assert(output->number > 0.5 && output->number < 0.51);
    solaros_controllers_manual(name, number(0.4));
    solaros_controllers_auto(name);
    solaros_controllers_bind(name, string("manual"), string("parameter:plant.drive"));
    now = 10;
    solaros_controllers_feed(name, number(0.3));
    solaros_controllers_start(name);
    now = 30; solar_os_controllers_tick(now);
    assert(field(solaros_controllers_status(name), "enabled")->number);
    solaros_controllers_stop(name);
    solaros_controllers_reset(name);
    solaros_controllers_delete(name);
    assert(solaros_controllers_list()->count == 0);
    if (setjmp(error_jump) == 0) { solaros_controllers_status(name); assert(false); }
    assert(last_error == ESP_ERR_NOT_FOUND);
    mp_obj_t bad[] = {string("bad"), string("unknown")};
    if (setjmp(error_jump) == 0) { solaros_controllers_create(2, bad); assert(false); }
    assert(last_error == ESP_ERR_INVALID_ARG);
    puts("Python controller bindings passed");
    return 0;
}
