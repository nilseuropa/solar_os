/* Included inside the BLE package gate, after common interpreter helpers. */
static solar_os_ble_session_t solua_ble_session;

static int solua_ble_check(lua_State *L, esp_err_t err)
{
    if (err == SOLAR_OS_BLE_ERR_CANCELLED) {
        return luaL_error(L, "BLE operation cancelled");
    }
    if (err == SOLAR_OS_BLE_ERR_CAPACITY) return luaL_error(L, "BLE connection capacity exhausted");
    return solua_check_esp(L, err);
}

static int solua_ble_scan(lua_State *L)
{
    solar_os_ble_scan_result_t results[SOLAR_OS_BLE_SCAN_MAX_RESULTS];
    size_t found = 0;
    if (solua_should_cancel(NULL)) {
        return solua_ble_check(L, SOLAR_OS_BLE_ERR_CANCELLED);
    }
    const esp_err_t err = solar_os_ble_scan(results, SOLAR_OS_BLE_SCAN_MAX_RESULTS, &found);
    /* Discovery reports NOT_FOUND when no advertisement matched. */
    if (err != ESP_ERR_NOT_FOUND || found != 0) {
        solua_ble_check(L, err);
    }
    if (solua_should_cancel(NULL)) {
        return solua_ble_check(L, SOLAR_OS_BLE_ERR_CANCELLED);
    }
    lua_newtable(L);
    const int list = lua_gettop(L);
    for (size_t i = 0; i < found && i < SOLAR_OS_BLE_SCAN_MAX_RESULTS; i++) {
        const solar_os_ble_scan_result_t *device = &results[i];
        char address[18];
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
            device->bda[0], device->bda[1], device->bda[2],
            device->bda[3], device->bda[4], device->bda[5]);
        lua_createtable(L, 0, 9);
        solua_set_str(L, -1, "address", address);
        solua_set_str(L, -1, "name", device->name);
        solua_set_int(L, -1, "addr_type", device->addr_type);
        solua_set_int(L, -1, "rssi", device->rssi);
        solua_set_int(L, -1, "appearance", device->appearance);
        solua_set_bool(L, -1, "hid_service", device->hid_service);
        solua_set_bool(L, -1, "keyboard_like", device->keyboard_like);
        solua_set_bool(L, -1, "remembered", device->remembered);
        solua_set_bool(L, -1, "connected", device->connected);
        lua_rawseti(L, list, (lua_Integer)i + 1);
    }
    return 1;
}

static solar_os_ble_session_t solua_ble_get(lua_State *L)
{
    if (solua_should_cancel(NULL)) {
        solua_ble_check(L, SOLAR_OS_BLE_ERR_CANCELLED);
    }
    if (solua_ble_session == SOLAR_OS_BLE_SESSION_INVALID) {
        solua_ble_check(L, solar_os_ble_session_create(
            solua_runner_control != NULL ? "lua.runner" : "lua.app", &solua_ble_session));
        solua_ble_check(L, solar_os_ble_session_set_cancel_check(
            solua_ble_session, solua_should_cancel, NULL));
    }
    return solua_ble_session;
}

static void solua_ble_destroy(void)
{
    if (solua_ble_session != SOLAR_OS_BLE_SESSION_INVALID) {
        (void)solar_os_ble_session_close(solua_ble_session);
        solua_ble_session = SOLAR_OS_BLE_SESSION_INVALID;
    }
}

static uint32_t solua_ble_timeout(lua_State *L, int index)
{
    const lua_Integer timeout = luaL_optinteger(L, index, 0);
    if (timeout < 0 || timeout > 60000) {
        luaL_error(L, "timeout must be 0..60000 ms");
    }
    return (uint32_t)timeout;
}

static uint16_t solua_ble_handle(lua_State *L, int index)
{
    const lua_Integer handle = luaL_checkinteger(L, index);
    if (handle < 1 || handle > UINT16_MAX) {
        luaL_error(L, "handle must be 1..65535");
    }
    return (uint16_t)handle;
}

static solar_os_ble_peer_t solua_ble_peer(lua_State *L)
{
    const lua_Integer peer = luaL_checkinteger(L, 1);
    if (peer <= 0 || (uint64_t)peer > UINT32_MAX) luaL_error(L, "invalid BLE peer handle");
    return (solar_os_ble_peer_t)peer;
}

