/* Execute the real binding, service, ABI and MicroPython VM over a fake radio. */
#define main ble_service_suite
#include "ble_service_test.c"
#undef main
#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "shared/runtime/gchelper.h"
#include "script_ble_abi_fixture.h"

static void *python_runner_control;
static atomic_bool stopped;
static bool python_should_cancel(void *user) { (void)user; return atomic_load(&stopped); }
static mp_obj_t python_key(const char *key) { return mp_obj_new_str(key, strlen(key)); }
static void python_dict_store_cstr(mp_obj_t d, const char *k, const char *v)
{ mp_obj_dict_store(d, python_key(k), mp_obj_new_str(v, strlen(v))); }
static void python_dict_store_int(mp_obj_t d, const char *k, mp_int_t v)
{ mp_obj_dict_store(d, python_key(k), mp_obj_new_int(v)); }
static void python_dict_store_uint(mp_obj_t d, const char *k, uint32_t v)
{ mp_obj_dict_store(d, python_key(k), mp_obj_new_int_from_uint(v)); }
static void python_dict_store_bool(mp_obj_t d, const char *k, bool v)
{ mp_obj_dict_store(d, python_key(k), mp_obj_new_bool(v)); }
static uint32_t python_u32_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < 0 || (uint64_t)value > UINT32_MAX) mp_raise_ValueError(MP_ERROR_TEXT("bad u32"));
    return value;
}
static int32_t python_i32_from_obj(mp_obj_t obj)
{
    const mp_int_t value = mp_obj_get_int(obj);
    if (value < INT32_MIN || value > INT32_MAX) mp_raise_ValueError(MP_ERROR_TEXT("bad i32"));
    return value;
}
static uint8_t python_u8_from_obj(mp_obj_t obj)
{
    const uint32_t value = python_u32_from_obj(obj);
    if (value > UINT8_MAX) mp_raise_ValueError(MP_ERROR_TEXT("bad u8"));
    return value;
}
static uint32_t python_optional_u32(size_t count, const mp_obj_t *args, size_t index, uint32_t fallback)
{ return index >= count || args[index] == mp_const_none ? fallback : python_u32_from_obj(args[index]); }
#include "../../src/apps/solar_os_python_ble.inc"
#include "../../src/apps/solar_os_python_ble_stream.inc"
#include "../../src/apps/solar_os_python_tcp_stream.inc"

