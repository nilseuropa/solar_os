/* Package-owned Bluetooth byte-stream management; definitions live in core. */
static const solar_os_native_ble_stream_api_v1_t *python_ble_stream_cached_api;
static const solar_os_native_ble_stream_api_v1_t *python_ble_stream_api(void)
{
    if (!python_ble_stream_cached_api) {
        python_ble_stream_cached_api = solar_os_native_ble_stream_get(
            SOLAR_OS_SCRIPT_SERVICE_QUERY(ble_stream),
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_ble_stream_api_v1_t, status));
        if (!python_ble_stream_cached_api) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Bluetooth stream service unavailable"));
    }
    return python_ble_stream_cached_api;
}
static void python_ble_stream_cleanup(void)
{ python_ble_stream_cached_api = NULL; }
static const char *python_ble_stream_name(mp_obj_t object)
{
    size_t length;
    const char *name = mp_obj_str_get_data(object, &length);
    if (!length || memchr(name, 0, length))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid stream name"));
    return name;
}
static mp_obj_t solaros_streams_ble_connect(size_t n_args, const mp_obj_t *args)
{
    size_t len;
    const char *address = mp_obj_str_get_data(args[1], &len);
    uint8_t bda[6];
    if (!python_ble_api()->parse_address(address, len, bda))
        mp_raise_ValueError(MP_ERROR_TEXT("expected aa:bb:cc:dd:ee:ff"));
    uint32_t type = n_args > 2 ? python_u32_from_obj(args[2]) : 0;
    if (type > 3) mp_raise_ValueError(MP_ERROR_TEXT("address type must be 0..3"));
    python_ble_check(python_ble_stream_api()->connect(python_ble_stream_name(args[0]), bda, type));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_streams_ble_connect_obj, 2, 3, solaros_streams_ble_connect);
static mp_obj_t solaros_streams_ble_listen(mp_obj_t name)
{
    python_ble_check(python_ble_stream_api()->listen(python_ble_stream_name(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_ble_listen_obj, solaros_streams_ble_listen);
static mp_obj_t solaros_streams_ble_remove(mp_obj_t name)
{
    python_ble_check(python_ble_stream_api()->remove(python_ble_stream_name(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_ble_remove_obj, solaros_streams_ble_remove);
