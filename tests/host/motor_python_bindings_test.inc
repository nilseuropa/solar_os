/* Exercise the production binding functions and native service. Only the
 * MicroPython object primitives are substituted; firmware builds compile the
 * same functions against the real MicroPython runtime. */
#define main motor_registry_suite
#include "motor_service_test.c"
#undef main
#include <setjmp.h>

typedef struct object *mp_obj_t;
enum { OBJECT_NONE, OBJECT_NUMBER, OBJECT_STRING, OBJECT_DICT, OBJECT_LIST };
struct object {
    int type;
    double number;
    char text[80];
    const char *keys[24];
    mp_obj_t values[24];
    size_t count;
};
static struct object pool[512], none;
static unsigned used;
static mp_obj_t mp_const_none = &none;
static jmp_buf error_jump;
static esp_err_t last_error;
static void python_check_esp(esp_err_t error)
{ if (error != ESP_OK) { last_error = error; longjmp(error_jump, 1); } }
static mp_obj_t object(int type)
{ assert(used < 512); mp_obj_t o = &pool[used++]; o->type = type; return o; }
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
{ assert(list->count < 24); list->values[list->count++] = value; }
static void store(mp_obj_t dict, const char *key, mp_obj_t value)
{ assert(dict->count < 24); dict->keys[dict->count] = key; dict->values[dict->count++] = value; }
static void python_dict_store_cstr(mp_obj_t d, const char *k, const char *v) { store(d, k, string(v)); }
static void python_dict_store_bool(mp_obj_t d, const char *k, bool v) { store(d, k, number(v)); }
static void python_dict_store_uint(mp_obj_t d, const char *k, uint32_t v) { store(d, k, number(v)); }
static void python_dict_store_float(mp_obj_t d, const char *k, float v) { store(d, k, number(v)); }
static mp_obj_t field(mp_obj_t dict, const char *key)
{ for (size_t i = 0; i < dict->count; i++) if (!strcmp(dict->keys[i], key)) return dict->values[i]; assert(false); return NULL; }

/* MOTOR_BINDINGS */

int main(void)
{
    fake_t a = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    fake_t b = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const solar_os_motor_ops_t full = {.status = status, .drive = drive, .coast = coast, .brake = brake, .clear = clear};
    const solar_os_motor_ops_t basic = {.status = status, .drive = drive, .coast = coast};
    const solar_os_motor_registration_t one = {.name = "left", .driver = "voltage-backend", .ops = &full, .ctx = &a};
    const solar_os_motor_registration_t two = {.name = "right", .driver = "pwm-backend", .ops = &basic, .ctx = &b};
    assert(solar_os_motor_register(&one) == ESP_OK && solar_os_motor_register(&two) == ESP_OK);
    mp_obj_t name = string("left"), right = string("right");
    mp_obj_t list = solaros_motor_list();
    assert(list->count == 2 && !strcmp(field(list->values[1], "driver")->text, "pwm-backend"));
    assert(field(list->values[0], "can_brake")->number && !field(list->values[1], "can_clear")->number);
    assert(solaros_motor_drive(name, number(-0.5)) == mp_const_none && a.value == -0.5f);
    mp_obj_t value = solaros_motor_status(name);
    assert(value->count == 17 && !strcmp(field(value, "mode")->text, "reverse"));
    assert(field(value, "drive")->number == -0.5 && field(value, "drive_valid")->number);
    assert(!field(value, "voltage_valid")->number && !field(value, "raw_valid")->number);
    assert(solaros_motor_brake(name) == mp_const_none && a.brakes == 1);
    assert(solaros_motor_clear(name) == mp_const_none && a.clears == 1);
    assert(solaros_motor_coast(right) == mp_const_none && b.value == 0);
    const double invalid[] = {1.1, -1.1, NAN, INFINITY};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        if (setjmp(error_jump) == 0) { (void)solaros_motor_drive(name, number(invalid[i])); assert(false); }
        assert(last_error == ESP_ERR_INVALID_ARG);
    }
    if (setjmp(error_jump) == 0) { (void)solaros_motor_brake(right); assert(false); }
    assert(last_error == ESP_ERR_NOT_SUPPORTED);
    if (setjmp(error_jump) == 0) { (void)solaros_motor_status(string("missing")); assert(false); }
    assert(last_error == ESP_ERR_NOT_FOUND);
    if (setjmp(error_jump) == 0) { (void)solaros_motor_drive(name, string("bad")); assert(false); }
    assert(last_error == ESP_ERR_INVALID_ARG);
    assert(solar_os_motor_unregister("left") == ESP_OK && solar_os_motor_unregister("right") == ESP_OK);
    assert(solaros_motor_list()->count == 0 && allocations == 0);
    puts("Python motor binding functions: records, two providers, normalized controls and errors PASS");
}