static int solua_ble_gatt_connect(lua_State *L)
{
    size_t len;
    luaL_checktype(L, 1, LUA_TSTRING);
    const char *address = luaL_checklstring(L, 1, &len);
    uint8_t bda[6];
    if (!solar_os_ble_parse_address(address, len, bda)) {
        return luaL_error(L, "expected aa:bb:cc:dd:ee:ff");
    }
    const lua_Integer type = luaL_optinteger(L, 2, 0);
    if (type < 0 || type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY) {
        return luaL_error(L, "address type must be 0..3");
    }
    const uint32_t timeout = solua_ble_timeout(L, 3);
    solar_os_ble_peer_t peer;
    solua_ble_check(L, solar_os_ble_peer_connect(solua_ble_get(L), bda, type, timeout, &peer));
    lua_pushinteger(L, peer);
    return 1;
}

static int solua_ble_gatt_disconnect(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_peer_disconnect(solua_ble_get(L), solua_ble_peer(L)));
}

static int solua_ble_gatt_capacity(lua_State *L)
{
    lua_pushinteger(L, solar_os_ble_peer_capacity());
    return 1;
}

static int solua_ble_gatt_status(lua_State *L)
{
    solar_os_ble_session_info_t info;
    solua_ble_check(L, solar_os_ble_peer_get_info(solua_ble_get(L), solua_ble_peer(L), &info));
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
        info.gatt.bda[0], info.gatt.bda[1], info.gatt.bda[2],
        info.gatt.bda[3], info.gatt.bda[4], info.gatt.bda[5]);
    lua_createtable(L, 0, 10);
    solua_set_str(L, -1, "owner", info.owner);
    solua_set_str(L, -1, "address", address);
    solua_set_str(L, -1, "status", info.gatt.status);
    solua_set_bool(L, -1, "connected", info.gatt.connected);
    solua_set_bool(L, -1, "busy", info.busy);
    solua_set_bool(L, -1, "retiring", info.retiring);
    solua_set_int(L, -1, "addr_type", info.gatt.addr_type);
    solua_set_int(L, -1, "mtu", info.gatt.mtu);
    solua_set_int(L, -1, "service_count", info.gatt.service_count);
    solua_set_int(L, -1, "max_value_bytes", SOLAR_OS_BLE_GATT_VALUE_MAX);
    solua_set_int(L, -1, "event_capacity", info.event_capacity);
    solua_set_int(L, -1, "event_count", info.event_count);
    solua_set_int(L, -1, "events_dropped", info.events_dropped);
    return 1;
}

static int solua_ble_gatt_services(lua_State *L)
{
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t count = 0;
    solua_ble_check(L, solar_os_ble_peer_services(solua_ble_get(L), solua_ble_peer(L), services,
        SOLAR_OS_BLE_GATT_MAX_SERVICES, &count));
    lua_createtable(L, count, 0);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_SERVICES; i++) {
        lua_createtable(L, 0, 5);
        solua_set_int(L, -1, "index", i);
        solua_set_str(L, -1, "uuid", services[i].uuid);
        solua_set_bool(L, -1, "primary", services[i].primary);
        solua_set_int(L, -1, "start_handle", services[i].start_handle);
        solua_set_int(L, -1, "end_handle", services[i].end_handle);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int solua_ble_gatt_characteristics(lua_State *L)
{
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (index < 0 || index >= SOLAR_OS_BLE_GATT_MAX_SERVICES) {
        return luaL_error(L, "service index out of range");
    }
    solar_os_ble_gatt_characteristic_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS];
    size_t count = 0;
    solua_ble_check(L, solar_os_ble_peer_characteristics(solua_ble_get(L), solua_ble_peer(L), index,
        chars, SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, &count));
    lua_createtable(L, count, 0);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS; i++) {
        lua_createtable(L, 0, 3);
        solua_set_str(L, -1, "uuid", chars[i].uuid);
        solua_set_int(L, -1, "handle", chars[i].handle);
        solua_set_int(L, -1, "properties", chars[i].properties);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int solua_ble_gatt_read(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    const uint32_t timeout = solua_ble_timeout(L, 3);
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t len = 0;
    solua_ble_check(L, solar_os_ble_peer_read(solua_ble_get(L), solua_ble_peer(L), handle,
        value, sizeof(value), &len, timeout));
    lua_pushlstring(L, (const char *)value, len);
    return 1;
}

static int solua_ble_gatt_write(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    size_t len;
    luaL_checktype(L, 3, LUA_TSTRING);
    const char *value = luaL_checklstring(L, 3, &len);
    bool response = true;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TBOOLEAN);
        response = lua_toboolean(L, 4);
    }
    const uint32_t timeout = solua_ble_timeout(L, 5);
    return solua_ble_check(L, solar_os_ble_peer_write(solua_ble_get(L), solua_ble_peer(L), handle,
        (const uint8_t *)value, len, response, timeout));
}

