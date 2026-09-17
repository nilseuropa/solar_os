/* Reuse the threaded fake radio and run the service suite before exercising
 * the actual Lua bindings against the real session service. */
#define main ble_service_suite
#include "ble_service_test.c"
#undef main

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static void *solua_runner_control;
static atomic_bool stopped;
static bool solua_should_cancel(void *user) { (void)user; return atomic_load(&stopped); }
static int solua_check_esp(lua_State *L, esp_err_t err)
{
    return err == ESP_OK ? 0 : luaL_error(L, "BLE error %d", err);
}
static void solua_set_str(lua_State *L, int table, const char *key, const char *value)
{
    table = lua_absindex(L, table);
    lua_pushstring(L, value);
    lua_setfield(L, table, key);
}
static void solua_set_int(lua_State *L, int table, const char *key, lua_Integer value)
{
    table = lua_absindex(L, table);
    lua_pushinteger(L, value);
    lua_setfield(L, table, key);
}
static void solua_set_bool(lua_State *L, int table, const char *key, bool value)
{
    table = lua_absindex(L, table);
    lua_pushboolean(L, value);
    lua_setfield(L, table, key);
}

#include "solar_os_lua_ble.inc"

static lua_State *new_vm(void)
{
    lua_State *L = luaL_newstate();
    assert(L != NULL);
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    const luaL_Reg methods[] = {
        {"connect", solua_ble_gatt_connect}, {"disconnect", solua_ble_gatt_disconnect},
        {"status", solua_ble_gatt_status}, {"services", solua_ble_gatt_services},
        {"characteristics", solua_ble_gatt_characteristics}, {"read", solua_ble_gatt_read},
        {"capacity", solua_ble_gatt_capacity}, {"write", solua_ble_gatt_write},
        {"subscribe", solua_ble_gatt_subscribe}, {"unsubscribe", solua_ble_gatt_unsubscribe},
        {"configure_queue", solua_ble_gatt_configure_queue}, {"poll", solua_ble_gatt_poll}, {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setglobal(L, "gatt");
    lua_newtable(L);
    lua_pushcfunction(L, solua_ble_scan);
    lua_setfield(L, -2, "scan");
    lua_setglobal(L, "ble");
    const luaL_Reg server_methods[] = {
        {"create", solua_ble_server_create}, {"service", solua_ble_server_service},
        {"characteristic", solua_ble_server_characteristic}, {"start", solua_ble_server_start},
        {"stop", solua_ble_server_stop}, {"close", solua_ble_server_close},
        {"status", solua_ble_server_status}, {"poll", solua_ble_server_poll},
        {"peers", solua_ble_server_peers}, {"set", solua_ble_server_set},
        {"send", solua_ble_server_send}, {"disconnect", solua_ble_server_disconnect}, {NULL, NULL},
    };
    lua_newtable(L); luaL_setfuncs(L, server_methods, 0); lua_setglobal(L, "server");
    const luaL_Reg hid_methods[] = {
        {"start", solua_ble_hid_start}, {"pair", solua_ble_hid_pair},
        {"stop", solua_ble_hid_stop},
        {"status", solua_ble_hid_status}, {"poll", solua_ble_hid_poll}, {NULL, NULL},
    };
    const luaL_Reg hid_keyboard_methods[] = {
        {"press", solua_ble_hid_keyboard_press},
        {"release", solua_ble_hid_keyboard_release},
        {"release_all", solua_ble_hid_keyboard_release_all}, {NULL, NULL},
    };
    const luaL_Reg hid_mouse_methods[] = {
        {"move", solua_ble_hid_mouse_move},
        {"button", solua_ble_hid_mouse_button}, {NULL, NULL},
    };
    const luaL_Reg hid_gamepad_methods[] = {
        {"axis", solua_ble_hid_gamepad_axis},
        {"button", solua_ble_hid_gamepad_button},
        {"hat", solua_ble_hid_gamepad_hat},
        {"send", solua_ble_hid_gamepad_send}, {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, hid_methods, 0);
    lua_newtable(L); luaL_setfuncs(L, hid_keyboard_methods, 0); lua_setfield(L, -2, "keyboard");
    lua_newtable(L); luaL_setfuncs(L, hid_mouse_methods, 0); lua_setfield(L, -2, "mouse");
    lua_newtable(L); luaL_setfuncs(L, hid_gamepad_methods, 0); lua_setfield(L, -2, "gamepad");
    lua_setglobal(L, "hid");
    return L;
}

static void run(lua_State *L, const char *source)
{
    if (luaL_dostring(L, source) != LUA_OK) {
        fprintf(stderr, "Lua test: %s\n", lua_tostring(L, -1));
        assert(false);
    }
}

static void *stop_waiting_vm(void *user)
{
    const unsigned *after = user;
    (void)await_submission(*after);
    atomic_store(&stopped, true);
    return NULL;
}

int main(void)
{
    assert(ble_service_suite() == 0);
    lua_State *L = new_vm();
    run(L, "devices = ble.scan(); assert(#devices == 1); "
        "d = devices[1]; assert(d.address == '01:02:03:04:05:06'); "
        "assert(d.name == 'Sensor' and d.addr_type == 1 and d.rssi == -73); "
        "assert(d.appearance == 961 and d.hid_service and d.remembered); "
        "assert(d.keyboard_like == false and d.connected == false)");
    scan_count = 0;
    run(L, "assert(#ble.scan() == 0)");
    scan_error = ESP_ERR_NOT_FOUND;
    run(L, "assert(#ble.scan() == 0)");
    scan_count = 1;
    scan_error = ESP_FAIL;
    run(L, "assert(not pcall(ble.scan))");
    scan_error = ESP_OK;
    atomic_store(&stopped, true);
    run(L, "assert(not pcall(ble.scan))");
    atomic_store(&stopped, false);
    run(L,
        "assert(not pcall(gatt.disconnect, 0)); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06x', 1)); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06' .. string.char(0), 1)); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06', 4294967297)); "
        "assert(not pcall(gatt.connect, '01:02:03:04:05:06', 1, 4294967296)); "
        "peer = gatt.connect('01:02:03:04:05:06', 1); "
        "assert(not pcall(ble.scan)); "
        "assert(type(peer) == 'number' and gatt.capacity() == 1); "
        "assert(not pcall(gatt.configure_queue, peer, 0)); "
        "assert(not pcall(gatt.configure_queue, peer, 4294967296)); "
        "assert(not pcall(gatt.subscribe, peer, 3, 1)); "
        "assert(not pcall(gatt.unsubscribe, peer, 3, -1)); "
        "gatt.configure_queue(peer, 2); gatt.subscribe(peer, 3); "
        "assert(gatt.status(peer).event_capacity == 2 and gatt.poll(peer) == nil); "
        "gatt.unsubscribe(peer, 3); "
        "assert(not pcall(gatt.status, 0)); "
        "assert(not pcall(gatt.status, 4294967296)); "
        "local s = gatt.status(peer); "
        "assert(s.connected and not s.retiring and s.mtu == 247); "
        "assert(s.owner == 'lua.app' and s.address == '01:02:03:04:05:06'); "
        "assert(s.addr_type == 1 and s.max_value_bytes == 128); "
        "local services = gatt.services(peer); "
        "assert(#services == 24 and services[1].index == 0); "
        "assert(services[1].uuid == '0x180f' and services[1].primary); "
        "local chars = gatt.characteristics(peer, services[1].index); "
        "assert(#chars == 1 and chars[1].handle == 3 and chars[1].properties == 2); "
        "assert(chars[1].uuid == '0x2a19'); "
        "assert(gatt.read(peer, 3) == string.rep('B', 128)); "
        "gatt.write(peer, 3, string.char(0, 255)); "
        "assert(not pcall(gatt.read, peer, 0)); "
        "assert(not pcall(gatt.read, peer, 65536)); "
        "assert(not pcall(gatt.read, peer, -1)); "
        "assert(not pcall(gatt.read, peer, 3, -1)); "
        "assert(not pcall(gatt.read, peer, 3, 60001)); "
        "assert(not pcall(gatt.characteristics, peer, -1)); "
        "assert(not pcall(gatt.characteristics, peer, 4294967296)); "
        "assert(not pcall(gatt.write, peer, 3, 123)); "
        "assert(not pcall(gatt.write, peer, 3, '')); "
        "assert(not pcall(gatt.write, peer, 3, string.rep('x', 129))); "
        "assert(not pcall(gatt.write, peer, 3, 'xx', 0));");
    assert(write_response);
    run(L, "gatt.subscribe(peer, 3, true)");
    uint8_t notification_data[] = {0, 255};
    solar_os_ble_backend_event_t notification = {.type=SOLAR_OS_BLE_BACKEND_NOTIFICATION,
        .epoch=fake_epoch,.conn_id=7,.handle=3,.value=notification_data,.value_len=2,.indication=true};
    solar_os_ble_service_event(&notification);
    notification_data[1]=0;
    run(L, "local e=gatt.poll(peer); assert(e.handle==3 and e.indication and e.data==string.char(0,255)); "
           "assert(gatt.poll(peer)==nil)");
    solar_os_ble_service_event(&notification);
    run(L, "gatt.unsubscribe(peer,3); assert(gatt.poll(peer)==nil)");
    run(L, "gatt.write(peer, 3, string.char(0, 255), false)");
    assert(!write_response);
    uint8_t value[2];
    size_t len;
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &len, 10) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK);
    run(L, "assert(gatt.status(peer).connected)");
    /* Stop interrupts a blocking native call, then blocks new work. */
    defer_read = true;
    const unsigned after = submission_count();
    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_waiting_vm, (void *)&after) == 0);
    run(L, "local ok, err = pcall(gatt.read, peer, 3); assert(not ok and string.find(err, 'cancelled'))");
    assert(pthread_join(stopper, NULL) == 0);
    defer_read = false;
    run(L, "local ok, err = pcall(gatt.read, peer, 3); assert(not ok and string.find(err, 'cancelled'))");
    const solar_os_ble_session_t old = solua_ble_session;
    solua_ble_destroy();
    solua_ble_destroy();
    lua_close(L);
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(old, &info) == ESP_ERR_INVALID_STATE);
    retired();
    stopped = false;

    /* A new VM owns a new session; an uncaught exception can be cleaned up. */
    L = new_vm();
    run(L, "peer = gatt.connect('01:02:03:04:05:06', 1)");
    assert(solua_ble_session != old);
    assert(luaL_dostring(L, "error('intentional')") != LUA_OK);
    solua_ble_destroy();
    lua_close(L);
    retired();
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_OK);

    /* A different owner cannot read or cancel the shell peer. */
    L = new_vm();
    run(L, "assert(not pcall(gatt.read, peer, 3)); assert(not pcall(gatt.disconnect, 0))");
    solua_ble_destroy();
    lua_close(L);
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &len, 100) == ESP_OK);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK);
    retired();
    L = new_vm();
    run(L, "peer=gatt.connect('01:02:03:04:05:06',1)");
    defer_subscription=true;
    const unsigned before_subscribe=submission_count();
    assert(pthread_create(&stopper,NULL,stop_waiting_vm,(void *)&before_subscribe)==0);
    run(L, "local ok,err=pcall(gatt.subscribe,peer,3); assert(not ok and string.find(err,'cancelled'))");
    assert(pthread_join(stopper,NULL)==0);
    defer_subscription=false;
    solua_ble_destroy(); lua_close(L); retired(); stopped=false;
    L = new_vm();
    run(L, "server.create('Lua peripheral',16); svc=server.service('1234'); chr=server.characteristic(svc,'abcd',62,string.char(0,255,128))");
    assert(fake_server_request.value_len==3 && fake_server_request.value[1]==255);
    run(L, "server.start(); assert(server.status().event_capacity==16); assert(server.poll()==nil); assert(#server.peers()==0)");
    run(L, "server.set(chr,string.char(0,255)); server.send(7,chr,string.char(128,0),true)");
    assert(fake_server_request.indicate && fake_server_request.value_len==2 && fake_server_request.value[0]==128);
    run(L, "assert(not pcall(server.create,'name',0)); assert(not pcall(server.service,'a'..string.char(0)..'b'))");
    run(L, "assert(not pcall(server.characteristic,svc,'abcd',256)); assert(not pcall(server.set,0,'x'))");
    run(L, "assert(not pcall(server.set,chr,string.rep('x',129))); assert(not pcall(server.send,7,chr,'x',1))");
    run(L, "server.disconnect(7); server.stop(); server.close(); server.create('again')");
    assert(fake_server_owner==solua_ble_session);
    solar_os_ble_session_t outsider;
    assert(solar_os_ble_session_create("outsider",&outsider)==ESP_OK);
    solar_os_ble_server_request_t request={.op=SOLAR_OS_BLE_SERVER_STATUS};
    assert(solar_os_ble_server_request(outsider,&request)==ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_session_close(outsider)==ESP_OK && fake_server_owner==solua_ble_session);
    request.value_len=129;
    assert(solar_os_ble_server_request(solua_ble_session,&request)==ESP_ERR_INVALID_ARG);
    request.value_len=0; memset(request.text,'x',sizeof(request.text));
    assert(solar_os_ble_server_request(solua_ble_session,&request)==ESP_ERR_INVALID_ARG);
    run(L, "assert(not pcall(hid.start,'')); assert(not pcall(hid.start,string.rep('x',27))); "
           "assert(not pcall(hid.start,'x'..string.char(0))); hid.start('Lua HID'); hid.pair(); "
           "assert(hid.status().event_capacity==16 and hid.poll()==nil)");
    assert(fake_hid_request.op == SOLAR_OS_BLE_HID_OP_POLL);
    assert(fake_hid_owner == solua_ble_session);
    fake_hid_event=(solar_os_ble_hid_device_event_t){
        .type=SOLAR_OS_BLE_HID_PASSKEY,.peer=7,.passkey=12345};
    fake_hid_event_ready=true;
    run(L, "local e=hid.poll(); assert(e.type=='passkey' and e.peer==7 and e.passkey==12345)");
    run(L, "assert(not pcall(hid.keyboard.press)); assert(not pcall(hid.keyboard.press,-1)); "
           "hid.keyboard.press(260,4); hid.keyboard.release(4); hid.keyboard.release_all()");
    assert(fake_hid_request.op == SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE_ALL);
    run(L, "hid.mouse.move(-400,500); hid.mouse.button(1,true)");
    assert(fake_hid_request.op == SOLAR_OS_BLE_HID_OP_MOUSE_BUTTON &&
           fake_hid_request.button == 1 && fake_hid_request.pressed);
    run(L, "assert(not pcall(hid.mouse.button,256,true)); "
           "hid.gamepad.axis(0,-32768); hid.gamepad.button(16,false); "
           "hid.gamepad.hat(8); hid.gamepad.send(); hid.stop()");
    assert(fake_hid_request.op == SOLAR_OS_BLE_HID_OP_STOP && !fake_hid_owner);
    solar_os_ble_session_t server_owner=solua_ble_session;
    solua_ble_destroy(); assert(!fake_server_owner); lua_close(L);
    request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_STATUS};
    assert(solar_os_ble_server_request(server_owner,&request)==ESP_ERR_INVALID_STATE);
    puts("Lua BLE bindings: real VM, binary I/O, validation, ownership and cleanup OK");
    return 0;
}
