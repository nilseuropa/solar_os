/* Included inside the BLE package gate, after common interpreter helpers. */
static solar_os_ble_session_t python_ble_session;

static void python_ble_check(esp_err_t err)
{
    if (err == SOLAR_OS_BLE_ERR_CANCELLED) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("BLE operation cancelled"));
    }
    if (err == SOLAR_OS_BLE_ERR_CAPACITY) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("BLE connection capacity exhausted"));
    }
    python_check_esp(err);
}

static mp_obj_t solaros_ble_scan(void)
{
    solar_os_ble_scan_result_t results[SOLAR_OS_BLE_SCAN_MAX_RESULTS];
    size_t found = 0;
    if (python_should_cancel(NULL)) {
        python_ble_check(SOLAR_OS_BLE_ERR_CANCELLED);
    }
    const esp_err_t err = solar_os_ble_scan(results, SOLAR_OS_BLE_SCAN_MAX_RESULTS, &found);
    /* Discovery reports NOT_FOUND when no advertisement matched. */
    if (err != ESP_ERR_NOT_FOUND || found != 0) {
        python_ble_check(err);
    }
    if (python_should_cancel(NULL)) {
        python_ble_check(SOLAR_OS_BLE_ERR_CANCELLED);
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < found && i < SOLAR_OS_BLE_SCAN_MAX_RESULTS; i++) {
        const solar_os_ble_scan_result_t *device = &results[i];
        char address[18];
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
            device->bda[0], device->bda[1], device->bda[2],
            device->bda[3], device->bda[4], device->bda[5]);
        mp_obj_t dict = mp_obj_new_dict(9);
        python_dict_store_cstr(dict, "address", address);
        python_dict_store_cstr(dict, "name", device->name);
        python_dict_store_int(dict, "addr_type", device->addr_type);
        python_dict_store_int(dict, "rssi", device->rssi);
        python_dict_store_int(dict, "appearance", device->appearance);
        python_dict_store_bool(dict, "hid_service", device->hid_service);
        python_dict_store_bool(dict, "keyboard_like", device->keyboard_like);
        python_dict_store_bool(dict, "remembered", device->remembered);
        python_dict_store_bool(dict, "connected", device->connected);
        mp_obj_list_append(list, dict);
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_scan_obj, solaros_ble_scan);

static solar_os_ble_session_t python_ble_get(void)
{
    if (python_should_cancel(NULL)) {
        python_ble_check(SOLAR_OS_BLE_ERR_CANCELLED);
    }
    if (python_ble_session == SOLAR_OS_BLE_SESSION_INVALID) {
        python_ble_check(solar_os_ble_session_create(
            python_runner_control != NULL ? "python.runner" : "python.app",
            &python_ble_session));
        python_ble_check(solar_os_ble_session_set_cancel_check(
            python_ble_session, python_should_cancel, NULL));
    }
    return python_ble_session;
}

static void python_ble_destroy(void)
{
    if (python_ble_session != SOLAR_OS_BLE_SESSION_INVALID) {
        (void)solar_os_ble_session_close(python_ble_session);
        python_ble_session = SOLAR_OS_BLE_SESSION_INVALID;
    }
}

static uint32_t python_ble_timeout(size_t n_args, const mp_obj_t *args, size_t index)
{
    const uint32_t timeout = python_optional_u32(n_args, args, index, 0);
    if (timeout > 60000U) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be 0..60000 ms"));
    }
    return timeout;
}

static uint16_t python_ble_handle(mp_obj_t arg)
{
    const mp_int_t handle = mp_obj_get_int(arg);
    if (handle < 1 || handle > UINT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("handle must be 1..65535"));
    }
    return (uint16_t)handle;
}

static mp_obj_t solaros_ble_gatt_connect(size_t n_args, const mp_obj_t *args)
{
    size_t len;
    const char *address = mp_obj_str_get_data(args[0], &len);
    uint8_t bda[6];
    if (!solar_os_ble_parse_address(address, len, bda)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected aa:bb:cc:dd:ee:ff"));
    }
    const uint32_t type = python_optional_u32(n_args, args, 1, 0);
    if (type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY) {
        mp_raise_ValueError(MP_ERROR_TEXT("address type must be 0..3"));
    }
    const uint32_t timeout = python_ble_timeout(n_args, args, 2);
    solar_os_ble_peer_t peer;
    python_ble_check(solar_os_ble_peer_connect(python_ble_get(), bda, type, timeout, &peer));
    return mp_obj_new_int_from_uint(peer);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_connect_obj, 1, 3, solaros_ble_gatt_connect);