static int solua_ble_gatt_subscribe(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    bool indication = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        indication = lua_toboolean(L, 3);
    }
    const uint32_t timeout = solua_ble_timeout(L, 4);
    return solua_ble_check(L, solar_os_ble_peer_subscribe(solua_ble_get(L), solua_ble_peer(L),
        handle, indication ? 2 : 1, timeout));
}

static int solua_ble_gatt_unsubscribe(lua_State *L)
{
    const uint16_t handle = solua_ble_handle(L, 2);
    const uint32_t timeout = solua_ble_timeout(L, 3);
    return solua_ble_check(L, solar_os_ble_peer_subscribe(solua_ble_get(L), solua_ble_peer(L), handle, 0, timeout));
}

static int solua_ble_gatt_configure_queue(lua_State *L)
{
    const lua_Integer capacity = luaL_checkinteger(L, 2);
    if (capacity <= 0 || (uint64_t)capacity > UINT32_MAX) return luaL_error(L, "invalid queue capacity");
    return solua_ble_check(L, solar_os_ble_peer_configure_queue(solua_ble_get(L), solua_ble_peer(L), capacity));
}

static int solua_ble_gatt_poll(lua_State *L)
{
    solar_os_ble_notification_t event;
    esp_err_t err = solar_os_ble_peer_poll(solua_ble_get(L), solua_ble_peer(L), &event);
    if (err == ESP_ERR_NOT_FOUND) { lua_pushnil(L); return 1; }
    solua_ble_check(L, err);
    lua_createtable(L, 0, 3);
    solua_set_int(L, -1, "handle", event.handle);
    solua_set_bool(L, -1, "indication", event.indication);
    lua_pushlstring(L, (const char *)event.value, event.value_len);
    lua_setfield(L, -2, "data");
    return 1;
}

static void solua_ble_server_text(lua_State *L, int index, solar_os_ble_server_request_t *r)
{
    size_t len;
    luaL_checktype(L, index, LUA_TSTRING);
    const char *text = luaL_checklstring(L, index, &len);
    if (!len || len >= sizeof(r->text) || memchr(text, 0, len)) luaL_error(L, "invalid name or UUID");
    memcpy(r->text, text, len);
}
static uint32_t solua_ble_server_id(lua_State *L, int index)
{
    lua_Integer value = luaL_checkinteger(L, index);
    if (value <= 0 || (uint64_t)value > UINT32_MAX) luaL_error(L, "invalid local BLE ID");
    return value;
}
static void solua_ble_server_value(lua_State *L, int index, solar_os_ble_server_request_t *r)
{
    size_t len;
    luaL_checktype(L, index, LUA_TSTRING);
    const char *value = luaL_checklstring(L, index, &len);
    if (len > sizeof(r->value)) luaL_error(L, "BLE value exceeds 128 bytes");
    r->value_len = len; memcpy(r->value, value, len);
}
static int solua_ble_server_create(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_CREATE};
    solua_ble_server_text(L, 1, &r);
    lua_Integer capacity = luaL_optinteger(L, 2, 16);
    if (capacity <= 0 || (uint64_t)capacity > UINT32_MAX) return luaL_error(L, "invalid queue capacity");
    r.capacity = capacity;
    return solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r));
}
static int solua_ble_server_service(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_SERVICE};
    solua_ble_server_text(L, 1, &r);
    solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r));
    lua_pushinteger(L, r.id); return 1;
}
static int solua_ble_server_characteristic(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_CHARACTERISTIC};
    r.parent = solua_ble_server_id(L, 1); solua_ble_server_text(L, 2, &r);
    lua_Integer properties = luaL_checkinteger(L, 3);
    if (properties < 1 || properties > 0x3e || (properties & ~0x3e)) return luaL_error(L, "invalid properties");
    r.properties = properties;
    if (!lua_isnoneornil(L, 4)) solua_ble_server_value(L, 4, &r);
    solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r));
    lua_pushinteger(L, r.id); return 1;
}
#define SOLUA_BLE_SERVER_SIMPLE(name, operation) \
    static int solua_ble_server_##name(lua_State *L) { \
        solar_os_ble_server_request_t r = {.op=operation}; \
        return solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r)); \
    }
