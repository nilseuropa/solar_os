/* Native service and production C bindings; MicroPython object primitives
 * are substituted. Firmware builds also compile against real MicroPython. */
#define main encoder_registry_suite
#include "encoder_service_test.c"
#undef main
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include "encoder_binding_expansion_test.inc"

typedef struct object *mp_obj_t;
typedef struct { mp_obj_t key, value; } mp_map_elem_t;
typedef struct { size_t alloc; mp_map_elem_t table[24]; } mp_map_t;
enum { OBJECT_NONE, OBJECT_NUMBER, OBJECT_STRING, OBJECT_DICT, OBJECT_LIST };
typedef struct object {
    int type;
    double number;
    char text[80];
    mp_map_t map;
    mp_obj_t values[24];
    size_t count;
} mp_obj_dict_t;
static struct object pool[512], none;
static unsigned used;
static mp_obj_t mp_const_none = &none;
static int mp_type_dict, mp_type_ValueError;
static jmp_buf error_jump;
static esp_err_t last_error;
#define MP_OBJ_NULL NULL
#define MP_OBJ_TO_PTR(o) (o)
#define MP_ERROR_TEXT(s) (s)
static void python_check_esp(esp_err_t error)
{ if (error != ESP_OK) { last_error = error; longjmp(error_jump, 1); } }
static void __attribute__((noreturn)) python_raise_esp(esp_err_t error)
{ last_error = error; longjmp(error_jump, 1); }
static void __attribute__((noreturn)) mp_raise_ValueError(const char *message)
{ (void)message; last_error = ESP_ERR_INVALID_ARG; longjmp(error_jump, 1); }
static void __attribute__((noreturn)) mp_raise_TypeError(const char *message) { mp_raise_ValueError(message); }
static void __attribute__((noreturn)) mp_raise_msg_varg(const int *type, const char *message, ...)
{ (void)type; mp_raise_ValueError(message); }
static mp_obj_t object(int type)
{ assert(used < 512); mp_obj_t o = &pool[used++]; o->type = type; return o; }
static mp_obj_t mp_obj_new_int(int32_t value) { mp_obj_t o = object(OBJECT_NUMBER); o->number = value; return o; }
static mp_obj_t string(const char *value)
{ mp_obj_t o = object(OBJECT_STRING); assert(strlen(value) < sizeof(o->text)); strcpy(o->text, value); return o; }
static const char *mp_obj_str_get_str(mp_obj_t value)
{ if (value->type != OBJECT_STRING) mp_raise_TypeError("string required"); return value->text; }
static int mp_obj_get_int(mp_obj_t value)
{
    if (value->type != OBJECT_NUMBER || !isfinite(value->number) || value->number < INT_MIN || value->number > INT_MAX)
        mp_raise_TypeError("integer required");
    return (int)value->number;
}
static bool mp_obj_is_type(mp_obj_t value, const int *type)
{ (void)type; return value->type == OBJECT_DICT; }
static bool mp_map_slot_is_filled(mp_map_t *map, size_t i) { return map->table[i].key != NULL; }
static mp_obj_t mp_obj_new_dict(size_t capacity) { (void)capacity; return object(OBJECT_DICT); }
static mp_obj_t mp_obj_new_list(size_t n, const mp_obj_t *items)
{ (void)n; (void)items; return object(OBJECT_LIST); }
static void mp_obj_list_append(mp_obj_t list, mp_obj_t value)
{ assert(list->count < 24); list->values[list->count++] = value; }
static void store(mp_obj_t dict, const char *key, mp_obj_t value)
{ assert(dict->map.alloc < 24); dict->map.table[dict->map.alloc++] = (mp_map_elem_t){string(key), value}; }
static void python_dict_store_cstr(mp_obj_t d, const char *k, const char *v) { store(d, k, string(v)); }
static void python_dict_store_bool(mp_obj_t d, const char *k, bool v) { store(d, k, mp_obj_new_int(v)); }
static void python_dict_store_int(mp_obj_t d, const char *k, int32_t v) { store(d, k, mp_obj_new_int(v)); }
static mp_obj_t python_get_dict_obj(mp_obj_t dict, const char *key, bool required)
{
    for (size_t i = 0; i < dict->map.alloc; i++) if (!strcmp(dict->map.table[i].key->text, key)) return dict->map.table[i].value;
    if (required) mp_raise_ValueError("missing key");
    return MP_OBJ_NULL;
}