static mp_obj_t solaros_ble_gatt_disconnect(mp_obj_t peer_obj)
{
    python_ble_check(solar_os_ble_peer_disconnect(python_ble_get(), python_u32_from_obj(peer_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_disconnect_obj, solaros_ble_gatt_disconnect);

static mp_obj_t solaros_ble_gatt_capacity(void)
{
    return mp_obj_new_int_from_uint(solar_os_ble_peer_capacity());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_gatt_capacity_obj, solaros_ble_gatt_capacity);

static mp_obj_t solaros_ble_gatt_status(mp_obj_t peer_obj)
{
    solar_os_ble_session_info_t info;
    python_ble_check(solar_os_ble_peer_get_info(python_ble_get(), python_u32_from_obj(peer_obj), &info));
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x",
        info.gatt.bda[0], info.gatt.bda[1], info.gatt.bda[2],
        info.gatt.bda[3], info.gatt.bda[4], info.gatt.bda[5]);
    mp_obj_t result = mp_obj_new_dict(10);
    python_dict_store_cstr(result, "owner", info.owner);
    python_dict_store_cstr(result, "address", address);
    python_dict_store_cstr(result, "status", info.gatt.status);
    python_dict_store_bool(result, "connected", info.gatt.connected);
    python_dict_store_bool(result, "busy", info.busy);
    python_dict_store_bool(result, "retiring", info.retiring);
    python_dict_store_uint(result, "addr_type", info.gatt.addr_type);
    python_dict_store_uint(result, "mtu", info.gatt.mtu);
    python_dict_store_uint(result, "service_count", info.gatt.service_count);
    python_dict_store_uint(result, "max_value_bytes", SOLAR_OS_BLE_GATT_VALUE_MAX);
    python_dict_store_uint(result, "event_capacity", info.event_capacity);
    python_dict_store_uint(result, "event_count", info.event_count);
    python_dict_store_uint(result, "events_dropped", info.events_dropped);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_status_obj, solaros_ble_gatt_status);

static mp_obj_t solaros_ble_gatt_services(mp_obj_t peer_obj)
{
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t count = 0;
    python_ble_check(solar_os_ble_peer_services(python_ble_get(), python_u32_from_obj(peer_obj), services,
        SOLAR_OS_BLE_GATT_MAX_SERVICES, &count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_SERVICES; i++) {
        mp_obj_t item = mp_obj_new_dict(5);
        python_dict_store_uint(item, "index", i);
        python_dict_store_cstr(item, "uuid", services[i].uuid);
        python_dict_store_bool(item, "primary", services[i].primary);
        python_dict_store_uint(item, "start_handle", services[i].start_handle);
        python_dict_store_uint(item, "end_handle", services[i].end_handle);
        mp_obj_list_append(result, item);
    }
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_services_obj, solaros_ble_gatt_services);

static mp_obj_t solaros_ble_gatt_characteristics(mp_obj_t peer_obj, mp_obj_t index_obj)
{
    const uint32_t index = python_u32_from_obj(index_obj);
    if (index >= SOLAR_OS_BLE_GATT_MAX_SERVICES) {
        mp_raise_ValueError(MP_ERROR_TEXT("service index out of range"));
    }
    solar_os_ble_gatt_characteristic_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS];
    size_t count = 0;
    python_ble_check(solar_os_ble_peer_characteristics(python_ble_get(), python_u32_from_obj(peer_obj), index,
        chars, SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, &count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count && i < SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS; i++) {
        mp_obj_t item = mp_obj_new_dict(3);
        python_dict_store_cstr(item, "uuid", chars[i].uuid);
        python_dict_store_uint(item, "handle", chars[i].handle);
        python_dict_store_uint(item, "properties", chars[i].properties);
        mp_obj_list_append(result, item);
    }
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_gatt_characteristics_obj, solaros_ble_gatt_characteristics);

static mp_obj_t solaros_ble_gatt_read(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    const uint32_t timeout = python_ble_timeout(n_args, args, 2);
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t len = 0;
    python_ble_check(solar_os_ble_peer_read(python_ble_get(), python_u32_from_obj(args[0]), handle,
        value, sizeof(value), &len, timeout));
    return mp_obj_new_bytes(value, len);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_read_obj, 2, 3, solaros_ble_gatt_read);

static mp_obj_t solaros_ble_gatt_write(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    mp_buffer_info_t data;
    mp_get_buffer_raise(args[2], &data, MP_BUFFER_READ);
    bool response = true;
    if (n_args > 3 && args[3] != mp_const_none) {
        if (args[3] != mp_const_true && args[3] != mp_const_false) {
            mp_raise_ValueError(MP_ERROR_TEXT("with_response must be boolean"));
        }
        response = args[3] == mp_const_true;
    }
    const uint32_t timeout = python_ble_timeout(n_args, args, 4);
    python_ble_check(solar_os_ble_peer_write(python_ble_get(), python_u32_from_obj(args[0]), handle,
        data.buf, data.len, response, timeout));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_write_obj, 3, 5, solaros_ble_gatt_write);

static mp_obj_t solaros_ble_gatt_subscribe(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    bool indication = false;
    if (n_args > 2 && args[2] != mp_const_none) {
        if (args[2] != mp_const_true && args[2] != mp_const_false)
            mp_raise_ValueError(MP_ERROR_TEXT("indicate must be boolean"));
        indication = args[2] == mp_const_true;
    }
    const uint32_t timeout = python_ble_timeout(n_args, args, 3);
    python_ble_check(solar_os_ble_peer_subscribe(python_ble_get(), python_u32_from_obj(args[0]),
        handle, indication ? 2 : 1, timeout));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_subscribe_obj, 2, 4, solaros_ble_gatt_subscribe);

static mp_obj_t solaros_ble_gatt_unsubscribe(size_t n_args, const mp_obj_t *args)
{
    const uint16_t handle = python_ble_handle(args[1]);
    const uint32_t timeout = python_ble_timeout(n_args, args, 2);
    python_ble_check(solar_os_ble_peer_subscribe(python_ble_get(), python_u32_from_obj(args[0]), handle, 0, timeout));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_gatt_unsubscribe_obj, 2, 3, solaros_ble_gatt_unsubscribe);

static mp_obj_t solaros_ble_gatt_configure_queue(mp_obj_t peer_obj, mp_obj_t capacity_obj)
{
    const uint32_t capacity = python_u32_from_obj(capacity_obj);
    python_ble_check(solar_os_ble_peer_configure_queue(python_ble_get(), python_u32_from_obj(peer_obj), capacity));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_gatt_configure_queue_obj, solaros_ble_gatt_configure_queue);

static mp_obj_t solaros_ble_gatt_poll(mp_obj_t peer_obj)
{
    solar_os_ble_notification_t event;
    esp_err_t err = solar_os_ble_peer_poll(python_ble_get(), python_u32_from_obj(peer_obj), &event);
    if (err == ESP_ERR_NOT_FOUND) return mp_const_none;
    python_ble_check(err);
    mp_obj_t result = mp_obj_new_dict(3);
    python_dict_store_uint(result, "handle", event.handle);
    python_dict_store_bool(result, "indication", event.indication);
    mp_obj_dict_store(result, python_key("data"), mp_obj_new_bytes(event.value, event.value_len));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_gatt_poll_obj, solaros_ble_gatt_poll);

static void python_ble_server_text(mp_obj_t arg, solar_os_ble_server_request_t *r)
{
    size_t len;
    const char *text = mp_obj_str_get_data(arg, &len);
    if (!len || len >= sizeof(r->text) || memchr(text, 0, len))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid name or UUID"));
    memcpy(r->text, text, len);
}
static uint32_t python_ble_server_id(mp_obj_t arg)
{
    uint32_t id = python_u32_from_obj(arg);
    if (!id) mp_raise_ValueError(MP_ERROR_TEXT("invalid local BLE ID"));
    return id;
}
static void python_ble_server_value(mp_obj_t arg, solar_os_ble_server_request_t *r)
{
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(arg, &buffer, MP_BUFFER_READ);
    if (buffer.len > sizeof(r->value)) mp_raise_ValueError(MP_ERROR_TEXT("BLE value exceeds 128 bytes"));
    r->value_len = buffer.len; memcpy(r->value, buffer.buf, buffer.len);
}
static mp_obj_t solaros_ble_server_create(size_t n_args, const mp_obj_t *args)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_CREATE};
    python_ble_server_text(args[0], &r);
    r.capacity = python_optional_u32(n_args, args, 1, 16);
    if (!r.capacity) mp_raise_ValueError(MP_ERROR_TEXT("invalid queue capacity"));
    python_ble_check(solar_os_ble_server_request(python_ble_get(), &r));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_server_create_obj, 1, 2, solaros_ble_server_create);
static mp_obj_t solaros_ble_server_service(mp_obj_t uuid)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_SERVICE};
    python_ble_server_text(uuid, &r);
    python_ble_check(solar_os_ble_server_request(python_ble_get(), &r));
    return mp_obj_new_int_from_uint(r.id);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_server_service_obj, solaros_ble_server_service);