void solar_os_micropython_vm_hook(void) {}
void solar_os_micropython_check_interrupt(void) {}
uint32_t solar_os_micropython_random_seed(void) { return 42; }
void mp_hal_stdout_tx_strn(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
void mp_hal_stdout_tx_strn_cooked(const char *s, size_t n) { fwrite(s, 1, n, stdout); }
int solar_os_micropython_resolve_path(const char *in, char *out, size_t size)
{
    if (strlen(in) >= size) return -1;
    strcpy(out, in); return 0;
}
mp_import_stat_t mp_import_stat(const char *path) { (void)path; return MP_IMPORT_STAT_NO_EXIST; }
static mp_obj_t no_open(size_t n, const mp_obj_t *args, mp_map_t *kwargs)
{ (void)n; (void)args; (void)kwargs; mp_raise_OSError(2); }
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, no_open);
void gc_collect(void) { gc_collect_start(); gc_helper_collect_regs_and_stack(); gc_collect_end(); }
void nlr_jump_fail(void *value) { (void)value; assert(0); abort(); }
static void run(const char *source)
{
    nlr_buf_t nlr;
    if (!nlr_push(&nlr)) {
        mp_lexer_t *lexer = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, source, strlen(source), 0);
        mp_parse_tree_t tree = mp_parse(lexer, MP_PARSE_FILE_INPUT);
        mp_obj_t fn = mp_compile(&tree, MP_QSTR__lt_stdin_gt_, false);
        mp_call_function_0(fn); nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        assert(0);
    }
}
static mp_obj_dict_t *module(const char *name)
{
    mp_obj_t value = mp_obj_new_module(qstr_from_str(name));
    mp_store_global(qstr_from_str(name), value);
    return ((mp_obj_module_t *)MP_OBJ_TO_PTR(value))->globals;
}
static void *stop_waiting_vm(void *user)
{
    (void)await_submission(*(const unsigned *)user);
    atomic_store(&stopped, true);
    return NULL;
}
int main(void)
{
    assert(!ble_service_suite());
    char top;
    void *heap = malloc(2 * 1024 * 1024); assert(heap);
    mp_cstack_init_with_top(&top, 1024 * 1024);
    gc_init(heap, (char *)heap + 2 * 1024 * 1024); mp_init();
    mp_obj_dict_t *globals = module("ble");
#define EXPOSE(name, object) mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), python_key(name), MP_OBJ_FROM_PTR(&(object)))
    EXPOSE("scan", solaros_ble_scan_obj);
    EXPOSE("status", solaros_ble_status_obj); EXPOSE("connected", solaros_ble_connected_obj);
    EXPOSE("pair", solaros_ble_pair_obj); EXPOSE("forget", solaros_ble_forget_obj);
    EXPOSE("layout", solaros_ble_layout_obj); EXPOSE("read", solaros_ble_read_obj);
    globals = module("ble_stream");
    EXPOSE("listen",solaros_streams_ble_listen_obj);EXPOSE("connect",solaros_streams_ble_connect_obj);
    EXPOSE("remove",solaros_streams_ble_remove_obj);
    globals = module("tcp_stream");
    EXPOSE("listen",solaros_streams_tcp_listen_obj);EXPOSE("connect",solaros_streams_tcp_connect_obj);
    EXPOSE("remove",solaros_streams_tcp_remove_obj);
    globals = module("gatt");
#define GATT(name) EXPOSE(#name, solaros_ble_gatt_##name##_obj)
    GATT(connect); GATT(disconnect); GATT(capacity); GATT(status); GATT(services); GATT(characteristics);
    GATT(read); GATT(write); GATT(subscribe); GATT(unsubscribe); GATT(configure_queue); GATT(poll);
    globals = module("server");
#define SERVER(name) EXPOSE(#name, solaros_ble_server_##name##_obj)
    SERVER(create); SERVER(service); SERVER(characteristic); SERVER(start); SERVER(stop); SERVER(close);
    SERVER(status); SERVER(poll); SERVER(peers); SERVER(set); SERVER(send); SERVER(disconnect);
    globals = module("hid");
#define HID(name) EXPOSE(#name, solaros_ble_hid_##name##_obj)
    HID(start); HID(hosts); HID(connect); HID(forget); HID(disconnect); HID(pair); HID(stop); HID(status); HID(poll);
    HID(keyboard_press); HID(keyboard_release); HID(keyboard_release_all); HID(mouse_move); HID(mouse_button);
    HID(gamepad_axis); HID(gamepad_button); HID(gamepad_hat); HID(gamepad_send);
    run("def fails(fn,*args):\n try:\n  fn(*args)\n except (OSError,ValueError,TypeError):\n  return\n assert False\n"
        "tcp_stream.connect('tcp-out','127.0.0.1',9)\nfails(tcp_stream.connect,'tcp-out','127.0.0.1',9)\n"
        "tcp_stream.remove('tcp-out')\nfails(tcp_stream.listen,'bad',0)\nfails(tcp_stream.listen,'bad',65536)\n"
        "fails(tcp_stream.connect,'bad','127.0.0.1',4294967296)\n"
        "fails(tcp_stream.connect,'bad\\x00name','127.0.0.1',9)\n"
        "fails(tcp_stream.connect,'bad','127.0.0.1\\x00x',9)\n"
        "tcp_stream.connect('vm-tcp','127.0.0.1',9)\n"
        "assert ble.status()=='connected' and ble.connected()\nble.pair()\nble.forget()\n"
        "assert ble.layout()=='us'\nble.layout('de')\nassert ble.layout()=='de'\n"
        "assert ble.read(2)==b'\\x00\\xff'\nfails(ble.read,257)\nfails(ble.layout,'invalid')\n"
        "ble_stream.listen('python-link')\nfails(ble_stream.listen,'python-link')\nble_stream.remove('python-link')\n"
        "ble_stream.connect('python-out','01:02:03:04:05:06',1)\nble_stream.remove('python-out')\n"
        "fails(ble_stream.listen,'bad\\x00name')\nble_stream.listen('vm-persist')\n"
        "devices=ble.scan()\nassert len(devices)==1\nd=devices[0]\n"
        "assert d['address']=='01:02:03:04:05:06' and d['name']=='Sensor' and not d['keyboard_like']\n"
        "fails(gatt.connect,'bad')\nfails(gatt.connect,'01:02:03:04:05:06',4)\n"
        "peer=gatt.connect('01:02:03:04:05:06',1)\nassert gatt.capacity()==1\n"
        "assert gatt.status(peer)['connected'] and gatt.status(peer)['max_value_bytes']==176\n"
        "assert gatt.services(peer)[0]['uuid']=='0x180f'\n"
        "assert gatt.characteristics(peer,0)[0]['uuid']=='0x2a19'\n"
        "assert gatt.read(peer,3)==b'B'*176\ngatt.write(peer,3,b'\\x00\\xff')\n"
        "fails(gatt.read,peer,0)\nfails(gatt.read,peer,3,60001)\nfails(gatt.write,peer,3,b'')\n"
        "fails(gatt.write,peer,3,b'x'*177)\ngatt.configure_queue(peer,2)\ngatt.subscribe(peer,3,True)\n");
    assert(write_response);
    keyboard_backend_result=ESP_ERR_NOT_SUPPORTED;
    run("try:\n ble.pair()\n assert False\nexcept OSError as e:\n assert 'BLE error -6' in str(e)\n");
    keyboard_backend_result=ESP_OK;
    uint8_t bytes[] = {0,255};
    solar_os_ble_backend_event_t event = {.type=SOLAR_OS_BLE_BACKEND_NOTIFICATION,.epoch=fake_epoch,
        .conn_id=7,.handle=3,.value=bytes,.value_len=2,.indication=true};
    solar_os_ble_service_event(&event); bytes[1]=0;
    run("e=gatt.poll(peer)\nassert e['data']==b'\\x00\\xff' and e['indication']\n"
        "assert gatt.poll(peer) is None\ngatt.unsubscribe(peer,3)\ngatt.write(peer,3,b'\\x00\\xff',False)\n");
    assert(!write_response);
    defer_read=true;
    const unsigned after=submission_count(); pthread_t stopper;
    assert(!pthread_create(&stopper,NULL,stop_waiting_vm,(void *)&after));
    run("try:\n gatt.read(peer,3)\n assert False\nexcept OSError as e:\n assert 'cancelled' in str(e)\n");
    assert(!pthread_join(stopper,NULL)); defer_read=false;
    run("fails(gatt.read,peer,3)\n");
    const solar_os_ble_session_t old=python_ble_session;
    python_ble_destroy(); python_ble_destroy(); retired(); atomic_store(&stopped,false);
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(old,&info)==ESP_ERR_INVALID_STATE);
    run("server.create('Service')\ns=server.service('180f')\nc=server.characteristic(s,'2a19',18)\n"
        "server.start()\nassert server.status()['event_capacity']==16\nassert server.poll() is None\n"
        "assert server.peers()==[]\nserver.set(c,b'\\x00\\xff')\nserver.stop()\nserver.close()\n"
        "hid.start('Keyboard',True)\nassert hid.status()['host_name']=='Desktop'\n"
        "assert hid.hosts()[0]['address']=='01:02:03:04:05:06'\n"
        "hid.connect('01:02:03:04:05:06',1)\nhid.forget('01:02:03:04:05:06')\nhid.disconnect()\n"
        "hid.pair()\nassert hid.poll() is None\nhid.keyboard_press(260,4)\nhid.keyboard_release(4)\n"
        "hid.keyboard_release_all()\nhid.mouse_move(-400,500)\nhid.mouse_button(1,True)\n"
        "hid.gamepad_axis(0,-32768)\nhid.gamepad_button(16,False)\nhid.gamepad_hat(8)\nhid.gamepad_send()\n"
        "fails(hid.mouse_button,256,True)\nfails(hid.keyboard_press,-1)\nhid.stop()\n");
    assert(fake_hid_request.op==SOLAR_OS_BLE_HID_OP_STOP && !fake_hid_owner);
    assert(python_ble_session && python_ble_session!=old && !script_ble_adapter_allocations);
    python_ble_destroy();
    const unsigned lookups=script_ble_lookups;
    script_ble_available=false; run("fails(ble.scan)\nfails(ble.status)\n");
    assert(script_ble_lookups==lookups+2);
    script_ble_available=true; run("assert len(ble.scan())==1 and ble.status()=='connected'\n");
    script_ble_available=false; run("assert len(ble.scan())==1\nassert gatt.capacity()==1 and ble.connected()\n");
    assert(script_ble_lookups==lookups+4);
    python_tcp_stream_cleanup();
    const unsigned tcp_lookups=script_ble_lookups;
    run("fails(tcp_stream.connect,'unavailable','127.0.0.1',9)\n");
    assert(script_ble_lookups == tcp_lookups + 1);
    script_ble_available=true;
    run("tcp_stream.connect('cached','127.0.0.1',9)\n");
    script_ble_available=false;
    run("tcp_stream.remove('cached')\n");
    assert(script_ble_lookups == tcp_lookups + 2);
    python_ble_destroy();python_ble_stream_cleanup();python_tcp_stream_cleanup(); mp_deinit(); free(heap);
    const solar_os_native_tcp_stream_api_v1_t *tcp=solar_os_native_tcp_stream_get_service("stream.tcp",1,sizeof(*tcp));
    assert(tcp && !tcp->remove("vm-tcp"));
    const solar_os_native_ble_stream_api_v1_t *streams=solar_os_native_ble_stream_get_service("stream.ble",1,sizeof(*streams));
    solar_os_ble_stream_info_t stream_info;
    assert(streams && !streams->status("vm-persist",&stream_info) && stream_info.listening && !stream_info.claimed);
    assert(!streams->remove("vm-persist"));
    puts("MicroPython BLE: real VM, ABI binary I/O, cancellation, peripherals and cleanup passed");
    return 0;
}
