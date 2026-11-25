#include "script_hid_abi_fixture.h"
#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "shared/runtime/gchelper.h"
static mp_obj_t python_key(const char *key) { return mp_obj_new_str(key,strlen(key)); }
static void python_dict_store_bool(mp_obj_t dict,const char *key,bool value)
{ mp_obj_dict_store(dict,python_key(key),mp_obj_new_bool(value)); }
static int32_t python_i32_from_obj(mp_obj_t obj)
{
    const mp_int_t value=mp_obj_get_int(obj);
    if(value<INT32_MIN || value>INT32_MAX)mp_raise_ValueError(MP_ERROR_TEXT("invalid i32"));
    return value;
}
static uint8_t python_u8_from_obj(mp_obj_t obj)
{
    const mp_int_t value=mp_obj_get_int(obj);
    if(value<0 || value>UINT8_MAX)mp_raise_ValueError(MP_ERROR_TEXT("invalid u8"));
    return value;
}
#include "../../src/apps/solar_os_python_hid.inc"
void solar_os_micropython_vm_hook(void) {}
void solar_os_micropython_check_interrupt(void) {}
uint32_t solar_os_micropython_random_seed(void) { return 42; }
void mp_hal_stdout_tx_strn(const char *s,size_t n) { fwrite(s,1,n,stdout); }
void mp_hal_stdout_tx_strn_cooked(const char *s,size_t n) { fwrite(s,1,n,stdout); }
int solar_os_micropython_resolve_path(const char *in,char *out,size_t size)
{ if(strlen(in)>=size)return -1;strcpy(out,in);return 0; }
mp_import_stat_t mp_import_stat(const char *path) { (void)path;return MP_IMPORT_STAT_NO_EXIST; }
static mp_obj_t no_open(size_t count,const mp_obj_t *args,mp_map_t *kwargs)
{ (void)count;(void)args;(void)kwargs;mp_raise_OSError(2); }
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj,1,no_open);
void gc_collect(void) { gc_collect_start();gc_helper_collect_regs_and_stack();gc_collect_end(); }
void nlr_jump_fail(void *value) { (void)value;assert(0);abort(); }
static void run(const char *source)
{
    nlr_buf_t nlr;
    if(!nlr_push(&nlr)) {
        mp_lexer_t *lexer=mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,source,strlen(source),0);
        mp_parse_tree_t tree=mp_parse(lexer,MP_PARSE_FILE_INPUT);
        mp_obj_t fn=mp_compile(&tree,MP_QSTR__lt_stdin_gt_,false);mp_call_function_0(fn);nlr_pop();
    } else {mp_obj_print_exception(&mp_plat_print,(mp_obj_t)nlr.ret_val);assert(0);}
}
static mp_obj_dict_t *globals(mp_obj_t module)
{ return ((mp_obj_module_t *)MP_OBJ_TO_PTR(module))->globals; }
int main(void)
{
    script_hid_begin();char top;void *heap=malloc(2*1024*1024);assert(heap);
    mp_cstack_init_with_top(&top,1024*1024);gc_init(heap,(char *)heap+2*1024*1024);mp_init();
    mp_obj_t root=mp_obj_new_module(qstr_from_str("hid"));mp_store_global(qstr_from_str("hid"),root);
    mp_obj_dict_t *dict=globals(root);
#define EXPOSE(name,object) mp_obj_dict_store(MP_OBJ_FROM_PTR(dict),python_key(name),MP_OBJ_FROM_PTR(&(object)))
    EXPOSE("status",solaros_hid_status_obj);
    mp_obj_t keyboard=mp_obj_new_module(qstr_from_str("hid_keyboard"));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict),python_key("keyboard"),keyboard);dict=globals(keyboard);
    EXPOSE("press",solaros_hid_keyboard_press_obj);EXPOSE("release",solaros_hid_keyboard_release_obj);
    EXPOSE("release_all",solaros_hid_keyboard_release_all_obj);
    mp_obj_t mouse=mp_obj_new_module(qstr_from_str("hid_mouse"));dict=globals(root);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict),python_key("mouse"),mouse);dict=globals(mouse);
    EXPOSE("move",solaros_hid_mouse_move_obj);EXPOSE("button",solaros_hid_mouse_button_obj);
    mp_obj_t gamepad=mp_obj_new_module(qstr_from_str("hid_gamepad"));dict=globals(root);
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict),python_key("gamepad"),gamepad);dict=globals(gamepad);
    EXPOSE("axis",solaros_hid_gamepad_axis_obj);EXPOSE("button",solaros_hid_gamepad_button_obj);
    EXPOSE("hat",solaros_hid_gamepad_hat_obj);EXPOSE("send",solaros_hid_gamepad_send_obj);
    run("def fails(fn,*args):\n try:\n  fn(*args)\n except (OSError,ValueError,TypeError):\n  return\n assert False\n"
        "s=hid.status()\nassert s['initialized'] and s['connected']\n"
        "hid.keyboard.press(257,4)\nfails(hid.keyboard.press,65536)\n"
        "fails(hid.mouse.move,4294967296,0)\nfails(hid.gamepad.axis,4294967296,0)\n"
        "fails(hid.gamepad.hat,4294967296)\nfails(hid.gamepad.hat,-1)\nfails(hid.gamepad.button,256,True)\n");
    assert(hid_api_lookups==1 && hid_send_keyboard() && sent_keyboard.modifier==1 && sent_keyboard.keycode[0]==4);
    run("hid.mouse.move(17,-18)\nhid.mouse.button(1,True)\nhid.gamepad.axis(5,32767)\n"
        "hid.gamepad.button(32,True)\nhid.gamepad.hat(8)\nhid.gamepad.send()\n");
    assert(hid_send_mouse() && sent_mouse.x==17 && sent_mouse.y==-18 && sent_mouse.buttons==1);
    assert(hid_send_gamepad() && sent_gamepad.ry==127 && sent_gamepad.buttons==(UINT32_C(1)<<31) && sent_gamepad.hat==8);
    run("hid.keyboard.release(257,4)\nhid.keyboard.press(5)\n");
    python_hid_cleanup();script_hid_check_queued_cleanup();hid_api_available=false;
    run("try:\n hid.status()\n assert False\nexcept OSError as e:\n assert 'unavailable' in str(e)\n");
    python_hid_cleanup();hid_api_available=true;run("assert hid.status()['connected']\n");
    const unsigned cached=hid_api_lookups;hid_api_available=false;
    run("hid.keyboard.release_all()\nassert hid.status()['initialized']\n");
    assert(hid_api_lookups==cached);mp_deinit();free(heap);python_hid_cleanup();hid_test_shutdown();
    puts("MicroPython HID: real VM/service, ABI controls, wide integer bounds, cached lookup and post-VM cleanup passed");
    return 0;
}
