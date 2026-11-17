#include "script_agent_test_support.inc"
#include <setjmp.h>

typedef struct object *mp_obj_t;
typedef int64_t mp_int_t;
enum { NONE, NUMBER, STRING, DICT, LIST };
struct object {
    int type;
    int64_t number;
    char text[256];
    size_t length, count;
    const char *keys[40];
    mp_obj_t values[40];
};
static struct object pool[1024], none;
static unsigned used;
static mp_obj_t mp_const_none = &none;
typedef struct { jmp_buf jump; void *ret_val; } nlr_buf_t;
static nlr_buf_t *active_nlr;
static jmp_buf error_jump;
static bool fail_python_alloc;
#define nlr_push(buffer) (active_nlr = (buffer), setjmp((buffer)->jump))
static void nlr_pop(void) { active_nlr = NULL; }
static _Noreturn void nlr_jump(void *value) { (void)value; active_nlr = NULL; longjmp(error_jump, 1); }
static _Noreturn void python_error(void)
{
    if (active_nlr) { active_nlr->ret_val = NULL; longjmp(active_nlr->jump, 1); }
    longjmp(error_jump, 1);
}
static void python_check_esp(esp_err_t error) { if (error) python_error(); }
static mp_obj_t object(int type)
{
    if (fail_python_alloc) { fail_python_alloc = false; python_error(); }
    assert(used < 1024);
    mp_obj_t value = &pool[used++]; value->type = type; return value;
}
static mp_obj_t number(int64_t value) { mp_obj_t out = object(NUMBER); out->number = value; return out; }
static mp_obj_t string(const char *value)
{ mp_obj_t out = object(STRING); out->length = strlen(value); assert(out->length < sizeof(out->text)); strcpy(out->text, value); return out; }
static const char *mp_obj_str_get_data(mp_obj_t value, size_t *length)
{ if (value->type != STRING) python_error(); *length = value->length; return value->text; }
static bool mp_obj_is_str(mp_obj_t value) { return value->type == STRING; }
static void mp_obj_get_array(mp_obj_t value, size_t *count, mp_obj_t **items)
{ if (value->type != LIST) python_error(); *count = value->count; *items = value->values; }
static mp_int_t mp_obj_get_int(mp_obj_t value)
{ if (value->type != NUMBER) python_error(); return value->number; }
static mp_obj_t mp_obj_new_dict(size_t capacity) { assert(capacity <= 40); return object(DICT); }
static mp_obj_t mp_obj_new_list(size_t count, const mp_obj_t *items)
{ assert(!count && !items); return object(LIST); }
static void mp_obj_list_append(mp_obj_t list, mp_obj_t value) { assert(list->count < 40); list->values[list->count++] = value; }
static void store(mp_obj_t dict, const char *key, mp_obj_t value)
{ assert(dict->count < 40); dict->keys[dict->count] = key; dict->values[dict->count++] = value; }
static void python_dict_store_cstr(mp_obj_t dict, const char *key, const char *value) { store(dict, key, string(value)); }
static void python_dict_store_bool(mp_obj_t dict, const char *key, bool value) { store(dict, key, number(value)); }
static void python_dict_store_uint(mp_obj_t dict, const char *key, uint32_t value) { store(dict, key, number(value)); }
static void python_dict_store_int(mp_obj_t dict, const char *key, int value) { store(dict, key, number(value)); }
static void python_dict_store_u64(mp_obj_t dict, const char *key, uint64_t value) { store(dict, key, number((int64_t)value)); }
static mp_obj_t field(mp_obj_t dict, const char *key)
{ for (size_t i = 0; i < dict->count; i++) if (!strcmp(dict->keys[i], key)) return dict->values[i]; return NULL; }
static bool python_should_cancel(void *user) { return script_should_cancel(user); }
#define MP_ERROR_TEXT(text) text
#define mp_raise_ValueError(text) ((void)(text), python_error())
#define MP_DEFINE_CONST_FUN_OBJ_0(...)
#define MP_DEFINE_CONST_FUN_OBJ_1(...)
#define MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(...)
#define SOLAR_OS_PACKAGE_SERVICE_AGENT 1
#include "solar_os_python_agent.inc"

static void fail_conversion(void) { fail_python_alloc = true; }
int main(void)
{
    assert(!solar_os_json_init());
    mp_obj_t args[] = {string("Describe the image."), string("stream:camera0"), string("")};
    mp_obj_t result = solaros_agent_ask(2, args);
    assert(!strcmp(field(result, "text")->text, "image seen") && field(result, "total_tokens")->number == 33);
    assert(!allocations);
    mp_obj_t images = mp_obj_new_list(0, NULL);
    mp_obj_list_append(images, string("photo.jpg")); mp_obj_list_append(images, string("pipeline:1"));
    args[1] = images; result = solaros_agent_ask(3, args);
    assert(!strcmp(field(result, "conversation_id")->text, "c1"));
    args[1] = mp_const_none; args[2] = string("c1");
    assert(!strcmp(field(solaros_agent_ask(3, args), "text")->text, "image seen"));
    solaros_agent_set_endpoint(string("https://test/responses")); solaros_agent_set_model(string("vision"));
    solaros_agent_set_api_key(string("secret")); solaros_agent_set_reasoning_effort(string("low"));
    solaros_agent_set_tool_policy(string("off")); solaros_agent_set_max_tools(number(32));
    result = solaros_agent_status();
    assert(!strcmp(field(result, "model")->text, "vision") && field(result, "max_tools")->number == 32);
    assert(field(result, "api_key_set")->number && !field(result, "api_key"));
    solaros_agent_set_api_key(string("")); assert(!field(solaros_agent_status(), "api_key_set")->number);
    assert(solaros_agent_conversations()->count == 1);
    assert(field(solaros_agent_conversation_info(string("c1")), "created_at")->number == 5000000000LL);
    solaros_agent_delete_conversation(string("c1"));
    for (int i = 0; i < 4; i++) {
        int limits[] = {-1, 0, 33, 256};
        if (!setjmp(error_jump)) { solaros_agent_set_max_tools(number(limits[i])); assert(false); }
    }
    args[1] = images; mp_obj_list_append(images, string("third.jpg"));
    if (!setjmp(error_jump)) { solaros_agent_ask(2, args); assert(false); }
    args[1] = number(1);
    if (!setjmp(error_jump)) { solaros_agent_ask(2, args); assert(false); }
    args[1] = string("a"); args[1]->length = 3;
    if (!setjmp(error_jump)) { solaros_agent_ask(2, args); assert(false); }
    args[1] = string("missing.jpg");
    if (!setjmp(error_jump)) { solaros_agent_ask(2, args); assert(false); }
    args[1] = mp_const_none; agent_error = ESP_ERR_INVALID_STATE;
    if (!setjmp(error_jump)) { solaros_agent_ask(1, args); assert(false); }
    agent_error = ESP_OK; cancel_during_run = true;
    if (!setjmp(error_jump)) { solaros_agent_ask(1, args); assert(false); }
    script_cancel = cancel_during_run = false;
    after_run = fail_conversion;
    if (!setjmp(error_jump)) { solaros_agent_ask(1, args); assert(false); }
    after_run = NULL; assert(!allocations);
    solaros_agent_forget(); solaros_agent_cancel();
    puts("Python agent arguments, image queries, status, history, errors and conversion OOM passed");
}