static mp_obj_t solaros_ble_server_characteristic(size_t n_args, const mp_obj_t *args)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_CHARACTERISTIC};
    r.parent = python_ble_server_id(args[0]); python_ble_server_text(args[1], &r);
    uint32_t properties = python_u32_from_obj(args[2]);
    if (!properties || (properties & ~0x3eU)) mp_raise_ValueError(MP_ERROR_TEXT("invalid properties"));
    r.properties = properties;
    if (n_args > 3 && args[3] != mp_const_none) python_ble_server_value(args[3], &r);
    python_ble_check(solar_os_ble_server_request(python_ble_get(), &r));
    return mp_obj_new_int_from_uint(r.id);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_server_characteristic_obj, 3, 4, solaros_ble_server_characteristic);
#define PYTHON_BLE_SERVER_SIMPLE(name, operation) \
    static mp_obj_t solaros_ble_server_##name(void) { \
        solar_os_ble_server_request_t r = {.op=operation}; \
        python_ble_check(solar_os_ble_server_request(python_ble_get(), &r)); \
        return mp_const_none; \
    } \
    MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_server_##name##_obj, solaros_ble_server_##name);
PYTHON_BLE_SERVER_SIMPLE(start, SOLAR_OS_BLE_SERVER_START)
PYTHON_BLE_SERVER_SIMPLE(stop, SOLAR_OS_BLE_SERVER_STOP)
PYTHON_BLE_SERVER_SIMPLE(close, SOLAR_OS_BLE_SERVER_CLOSE)
#undef PYTHON_BLE_SERVER_SIMPLE
static mp_obj_t solaros_ble_server_status(void)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_STATUS};
    python_ble_check(solar_os_ble_server_request(python_ble_get(), &r));
    mp_obj_t result = mp_obj_new_dict(9);
    python_dict_store_bool(result, "registered", r.info.registered);
    python_dict_store_bool(result, "advertising", r.info.advertising);
    python_dict_store_bool(result, "closing", r.info.closing);
    python_dict_store_uint(result, "peers", r.info.peers);
    python_dict_store_uint(result, "services", r.info.services);
    python_dict_store_uint(result, "characteristics", r.info.characteristics);
    python_dict_store_uint(result, "event_capacity", r.info.event_capacity);
    python_dict_store_uint(result, "event_count", r.info.event_count);
    python_dict_store_uint(result, "events_dropped", r.info.events_dropped);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_server_status_obj, solaros_ble_server_status);
