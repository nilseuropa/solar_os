/* The production bindings and engine run unchanged. MicroPython object
 * primitives are substituted here; firmware builds use the real runtime. */
#include "solar_os_aogmaneo.h"
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct object *mp_obj_t;
typedef int32_t mp_int_t;
typedef struct { mp_obj_t key, value; } mp_map_elem_t;
typedef struct { size_t used, alloc; mp_map_elem_t *table; } mp_map_t;
enum { NONE, NUMBER, STRING, ARRAY, DICT };
struct object {
    int type;
    double number;
    char *bytes;
    size_t length;
    mp_obj_t items[256];
    mp_map_t map;
    mp_map_elem_t fields[16];
};
static void *allocations[16384];
static unsigned allocation_count;
static void *allocate(size_t bytes)
{
    assert(allocation_count < 16384);
    void *p = calloc(1, bytes ? bytes : 1); assert(p);
    allocations[allocation_count++] = p; return p;
}
static struct object none;
static mp_obj_t mp_const_none = &none;
static jmp_buf error_jump;
static bool expected_error, cancelled;
static int mp_type_MemoryError, mp_type_KeyboardInterrupt, mp_type_RuntimeError;
static _Noreturn void error(const char *message)
{
    if (!expected_error) { fprintf(stderr, "%s\n", message); abort(); }
    longjmp(error_jump, 1);
}
#define EXPECT_ERROR(expression) do { expected_error = true; \
    if (!setjmp(error_jump)) { (void)(expression); assert(!"expected an exception"); } \
    expected_error = false; } while (0)
static mp_obj_t object(int type)
{ mp_obj_t o = allocate(sizeof(*o)); o->type = type; return o; }
static mp_obj_t number(double value) { mp_obj_t o = object(NUMBER); o->number = value; return o; }
static mp_obj_t bytes(const void *data, size_t count)
{
    mp_obj_t o = object(STRING); o->length = count;
    o->bytes = allocate(count + 1); memcpy(o->bytes, data, count); return o;
}
static mp_obj_t string(const char *s) { return bytes(s, strlen(s)); }
static mp_obj_t array(size_t count, const mp_obj_t *items)
{
    assert(count <= 256);
    mp_obj_t o = object(ARRAY); o->length = count;
    if (items) memcpy(o->items, items, count * sizeof(*items));
    return o;
}
static mp_obj_t dictionary(void)
{
    mp_obj_t o = object(DICT); o->map.table = o->fields; o->map.alloc = 16; return o;
}
static void store(mp_obj_t o, const char *key, mp_obj_t value)
{
    assert(o->type == DICT && o->map.used < 16);
    o->fields[o->map.used++] = (mp_map_elem_t){string(key), value};
}
static bool mp_map_slot_is_filled(mp_map_t *map, size_t index) { return map->table[index].key != NULL; }
static const char *mp_obj_str_get_data(mp_obj_t o, size_t *count)
{ if (!o || o->type != STRING) error("expected string"); *count = o->length; return o->bytes; }
static mp_map_t *mp_obj_dict_get_map(mp_obj_t o)
{ if (!o || o->type != DICT) error("expected dict"); return &o->map; }
static mp_obj_t python_kw_value(mp_map_t *map, const char *name)
{
    if (map) for (size_t i = 0; i < map->used; ++i)
        if (!strcmp(map->table[i].key->bytes, name)) return map->table[i].value;
    return NULL;
}
static mp_int_t mp_obj_get_int(mp_obj_t o)
{
    if (!o || o->type != NUMBER || !isfinite(o->number) || floor(o->number) != o->number ||
        o->number < INT32_MIN || o->number > INT32_MAX) error("expected integer");
    return (mp_int_t)o->number;
}
static double mp_obj_get_float(mp_obj_t o)
{ if (!o || o->type != NUMBER) error("expected number"); return o->number; }
static bool mp_obj_is_true(mp_obj_t o) { return o && o != mp_const_none && (o->type != NUMBER || o->number); }
static void mp_obj_get_array(mp_obj_t o, size_t *count, mp_obj_t **items)
{ if (!o || o->type != ARRAY) error("expected array"); *count = o->length; *items = o->items; }
static mp_obj_t mp_obj_new_list(size_t n, const mp_obj_t *items) { return array(n, items); }
static void mp_obj_list_append(mp_obj_t o, mp_obj_t item)
{ assert(o->type == ARRAY && o->length < 256); o->items[o->length++] = item; }
static mp_obj_t mp_obj_new_int(int32_t n) { return number(n); }
static mp_obj_t mp_obj_new_float(float n) { return number(n); }
static mp_obj_t mp_obj_new_bytes(const uint8_t *data, size_t count) { return bytes(data, count); }
typedef struct { void *buf; size_t len; } mp_buffer_info_t;
static void mp_get_buffer_raise(mp_obj_t o, mp_buffer_info_t *buffer, int flags)
{ (void)flags; if (!o || o->type != STRING) error("expected bytes"); buffer->buf = o->bytes; buffer->len = o->length; }
static uint32_t python_u32_from_obj(mp_obj_t o)
{
    double n = mp_obj_get_float(o);
    if (!isfinite(n) || n < 0 || n > UINT32_MAX || floor(n) != n) error("invalid uint32");
    return (uint32_t)n;
}
static _Noreturn void mp_raise_type(const int *type) { (void)type; error("runtime exception"); }
static _Noreturn void mp_raise_msg_varg(const int *type, const char *format, ...)
{ (void)type; error(format); }
#define mp_raise_ValueError error
#define mp_raise_TypeError error
#define MP_ERROR_TEXT(text) text
#define MP_OBJ_NULL NULL
#define MP_BUFFER_READ 1
#define m_new(type, count) ((type *)allocate((count) * sizeof(type)))
#define MP_DEFINE_CONST_FUN_OBJ_KW(...)
#define MP_DEFINE_CONST_FUN_OBJ_1(...)
#define MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(...)
static struct { solar_os_aogmaneo_context_t aogmaneo; } runtime;
static __typeof__(runtime) *python_runtime_state = &runtime;
static bool python_should_cancel(void *user) { (void)user; return cancelled; }
static void test_delay(unsigned ticks) { (void)ticks; }
#define vTaskDelay test_delay
#define pdMS_TO_TICKS(n) (n)
#define SOLAR_OS_PACKAGE_SERVICE_AOGMANEO 1
#include "solar_os_python_aogmaneo.inc"
extern int aon_test_leases, aon_test_missing, aon_test_release_failures, aon_test_destroy_failures;
extern void aon_test_clean(void);