SOLUA_BLE_SERVER_SIMPLE(start, SOLAR_OS_BLE_SERVER_START)
SOLUA_BLE_SERVER_SIMPLE(stop, SOLAR_OS_BLE_SERVER_STOP)
SOLUA_BLE_SERVER_SIMPLE(close, SOLAR_OS_BLE_SERVER_CLOSE)
#undef SOLUA_BLE_SERVER_SIMPLE
static int solua_ble_server_status(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_STATUS};
    solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r));
    lua_createtable(L, 0, 9);
    solua_set_bool(L, -1, "registered", r.info.registered);
    solua_set_bool(L, -1, "advertising", r.info.advertising);
    solua_set_bool(L, -1, "closing", r.info.closing);
    solua_set_int(L, -1, "peers", r.info.peers);
    solua_set_int(L, -1, "services", r.info.services);
    solua_set_int(L, -1, "characteristics", r.info.characteristics);
    solua_set_int(L, -1, "event_capacity", r.info.event_capacity);
    solua_set_int(L, -1, "event_count", r.info.event_count);
    solua_set_int(L, -1, "events_dropped", r.info.events_dropped);
    return 1;
}
static int solua_ble_server_poll(lua_State *L)
{
    static const char *types[] = {"connected", "disconnected", "read", "write", "subscribe", "sent"};
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_POLL};
    esp_err_t err = solar_os_ble_server_request(solua_ble_get(L), &r);
    if (err == ESP_ERR_NOT_FOUND) { lua_pushnil(L); return 1; }
    solua_ble_check(L, err);
    lua_createtable(L, 0, 8);
    solua_set_str(L, -1, "type", types[r.event.type]);
    solua_set_int(L, -1, "peer", r.event.peer);
    solua_set_int(L, -1, "characteristic", r.event.characteristic);
    solua_set_int(L, -1, "mtu", r.event.mtu);
    solua_set_int(L, -1, "status", r.event.status);
    solua_set_bool(L, -1, "notify", r.event.notify);
    solua_set_bool(L, -1, "indicate", r.event.indicate);
    lua_pushlstring(L, (const char *)r.event.value, r.event.value_len); lua_setfield(L, -2, "data");
    return 1;
}
static int solua_ble_server_peers(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_PEER};
    lua_newtable(L);
    for (int i = 1;; i++) {
        esp_err_t err = solar_os_ble_server_request(solua_ble_get(L), &r);
        if (err == ESP_ERR_NOT_FOUND) return 1;
        solua_ble_check(L, err);
        r.peer = r.event.peer;
        lua_createtable(L, 0, 3);
        solua_set_int(L, -1, "peer", r.event.peer);
        solua_set_int(L, -1, "mtu", r.event.mtu);
        solua_set_bool(L, -1, "connected", r.event.status == 0);
        lua_rawseti(L, -2, i);
    }
}
static int solua_ble_server_set(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_SET};
    r.id = solua_ble_server_id(L, 1); solua_ble_server_value(L, 2, &r);
    return solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r));
}
static int solua_ble_server_send(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_SEND};
    r.peer = solua_ble_server_id(L, 1); r.id = solua_ble_server_id(L, 2);
    solua_ble_server_value(L, 3, &r);
    if (!lua_isnoneornil(L, 4)) { luaL_checktype(L, 4, LUA_TBOOLEAN); r.indicate = lua_toboolean(L, 4); }
    return solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r));
}
static int solua_ble_server_disconnect(lua_State *L)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_DISCONNECT, .peer=solua_ble_server_id(L, 1)};
    return solua_ble_check(L, solar_os_ble_server_request(solua_ble_get(L), &r));
}

static int solua_ble_hid_start(lua_State *L)
{
    size_t len;
    luaL_checktype(L, 1, LUA_TSTRING);
    const char *name = luaL_checklstring(L, 1, &len);
    if (!len || len > 26 || memchr(name, 0, len)) {
        return luaL_error(L, "HID name must be 1..26 bytes");
    }
    char copied[27];
    memcpy(copied, name, len);
    copied[len] = '\0';
    return solua_ble_check(L, solar_os_ble_hid_device_start(solua_ble_get(L), copied));
}

static int solua_ble_hid_stop(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_hid_device_stop(solua_ble_get(L)));
}

static int solua_ble_hid_status(lua_State *L)
{
    solar_os_ble_hid_info_t info;
    solua_ble_check(L, solar_os_ble_hid_device_status(solua_ble_get(L), &info));
    lua_createtable(L, 0, 13);
    solua_set_bool(L, -1, "registered", info.registered);
    solua_set_bool(L, -1, "advertising", info.advertising);
    solua_set_bool(L, -1, "closing", info.closing);
    solua_set_bool(L, -1, "connected", info.connected);
    solua_set_bool(L, -1, "encrypted", info.encrypted);
    solua_set_bool(L, -1, "bonded", info.bonded);
    solua_set_bool(L, -1, "keyboard_subscribed", info.keyboard_subscribed);
    solua_set_bool(L, -1, "mouse_subscribed", info.mouse_subscribed);
    solua_set_bool(L, -1, "gamepad_subscribed", info.gamepad_subscribed);
    solua_set_int(L, -1, "keyboard_leds", info.keyboard_leds);
    solua_set_int(L, -1, "event_capacity", info.event_capacity);
    solua_set_int(L, -1, "event_count", info.event_count);
    solua_set_int(L, -1, "events_dropped", info.events_dropped);
    return 1;
}