/* ENCODER_BINDINGS */

#define SOLAR_OS_PACKAGE_SERVICE_ENCODER 1
typedef struct { const char *names[8]; size_t count; } shell_completion_match_t;
static void shell_completion_emit(shell_completion_match_t *state, const char *name)
{
    /* Copy caller-owned metadata before the emitter's stack is reused. */
    static char names[8][SOLAR_OS_ENCODER_NAME_MAX];
    assert(state->count < 8);
    strlcpy(names[state->count], name, sizeof(names[0]));
    state->names[state->count] = names[state->count];
    state->count++;
}
/* ENCODER_COMPLETION */

int main(void)
{
    fake_t a = {.value = INT32_MIN, .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    fake_t b = {.value = INT32_MAX, .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const solar_os_encoder_ops_t full = {.read = read_count, .reset = reset_count};
    const solar_os_encoder_ops_t basic = {.read = read_count};
    const solar_os_encoder_registration_t one = {.name = "left", .driver = "pcnt", .mode = 4, .ops = &full, .ctx = &a};
    const solar_os_encoder_registration_t two = {.name = "right", .driver = "external", .mode = 2, .ops = &basic, .ctx = &b};
    assert(solar_os_encoder_register(&one) == ESP_OK && solar_os_encoder_register(&two) == ESP_OK);
    shell_completion_match_t completion = {0};
    shell_completion_emit_encoder_names(&completion);
    assert(completion.count == 2 && !strcmp(completion.names[0], "left") && !strcmp(completion.names[1], "right"));
    mp_obj_t name = string("left");
    mp_obj_t list = solaros_encoder_list();
    assert(list->count == 2 && !strcmp(python_get_dict_obj(list->values[1], "driver", true)->text, "external"));
    mp_obj_t s = solaros_encoder_status(name);
    assert(s->map.alloc == 5 && python_get_dict_obj(s, "mode", true)->number == 4);
    assert(python_get_dict_obj(s, "count", true)->number == INT32_MIN);
    assert(solaros_encoder_read(string("right"))->number == INT32_MAX);
    assert(solaros_encoder_reset(name) == mp_const_none && solaros_encoder_read(name)->number == 0);
    if (!setjmp(error_jump)) { (void)solaros_encoder_reset(string("right")); assert(false); }
    assert(last_error == ESP_ERR_NOT_SUPPORTED);
    if (!setjmp(error_jump)) { (void)solaros_encoder_read(string("missing")); assert(false); }
    assert(last_error == ESP_ERR_NOT_FOUND);
    if (!setjmp(error_jump)) { (void)solaros_encoder_status(mp_const_none); assert(false); }
    assert(last_error == ESP_ERR_INVALID_ARG);
    mp_obj_t config = mp_obj_new_dict(6);
    python_dict_store_int(config, "a", 1); python_dict_store_int(config, "b", 2);
    python_dict_store_int(config, "mode", 4); python_dict_store_int(config, "invert", 1);
    python_dict_store_int(config, "pull", 2); python_dict_store_int(config, "filter_ns", 1000);
    assert(solaros_expansion_attach(string("pcnt-encoder"), string("wheel"), config) == mp_const_none);
    check_attachment();
    python_dict_store_int(config, "typo", 4);
    if (!setjmp(error_jump)) { (void)solaros_expansion_attach(string("pcnt-encoder"), string("wheel"), config); assert(false); }
    assert(last_error == ESP_ERR_INVALID_ARG);
    assert(solar_os_encoder_unregister("left") == ESP_OK && solar_os_encoder_unregister("right") == ESP_OK);
    assert(solaros_encoder_list()->count == 0 && !allocations);
    puts("Python encoder bindings: records, signed bounds, errors and expansion encoding PASS");
    return 0;
}