static mp_obj_t shape(int x, int y, int z)
{ mp_obj_t v[] = {number(x), number(y), number(z)}; return array(3, v); }
int main(void)
{
    mp_obj_t io = dictionary(), layer = dictionary(), action = dictionary();
    store(io, "size", shape(1, 1, 4)); store(layer, "size", shape(2, 2, 8));
    store(action, "size", shape(1, 1, 4)); store(action, "type", number(2));
    store(action, "value_size", number(8)); store(action, "history_capacity", number(4));
    mp_obj_t inputs[] = {io, action}, layers[] = {layer};
    mp_obj_t create_args[] = {array(2, inputs), array(1, layers)};
    mp_obj_t kwargs = dictionary(); store(kwargs, "seed", number(12345));
    aon_test_missing = 1;
    EXPECT_ERROR(solaros_aogmaneo_create(2, create_args, &kwargs->map));
    assert(!aon_test_leases); aon_test_missing = 0;
    mp_obj_t model = solaros_aogmaneo_create(2, create_args, &kwargs->map);
    mp_obj_t read_args[] = {model, number(0)};
    EXPECT_ERROR(solaros_aogmaneo_prediction(1, read_args));
    for (int tick = 0; tick < 40; ++tick) {
        mp_obj_t code = number(tick % 4), values = array(1, &code);
        mp_obj_t data[] = {values, values};
        mp_obj_t args[] = {model, array(2, data)};
        solaros_aogmaneo_step(2, args, NULL);
    }
    assert(solaros_aogmaneo_prediction(1, read_args)->length == 1);
    assert(solaros_aogmaneo_activations(1, read_args)->length == 4);
    assert(solaros_aogmaneo_hidden(1, read_args)->length == 4);
    EXPECT_ERROR(solaros_aogmaneo_values(1, read_args));
    read_args[1] = number(1);
    assert(solaros_aogmaneo_values(2, read_args)->length == 1);
    read_args[1] = number(9); EXPECT_ERROR(solaros_aogmaneo_prediction(2, read_args));
    solaros_aogmaneo_clear(model); EXPECT_ERROR(solaros_aogmaneo_prediction(1, read_args));
    mp_obj_t bad_value = number(4), bad_codes = array(1, &bad_value), bad_inputs[] = {bad_codes, bad_codes};
    mp_obj_t bad_step[] = {model, array(2, bad_inputs)};
    EXPECT_ERROR(solaros_aogmaneo_step(2, bad_step, NULL));
    store(kwargs, "typo", number(1)); EXPECT_ERROR(solaros_aogmaneo_create(2, create_args, &kwargs->map));
    kwargs = dictionary(); store(kwargs, "inputs", create_args[0]);
    EXPECT_ERROR(solaros_aogmaneo_create(2, create_args, &kwargs->map));
    mp_obj_t visible = dictionary(); store(visible, "size", shape(4, 4, 1)); store(visible, "radius", number(1));
    mp_obj_t image_args[] = {shape(2, 2, 8), array(1, &visible)};
    mp_obj_t image = solaros_aogmaneo_image_create(2, image_args, NULL);
    uint8_t pixels[16] = {0, 128, 255, 64};
    mp_obj_t raw = bytes(pixels, 16), step_args[] = {image, array(1, &raw)};
    mp_obj_t codes = solaros_aogmaneo_image_step(2, step_args, NULL);
    assert(codes->length == 4);
    mp_obj_t recon_args[] = {image, codes};
    assert(solaros_aogmaneo_image_reconstruct(2, recon_args)->length == 16);
    EXPECT_ERROR(solaros_aogmaneo_clear(image));
    raw = string("short"); step_args[1] = array(1, &raw);
    EXPECT_ERROR(solaros_aogmaneo_image_step(2, step_args, NULL));
    solaros_aogmaneo_destroy(image); EXPECT_ERROR(solaros_aogmaneo_image_step(2, step_args, NULL));
    aon_test_destroy_failures = 1; EXPECT_ERROR(solaros_aogmaneo_destroy(model));
    assert(aon_test_leases == 1);
    aon_test_release_failures = 1; EXPECT_ERROR(solaros_aogmaneo_destroy(model));
    assert(aon_test_leases == 1); solaros_aogmaneo_destroy(model);
    EXPECT_ERROR(solaros_aogmaneo_destroy(model));
    mp_obj_t next = solaros_aogmaneo_create(2, create_args, NULL); assert(next->number != model->number);
    EXPECT_ERROR(solaros_aogmaneo_clear(model));
    cancelled = true; EXPECT_ERROR(solaros_aogmaneo_clear(next)); cancelled = false;
    assert(!python_aogmaneo_active());
    aon_test_destroy_failures = 1; python_aogmaneo_cleanup(); aon_test_clean();
    for (unsigned i = 0; i < allocation_count; ++i) free(allocations[i]);
    puts("Python AOgmaNeo marshaling with real engine and failure cleanup passed");
}
