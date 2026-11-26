/* Package-owned TCP stream bindings; the core owns named endpoints. */
static const solar_os_native_tcp_stream_api_v1_t *python_tcp_stream_cached_api;
static const solar_os_native_tcp_stream_api_v1_t *python_tcp_stream_api(void)
{
    if (!python_tcp_stream_cached_api) {
        python_tcp_stream_cached_api = solar_os_native_tcp_stream_get(SOLAR_OS_SCRIPT_SERVICE_QUERY(tcp_stream),
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_tcp_stream_api_v1_t, remove));
        if (!python_tcp_stream_cached_api)
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("TCP stream service unavailable"));
    }
    return python_tcp_stream_cached_api;
}
static void python_tcp_stream_cleanup(void) { python_tcp_stream_cached_api = NULL; }
static const char *python_tcp_stream_text(mp_obj_t object)
{
    size_t length;
    const char *value = mp_obj_str_get_data(object, &length);
    if (!length || memchr(value, 0, length)) mp_raise_ValueError(MP_ERROR_TEXT("invalid TCP stream name or host"));
    return value;
}
static void python_tcp_stream_check(int result)
{ if (result) mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("TCP stream error %d"), result); }
static mp_obj_t solaros_streams_tcp_connect(mp_obj_t name, mp_obj_t host, mp_obj_t port)
{
    uint32_t number = python_u32_from_obj(port);
    if (!number || number > UINT16_MAX) mp_raise_ValueError(MP_ERROR_TEXT("invalid port"));
    python_tcp_stream_check(python_tcp_stream_api()->connect(python_tcp_stream_text(name), python_tcp_stream_text(host), number));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_streams_tcp_connect_obj, solaros_streams_tcp_connect);
static mp_obj_t solaros_streams_tcp_listen(mp_obj_t name, mp_obj_t port)
{
    uint32_t number = python_u32_from_obj(port);
    if (!number || number > UINT16_MAX) mp_raise_ValueError(MP_ERROR_TEXT("invalid port"));
    python_tcp_stream_check(python_tcp_stream_api()->listen(python_tcp_stream_text(name), number));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_streams_tcp_listen_obj, solaros_streams_tcp_listen);
static mp_obj_t solaros_streams_tcp_remove(mp_obj_t name)
{
    python_tcp_stream_check(python_tcp_stream_api()->remove(python_tcp_stream_text(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_tcp_remove_obj, solaros_streams_tcp_remove);