static mp_obj_t solaros_ble_server_poll(void)
{
    static const char *types[] = {"connected", "disconnected", "read", "write", "subscribe", "sent"};
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_POLL};
    esp_err_t err = solar_os_ble_server_request(python_ble_get(), &r);
    if (err == ESP_ERR_NOT_FOUND) return mp_const_none;
    python_ble_check(err);
    mp_obj_t result = mp_obj_new_dict(8);
    python_dict_store_cstr(result, "type", types[r.event.type]);
    python_dict_store_uint(result, "peer", r.event.peer);
    python_dict_store_uint(result, "characteristic", r.event.characteristic);
    python_dict_store_uint(result, "mtu", r.event.mtu);
    python_dict_store_uint(result, "status", r.event.status);
    python_dict_store_bool(result, "notify", r.event.notify);
    python_dict_store_bool(result, "indicate", r.event.indicate);
    mp_obj_dict_store(result, mp_obj_new_str("data", 4), mp_obj_new_bytes(r.event.value, r.event.value_len));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_server_poll_obj, solaros_ble_server_poll);
static mp_obj_t solaros_ble_server_peers(void)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_PEER};
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (;;) {
        esp_err_t err = solar_os_ble_server_request(python_ble_get(), &r);
        if (err == ESP_ERR_NOT_FOUND) return result;
        python_ble_check(err); r.peer = r.event.peer;
        mp_obj_t item = mp_obj_new_dict(3);
        python_dict_store_uint(item, "peer", r.event.peer);
        python_dict_store_uint(item, "mtu", r.event.mtu);
        python_dict_store_bool(item, "connected", r.event.status == 0);
        mp_obj_list_append(result, item);
    }
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_server_peers_obj, solaros_ble_server_peers);
static mp_obj_t solaros_ble_server_set(mp_obj_t id, mp_obj_t data)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_SET, .id=python_ble_server_id(id)};
    python_ble_server_value(data, &r);
    python_ble_check(solar_os_ble_server_request(python_ble_get(), &r));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_server_set_obj, solaros_ble_server_set);
