#if SOLAR_OS_PACKAGE_SERVICE_DLP_CLIENT
typedef struct {
    mp_obj_base_t base;
    unsigned slot;
    uint32_t generation;
} python_dlp_t;
static const mp_obj_type_t solaros_dlp_connection_type;

static bool python_dlp_live(const python_dlp_t *self)
{
    return python_runtime_state && self->slot < 4 &&
        python_runtime_state->dlp_generation[self->slot] == self->generation &&
        python_runtime_state->dlp_links[self->slot];
}
static solar_os_display_link_t *python_dlp_link(mp_obj_t object)
{
    python_dlp_t *self = MP_OBJ_TO_PTR(object);
    if (!python_dlp_live(self)) mp_raise_ValueError(MP_ERROR_TEXT("DLP connection is closed"));
    return python_runtime_state->dlp_links[self->slot];
}
static void python_dlp_destroy(void)
{
    if (!python_runtime_state) return;
    for (unsigned i = 0; i < 4; i++) {
        solar_os_display_link_destroy(python_runtime_state->dlp_links[i]);
        python_runtime_state->dlp_links[i] = NULL;
    }
}
static mp_obj_t solaros_dlp_connection_close(mp_obj_t object)
{
    python_dlp_t *self = MP_OBJ_TO_PTR(object);
    if (python_dlp_live(self)) {
        solar_os_display_link_destroy(python_runtime_state->dlp_links[self->slot]);
        python_runtime_state->dlp_links[self->slot] = NULL;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_dlp_connection_close_obj, solaros_dlp_connection_close);
static mp_obj_t solaros_dlp_connection_info(mp_obj_t object)
{
    solar_os_display_link_t *link = python_dlp_link(object);
    const sdlp_info_t *info = solar_os_display_link_info(link);
    unsigned format = solar_os_display_link_format(link);
    mp_obj_t dict = mp_obj_new_dict(10);
    python_dict_store_int(dict, "width", info->width);
    python_dict_store_int(dict, "height", info->height);
    python_dict_store_int(dict, "format", format);
    python_dict_store_int(dict, "stride", sdlp_row_bytes(info->width, format));
    python_dict_store_int(dict, "frame_bytes", sdlp_row_bytes(info->width, format) * info->height);
    python_dict_store_int(dict, "formats", info->formats);
    python_dict_store_int(dict, "capabilities", info->capabilities);
    python_dict_store_cstr(dict, "model", info->model);
    python_dict_store_cstr(dict, "firmware", info->firmware);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_dlp_connection_info_obj, solaros_dlp_connection_info);
static mp_obj_t solaros_dlp_connection_present(mp_obj_t object, mp_obj_t pixels)
{
    solar_os_display_link_t *link = python_dlp_link(object);
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(pixels, &buffer, MP_BUFFER_READ);
    python_check_esp(solar_os_display_link_present(link, buffer.buf, buffer.len));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_dlp_connection_present_obj, solaros_dlp_connection_present);
static mp_obj_t solaros_dlp_connection_enter(mp_obj_t object)
{
    (void)python_dlp_link(object);
    return object;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_dlp_connection_enter_obj, solaros_dlp_connection_enter);
static mp_obj_t solaros_dlp_connection_exit(size_t count, const mp_obj_t *args)
{
    (void)count;
    return solaros_dlp_connection_close(args[0]);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_dlp_connection_exit_obj, 4, 4, solaros_dlp_connection_exit);
static const mp_rom_map_elem_t solaros_dlp_connection_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&solaros_dlp_connection_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_present), MP_ROM_PTR(&solaros_dlp_connection_present_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&solaros_dlp_connection_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&solaros_dlp_connection_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&solaros_dlp_connection_enter_obj)},
    {MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&solaros_dlp_connection_exit_obj)},
};
static MP_DEFINE_CONST_DICT(solaros_dlp_connection_locals, solaros_dlp_connection_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(solaros_dlp_connection_type, MP_QSTR_dlp_connection,
    MP_TYPE_FLAG_NONE, locals_dict, &solaros_dlp_connection_locals);

static mp_obj_t solaros_dlp_connect(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    mp_arg_check_num(count, kwargs ? kwargs->used : 0, 0, 3, true);
    python_check_known_kwargs(kwargs, "stream", "format", "timeout_ms", NULL);
    mp_obj_t values[3] = {MP_OBJ_NULL, MP_OBJ_NULL, MP_OBJ_NULL};
    const char *names[] = {"stream", "format", "timeout_ms"};
    for (unsigned i = 0; i < 3; i++) {
        values[i] = python_kw_value(kwargs, names[i]);
        if (i < count) {
            if (values[i] != MP_OBJ_NULL) mp_raise_ValueError(MP_ERROR_TEXT("duplicate DLP argument"));
            values[i] = args[i];
        }
    }
    if (values[0] == MP_OBJ_NULL) mp_raise_ValueError(MP_ERROR_TEXT("stream is required"));
    size_t length;
    const char *stream = mp_obj_str_get_data(values[0], &length);
    if (!length || length >= SOLAR_OS_STREAM_ID_MAX || memchr(stream, 0, length))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid stream name"));
    solar_os_display_link_options_t options = {.format = SDLP_MONO1, .timeout_ms = 5000,
        .cancelled = python_should_cancel};
    if (values[1] != MP_OBJ_NULL) options.format = python_i32_from_obj(values[1]);
    if (values[2] != MP_OBJ_NULL) options.timeout_ms = python_u32_from_obj(values[2]);
    if (options.timeout_ms < 100 || options.timeout_ms > 60000)
        mp_raise_ValueError(MP_ERROR_TEXT("timeout_ms must be 100..60000"));
    unsigned slot;
    for (slot = 0; slot < 4 && python_runtime_state->dlp_links[slot]; slot++) {}
    if (slot == 4) mp_raise_ValueError(MP_ERROR_TEXT("too many DLP connections"));
    python_dlp_t *self = mp_obj_malloc_with_finaliser(python_dlp_t, &solaros_dlp_connection_type);
    self->slot = 4; self->generation = 0;
    solar_os_display_link_t *link = NULL;
    python_check_esp(solar_os_display_link_open_stream(stream, "python-dlp", &options, &link));
    python_runtime_state->dlp_links[slot] = link;
    if (!++python_runtime_state->dlp_generation[slot]) ++python_runtime_state->dlp_generation[slot];
    self->slot = slot; self->generation = python_runtime_state->dlp_generation[slot];
    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_dlp_connect_obj, 0, solaros_dlp_connect);
#else
static void python_dlp_destroy(void) {}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE_STREAM
static mp_obj_t solaros_streams_ble_connect(size_t n_args, const mp_obj_t *args)
{
    size_t len;
    const char *address = mp_obj_str_get_data(args[1], &len);
    uint8_t bda[6];
    if (!solar_os_ble_parse_address(address, len, bda))
        mp_raise_ValueError(MP_ERROR_TEXT("expected aa:bb:cc:dd:ee:ff"));
    uint32_t type = n_args > 2 ? python_u32_from_obj(args[2]) : 0;
    if (type > 3) mp_raise_ValueError(MP_ERROR_TEXT("address type must be 0..3"));
    python_check_esp(solar_os_ble_stream_connect(mp_obj_str_get_str(args[0]), bda, type));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_streams_ble_connect_obj, 2, 3, solaros_streams_ble_connect);
static mp_obj_t solaros_streams_ble_listen(mp_obj_t name)
{
    python_check_esp(solar_os_ble_stream_listen(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_ble_listen_obj, solaros_streams_ble_listen);
static mp_obj_t solaros_streams_ble_remove(mp_obj_t name)
{
    python_check_esp(solar_os_ble_stream_remove(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_ble_remove_obj, solaros_streams_ble_remove);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_TCP_STREAM
static mp_obj_t solaros_streams_tcp_connect(mp_obj_t name, mp_obj_t host, mp_obj_t port)
{
    uint32_t number = python_u32_from_obj(port);
    if (!number || number > UINT16_MAX) mp_raise_ValueError(MP_ERROR_TEXT("invalid port"));
    python_check_esp(solar_os_tcp_stream_connect(mp_obj_str_get_str(name), mp_obj_str_get_str(host), number));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_3(solaros_streams_tcp_connect_obj, solaros_streams_tcp_connect);
static mp_obj_t solaros_streams_tcp_listen(mp_obj_t name, mp_obj_t port)
{
    uint32_t number = python_u32_from_obj(port);
    if (!number || number > UINT16_MAX) mp_raise_ValueError(MP_ERROR_TEXT("invalid port"));
    python_check_esp(solar_os_tcp_stream_listen(mp_obj_str_get_str(name), number));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_streams_tcp_listen_obj, solaros_streams_tcp_listen);
static mp_obj_t solaros_streams_tcp_remove(mp_obj_t name)
{
    python_check_esp(solar_os_tcp_stream_remove(mp_obj_str_get_str(name)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_streams_tcp_remove_obj, solaros_streams_tcp_remove);
#endif