static int solua_ble_hid_poll(lua_State *L)
{
    static const char *types[] = {
        "connected", "disconnected", "secured", "keyboard-leds",
    };
    solar_os_ble_hid_device_event_t event;
    const esp_err_t err = solar_os_ble_hid_device_poll(solua_ble_get(L), &event);
    if (err == ESP_ERR_NOT_FOUND) {
        lua_pushnil(L);
        return 1;
    }
    solua_ble_check(L, err);
    lua_createtable(L, 0, 6);
    solua_set_str(L, -1, "type", types[event.type]);
    solua_set_int(L, -1, "peer", event.peer);
    solua_set_int(L, -1, "status", event.status);
    solua_set_bool(L, -1, "encrypted", event.encrypted);
    solua_set_bool(L, -1, "bonded", event.bonded);
    solua_set_int(L, -1, "keyboard_leds", event.keyboard_leds);
    return 1;
}

static size_t solua_ble_hid_keys(lua_State *L, uint16_t keys[8])
{
    const int count = lua_gettop(L);
    if (count < 1 || count > 8) {
        luaL_error(L, "expected 1..8 keys");
    }
    for (int i = 0; i < count; i++) {
        const lua_Integer key = luaL_checkinteger(L, i + 1);
        if (key < 0 || (uint64_t)key > UINT16_MAX) {
            luaL_error(L, "invalid HID key");
        }
        keys[i] = (uint16_t)key;
    }
    return (size_t)count;
}

static int solua_ble_hid_keyboard_press(lua_State *L)
{
    uint16_t keys[8];
    return solua_ble_check(L, solar_os_ble_hid_keyboard_press(
        solua_ble_get(L), keys, solua_ble_hid_keys(L, keys)));
}

static int solua_ble_hid_keyboard_release(lua_State *L)
{
    uint16_t keys[8];
    return solua_ble_check(L, solar_os_ble_hid_keyboard_release(
        solua_ble_get(L), keys, solua_ble_hid_keys(L, keys)));
}

static int solua_ble_hid_keyboard_release_all(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_hid_keyboard_release_all(solua_ble_get(L)));
}

static int32_t solua_ble_hid_i32(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < INT32_MIN || value > INT32_MAX) {
        luaL_error(L, "expected 32-bit signed integer");
    }
    return (int32_t)value;
}

static uint8_t solua_ble_hid_u8(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > UINT8_MAX) {
        luaL_error(L, "expected value 0..255");
    }
    return (uint8_t)value;
}

static bool solua_ble_hid_bool(lua_State *L, int index)
{
    return lua_toboolean(L, index);
}

static int solua_ble_hid_mouse_move(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_hid_mouse_move(
        solua_ble_get(L), solua_ble_hid_i32(L, 1), solua_ble_hid_i32(L, 2)));
}

static int solua_ble_hid_mouse_button(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_hid_mouse_button(
        solua_ble_get(L), solua_ble_hid_u8(L, 1), solua_ble_hid_bool(L, 2)));
}

static int solua_ble_hid_gamepad_axis(lua_State *L)
{
    const lua_Integer axis = luaL_checkinteger(L, 1);
    const lua_Integer value = luaL_checkinteger(L, 2);
    if (axis < INT_MIN || axis > INT_MAX) {
        return luaL_error(L, "invalid gamepad axis");
    }
    if (value < INT16_MIN || value > INT16_MAX) {
        return luaL_error(L, "expected axis value -32768..32767");
    }
    return solua_ble_check(L, solar_os_ble_hid_gamepad_axis(
        solua_ble_get(L), (int)axis, (int16_t)value));
}

static int solua_ble_hid_gamepad_button(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_hid_gamepad_button(
        solua_ble_get(L), solua_ble_hid_u8(L, 1), solua_ble_hid_bool(L, 2)));
}

static int solua_ble_hid_gamepad_hat(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_hid_gamepad_hat(
        solua_ble_get(L), solua_ble_hid_u8(L, 1)));
}

static int solua_ble_hid_gamepad_send(lua_State *L)
{
    return solua_ble_check(L, solar_os_ble_hid_gamepad_send(solua_ble_get(L)));
}