static mp_obj_t solaros_ble_server_send(size_t n_args, const mp_obj_t *args)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_SEND};
    r.peer = python_ble_server_id(args[0]); r.id = python_ble_server_id(args[1]);
    python_ble_server_value(args[2], &r);
    if (n_args > 3 && args[3] != mp_const_none) {
        if (args[3] != mp_const_true && args[3] != mp_const_false)
            mp_raise_ValueError(MP_ERROR_TEXT("indicate must be boolean"));
        r.indicate = args[3] == mp_const_true;
    }
    python_ble_check(solar_os_ble_server_request(python_ble_get(), &r));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_server_send_obj, 3, 4, solaros_ble_server_send);
static mp_obj_t solaros_ble_server_disconnect(mp_obj_t peer)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_DISCONNECT, .peer=python_ble_server_id(peer)};
    python_ble_check(solar_os_ble_server_request(python_ble_get(), &r));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_server_disconnect_obj, solaros_ble_server_disconnect);

static mp_obj_t solaros_ble_hid_start(mp_obj_t name_obj)
{
    size_t len;
    const char *name = mp_obj_str_get_data(name_obj, &len);
    if (!len || len > 26 || memchr(name, 0, len))
        mp_raise_ValueError(MP_ERROR_TEXT("HID name must be 1..26 bytes"));
    char copied[27]; memcpy(copied, name, len); copied[len] = 0;
    python_ble_check(solar_os_ble_hid_device_start(python_ble_get(), copied));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_hid_start_obj, solaros_ble_hid_start);

static mp_obj_t solaros_ble_hid_pair(void)
{
    python_ble_check(solar_os_ble_hid_device_pair(python_ble_get()));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_hid_pair_obj, solaros_ble_hid_pair);

static mp_obj_t solaros_ble_hid_stop(void)
{
    python_ble_check(solar_os_ble_hid_device_stop(python_ble_get()));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_hid_stop_obj, solaros_ble_hid_stop);

static mp_obj_t solaros_ble_hid_status(void)
{
    solar_os_ble_hid_info_t info;
    python_ble_check(solar_os_ble_hid_device_status(python_ble_get(), &info));
    mp_obj_t result = mp_obj_new_dict(13);
    python_dict_store_bool(result, "registered", info.registered);
    python_dict_store_bool(result, "advertising", info.advertising);
    python_dict_store_bool(result, "closing", info.closing);
    python_dict_store_bool(result, "connected", info.connected);
    python_dict_store_bool(result, "encrypted", info.encrypted);
    python_dict_store_bool(result, "bonded", info.bonded);
    python_dict_store_bool(result, "keyboard_subscribed", info.keyboard_subscribed);
    python_dict_store_bool(result, "mouse_subscribed", info.mouse_subscribed);
    python_dict_store_bool(result, "gamepad_subscribed", info.gamepad_subscribed);
    python_dict_store_uint(result, "keyboard_leds", info.keyboard_leds);
    python_dict_store_uint(result, "event_capacity", info.event_capacity);
    python_dict_store_uint(result, "event_count", info.event_count);
    python_dict_store_uint(result, "events_dropped", info.events_dropped);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_hid_status_obj, solaros_ble_hid_status);

static mp_obj_t solaros_ble_hid_poll(void)
{
    static const char *types[] = {
        "connected", "disconnected", "secured", "keyboard-leds", "passkey",
    };
    solar_os_ble_hid_device_event_t event;
    esp_err_t err = solar_os_ble_hid_device_poll(python_ble_get(), &event);
    if (err == ESP_ERR_NOT_FOUND) return mp_const_none;
    python_ble_check(err);
    mp_obj_t result = mp_obj_new_dict(7);
    python_dict_store_cstr(result, "type", types[event.type]);
    python_dict_store_uint(result, "peer", event.peer);
    python_dict_store_uint(result, "passkey", event.passkey);
    python_dict_store_uint(result, "status", event.status);
    python_dict_store_bool(result, "encrypted", event.encrypted);
    python_dict_store_bool(result, "bonded", event.bonded);
    python_dict_store_uint(result, "keyboard_leds", event.keyboard_leds);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_hid_poll_obj, solaros_ble_hid_poll);

static size_t python_ble_hid_keys(size_t n_args, const mp_obj_t *args, uint16_t keys[8])
{
    if (!n_args || n_args > 8) mp_raise_ValueError(MP_ERROR_TEXT("expected 1..8 keys"));
    for (size_t i = 0; i < n_args; i++) {
        const mp_int_t value = mp_obj_get_int(args[i]);
        if (value < 0 || value > UINT16_MAX)
            mp_raise_ValueError(MP_ERROR_TEXT("invalid HID key"));
        keys[i] = (uint16_t)value;
    }
    return n_args;
}

static mp_obj_t solaros_ble_hid_keyboard_press(size_t n_args, const mp_obj_t *args)
{
    uint16_t keys[8];
    python_ble_check(solar_os_ble_hid_keyboard_press(
        python_ble_get(), keys, python_ble_hid_keys(n_args, args, keys)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_hid_keyboard_press_obj, 1, 8,
                                    solaros_ble_hid_keyboard_press);

static mp_obj_t solaros_ble_hid_keyboard_release(size_t n_args, const mp_obj_t *args)
{
    uint16_t keys[8];
    python_ble_check(solar_os_ble_hid_keyboard_release(
        python_ble_get(), keys, python_ble_hid_keys(n_args, args, keys)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ble_hid_keyboard_release_obj, 1, 8,
                                    solaros_ble_hid_keyboard_release);

static mp_obj_t solaros_ble_hid_keyboard_release_all(void)
{
    python_ble_check(solar_os_ble_hid_keyboard_release_all(python_ble_get()));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_hid_keyboard_release_all_obj,
                          solaros_ble_hid_keyboard_release_all);

static mp_obj_t solaros_ble_hid_mouse_move(mp_obj_t x_obj, mp_obj_t y_obj)
{
    python_ble_check(solar_os_ble_hid_mouse_move(
        python_ble_get(), python_i32_from_obj(x_obj), python_i32_from_obj(y_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_hid_mouse_move_obj, solaros_ble_hid_mouse_move);

static mp_obj_t solaros_ble_hid_mouse_button(mp_obj_t button_obj, mp_obj_t pressed_obj)
{
    python_ble_check(solar_os_ble_hid_mouse_button(
        python_ble_get(), python_u8_from_obj(button_obj), mp_obj_is_true(pressed_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_hid_mouse_button_obj, solaros_ble_hid_mouse_button);

static mp_obj_t solaros_ble_hid_gamepad_axis(mp_obj_t axis_obj, mp_obj_t value_obj)
{
    const mp_int_t value = mp_obj_get_int(value_obj);
    if (value < INT16_MIN || value > INT16_MAX)
        mp_raise_ValueError(MP_ERROR_TEXT("expected axis value -32768..32767"));
    python_ble_check(solar_os_ble_hid_gamepad_axis(
        python_ble_get(), mp_obj_get_int(axis_obj), (int16_t)value));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_hid_gamepad_axis_obj, solaros_ble_hid_gamepad_axis);

static mp_obj_t solaros_ble_hid_gamepad_button(mp_obj_t button_obj, mp_obj_t pressed_obj)
{
    python_ble_check(solar_os_ble_hid_gamepad_button(
        python_ble_get(), python_u8_from_obj(button_obj), mp_obj_is_true(pressed_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_ble_hid_gamepad_button_obj, solaros_ble_hid_gamepad_button);

static mp_obj_t solaros_ble_hid_gamepad_hat(mp_obj_t hat_obj)
{
    python_ble_check(solar_os_ble_hid_gamepad_hat(
        python_ble_get(), python_u8_from_obj(hat_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_ble_hid_gamepad_hat_obj, solaros_ble_hid_gamepad_hat);

static mp_obj_t solaros_ble_hid_gamepad_send(void)
{
    python_ble_check(solar_os_ble_hid_gamepad_send(python_ble_get()));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_ble_hid_gamepad_send_obj, solaros_ble_hid_gamepad_send);
